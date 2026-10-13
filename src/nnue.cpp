/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * nnue.cpp: see nnue.h.
 *
 * Accumulators live in a global array indexed by the position's stack index
 * (Position::sp). Each entry remembers the serial of the state it was built
 * for; when evaluating, we walk back to the nearest valid ancestor (or
 * refresh from scratch) and replay the changed pieces recorded by make().
 */
#include "nnue.h"
#include "movegen.h"
#include "perthread.h"
#include <cstring>
#include <fstream>
#include <vector>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

#if defined(AXIORA_EMBED)
#define AX_STR2(x) #x
#define AX_STR(x) AX_STR2(x)
#if defined(_WIN32)  // PE/COFF assembler: no .previous, read-only data is .rdata
#define AX_RODATA ".section .rdata,\"dr\"\n"
#define AX_PREV ".text\n"
#else
#define AX_RODATA ".section .rodata\n"
#define AX_PREV ".previous\n"
#endif
asm(AX_RODATA
    ".balign 64\n"
    ".global axioraNetData\n"
    "axioraNetData:\n"
    ".incbin \"" AX_STR(AXIORA_EMBED) "\"\n"
    ".global axioraNetEnd\n"
    "axioraNetEnd:\n"
    ".byte 0\n"
    AX_PREV);
extern "C" const unsigned char axioraNetData[];
extern "C" const unsigned char axioraNetEnd[];
#endif

namespace NNUE {

bool enabled = true;

struct alignas(64) Accumulator {
  int16_t v[2][MAX_H];  // [perspective WHITE/BLACK]
};

static int H = 0, QA = 255, QB = 64, SCALE = 400, ACT = 1, NB = 1;
static std::vector<int16_t> W1;  // [768][H]
alignas(64) static int16_t B1[MAX_H];
alignas(64) static int16_t W2[MAX_NB][2 * MAX_H];
static int32_t B2[MAX_NB] = {};
static bool isLoaded = false;
static std::string name = "none";

// Accumulator stack per search thread, indexed like Position::st and
// validated by the state's serial number.
struct AccStack {
  Accumulator accs[STACK_SIZE];
  uint64_t serial[STACK_SIZE] = {};
};
static PerThread<AccStack> stacks;

bool loaded() { return isLoaded; }
int hidden() { return H; }
std::string netName() { return name; }

static bool parse(const unsigned char *d, size_t n) {
  if (n < 28) return false;
  const bool v3 = std::memcmp(d, "AXN3", 4) == 0;
  if (!v3 && std::memcmp(d, "AXN2", 4) != 0) return false;
  uint32_t hdr[7] = {};
  const size_t hsz = v3 ? 32 : 28;
  if (n < hsz) return false;
  std::memcpy(hdr, d + 4, hsz - 4);
  if (hdr[0] != (v3 ? 3u : 2u) || hdr[1] == 0 || hdr[1] > MAX_H || hdr[1] % 16) return false;
  int h = int(hdr[1]), nb = v3 ? int(hdr[6]) : 1;
  if (nb < 1 || nb > MAX_NB) return false;
  size_t need = hsz + size_t(768) * h * 2 + h * 2 + size_t(nb) * 2 * h * 2 + size_t(nb) * 4;
  if (n < need) return false;
  H = h; NB = nb; QA = int(hdr[2]); QB = int(hdr[3]); SCALE = int(hdr[4]); ACT = int(hdr[5]);
  if (NB > 1 && ACT != 1) return false;
  const unsigned char *p = d + hsz;
  W1.assign(size_t(768) * H, 0);
  std::memcpy(W1.data(), p, size_t(768) * H * 2); p += size_t(768) * H * 2;
  std::memcpy(B1, p, H * 2); p += H * 2;
  for (int b = 0; b < NB; b++) { std::memcpy(W2[b], p, 2 * H * 2); p += 2 * H * 2; }
  std::memcpy(B2, p, size_t(NB) * 4);
  stacks.forEach([](AccStack &a) { std::memset(a.serial, 0, sizeof(a.serial)); });
  isLoaded = true;
  return true;
}

bool loadFile(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (!parse(buf.data(), buf.size())) return false;
  name = path;
  return true;
}

bool loadEmbedded() {
#if defined(AXIORA_EMBED)
  if (!parse(axioraNetData, size_t(axioraNetEnd - axioraNetData))) return false;
  name = "<embedded>";
  return true;
#else
  return false;
#endif
}

// Feature index of piece pc on sq, seen from perspective p (own pieces first,
// board flipped for Black).
static inline int feature(int p, int pc, int sq) {
  int c = colorOf(pc), t = typeOf(pc);
  if (p == WHITE) return (c == WHITE ? 0 : 384) + (t - 1) * 64 + sq;
  return (c == BLACK ? 0 : 384) + (t - 1) * 64 + (sq ^ 56);
}

static inline void addRow(int16_t *a, int f) {
  const int16_t *w = &W1[size_t(f) * H];
  for (int i = 0; i < H; i++) a[i] += w[i];
}
static inline void subRow(int16_t *a, int f) {
  const int16_t *w = &W1[size_t(f) * H];
  for (int i = 0; i < H; i++) a[i] -= w[i];
}

static void refresh(const Position &pos, Accumulator &acc) {
  for (int p = 0; p < 2; p++) {
    std::memcpy(acc.v[p], B1, H * 2);
    Bitboard b = pos.all;
    while (b) {
      int sq = popLsb(b);
      addRow(acc.v[p], feature(p, pos.board[sq], sq));
    }
  }
}

static void applyDirty(const StateInfo &st, const Accumulator &prev, Accumulator &acc) {
  for (int p = 0; p < 2; p++) {
    std::memcpy(acc.v[p], prev.v[p], H * 2);
    for (int k = 0; k < st.dirtyN; k++) {
      int pc = st.dpc[k];
      if (st.dfrom[k] != NO_SQ) subRow(acc.v[p], feature(p, pc, st.dfrom[k]));
      if (st.dto[k] != NO_SQ) addRow(acc.v[p], feature(p, pc, st.dto[k]));
    }
  }
}

static const Accumulator &update(const Position &pos) {
  const int s = pos.sp;
  AccStack &as = stacks.get();
  Accumulator *accs = as.accs;
  uint64_t *accSerial = as.serial;
  if (accSerial[s] == pos.st[s].serial) return accs[s];
  // Find the nearest ancestor with a valid accumulator (at most 32 back).
  int k = s - 1;
  const int lim = std::max(0, s - 32);
  while (k >= lim && accSerial[k] != pos.st[k].serial) k--;
  if (k < lim) {
    refresh(pos, accs[s]);
  } else {
    // The board is at state s; states k+1..s carry their changed pieces.
    for (int j = k + 1; j <= s; j++) {
      applyDirty(pos.st[j], accs[j - 1], accs[j]);
      accSerial[j] = pos.st[j].serial;
    }
  }
  accSerial[s] = pos.st[s].serial;
  return accs[s];
}

static inline int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}

static int output(const Accumulator &acc, int stm, int pieces) {
  const int16_t *us = acc.v[stm], *them = acc.v[stm ^ 1];
  const int bk = NB == 1 ? 0 : std::min(NB - 1, std::max(0, (pieces - 2) / 4));
  const int16_t *W2b = W2[bk];
  const int32_t B2b = B2[bk];
  int64_t sum = 0;
  if (ACT == 1) {
    // Squared clipped ReLU: sum of (v*v*w) >> 8 per neuron (fits int32), then * 256.
#if defined(__AVX2__)
    const __m256i zero = _mm256_setzero_si256(), qa = _mm256_set1_epi32(QA);
    __m256i s32 = _mm256_setzero_si256();
    for (int half = 0; half < 2; half++) {
      const int16_t *a = half ? them : us;
      const int16_t *w = W2b + half * H;
      for (int i = 0; i < H; i += 8) {
        __m256i v = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i *)(a + i)));
        v = _mm256_min_epi32(_mm256_max_epi32(v, zero), qa);
        __m256i ww = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i *)(w + i)));
        __m256i t = _mm256_mullo_epi32(_mm256_mullo_epi32(v, v), ww);
        s32 = _mm256_add_epi32(s32, _mm256_srai_epi32(t, 8));
      }
    }
    alignas(32) int32_t lanes[8];
    _mm256_store_si256((__m256i *)lanes, s32);
    for (int i = 0; i < 8; i++) sum += lanes[i];
#else
    for (int i = 0; i < H; i++) {
      int v = std::min(std::max<int>(us[i], 0), QA), u = std::min(std::max<int>(them[i], 0), QA);
      sum += (v * v * W2b[i]) >> 8;
      sum += (u * u * W2b[H + i]) >> 8;
    }
#endif
    return int(floorDiv((sum * 256 + B2b) * SCALE, int64_t(QA) * QA * QB));
  }
  for (int i = 0; i < H; i++) {
    int v = std::min(std::max<int>(us[i], 0), QA), u = std::min(std::max<int>(them[i], 0), QA);
    sum += int64_t(v) * W2b[i] + int64_t(u) * W2b[H + i];
  }
  return int(floorDiv((sum + B2b) * SCALE, int64_t(QA) * QB));
}

int evaluate(const Position &pos) { return output(update(pos), pos.side, popcount(pos.all)); }

int evaluateRefresh(const Position &pos) {
  static Accumulator tmp;
  refresh(pos, tmp);
  return output(tmp, pos.side, popcount(pos.all));
}

static bool everyNode = true;
static void verifyRec(Position &pos, int depth, uint64_t &bad, uint64_t &checked) {
  if (everyNode || depth == 0) {
    checked++;
    if (evaluate(pos) != evaluateRefresh(pos)) bad++;
  }
  if (depth == 0) return;
  MoveList ml;
  generate(pos, ml, false);
  for (int i = 0; i < ml.n; i++) {
    if (!pos.legal(ml.m[i])) continue;
    pos.make(ml.m[i]);
    verifyRec(pos, depth - 1, bad, checked);
    pos.unmake(ml.m[i]);
  }
  // Null move too (accumulator must simply be copied).
  if (everyNode && !pos.inCheck()) {
    pos.makeNull();
    checked++;
    if (evaluate(pos) != evaluateRefresh(pos)) bad++;
    pos.unmakeNull();
  }
}

uint64_t verify(Position &pos, int depth, uint64_t &checked, bool leavesOnly) {
  everyNode = !leavesOnly;
  evaluate(pos);  // root accumulator, so leaves replay several moves
  uint64_t bad = 0;
  checked = 0;
  verifyRec(pos, depth, bad, checked);
  return bad;
}

}  // namespace NNUE
