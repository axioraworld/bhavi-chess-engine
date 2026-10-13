/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * position.cpp: see position.h.
 */
#include "position.h"
#include <atomic>
#include "eval.h"
#include "movegen.h"
#include <algorithm>
#include <cstring>
#include <sstream>

namespace Zobrist {
uint64_t psq[16][64], castle[16], epFile[8], sideKey;
void init() {
  uint64_t x = 0x2545F4914F6CDD1DULL;
  auto next = [&]() {  // splitmix64
    x += 0x9E3779B97F4A7C15ULL;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  };
  for (auto &p : psq)
    for (auto &v : p) v = next();
  for (auto &v : castle) v = next();
  for (auto &v : epFile) v = next();
  sideKey = next();
}
}  // namespace Zobrist

// Castling rights kept after a move touching this square.
static int castleMask[64];
static struct CastleInit {
  CastleInit() {
    for (int i = 0; i < 64; i++) castleMask[i] = 15;
    castleMask[4] = 15 & ~(WK | WQ);
    castleMask[0] = 15 & ~WQ;
    castleMask[7] = 15 & ~WK;
    castleMask[60] = 15 & ~(BK | BQ);
    castleMask[56] = 15 & ~BQ;
    castleMask[63] = 15 & ~BK;
  }
} castleInit;

// State serial numbers must be unique across threads (each thread keeps its
// own NNUE accumulators keyed by serial): each thread counts in its own
// range, the top 16 bits holding a per-thread id.
static std::atomic<uint64_t> threadIds{0};
static thread_local uint64_t serialCounter = 0;
uint64_t nextSerial() {
  if (!serialCounter) serialCounter = (threadIds.fetch_add(1) + 1) << 48;
  return ++serialCounter;
}

static inline void addDirty(StateInfo &n, int pc, int from, int to) {
  if (n.dirtyN >= 4) return;  // only during setFen (state is refreshed anyway)
  n.dpc[n.dirtyN] = uint8_t(pc);
  n.dfrom[n.dirtyN] = uint8_t(from);
  n.dto[n.dirtyN] = uint8_t(to);
  n.dirtyN++;
}

void Position::put(int pc, int sq) {
  board[sq] = pc;
  pcs[pc] |= bit(sq);
  occ[colorOf(pc)] |= bit(sq);
  all |= bit(sq);
}
void Position::remove(int sq) {
  int pc = board[sq];
  board[sq] = NO_PIECE;
  pcs[pc] &= ~bit(sq);
  occ[colorOf(pc)] &= ~bit(sq);
  all &= ~bit(sq);
}
void Position::moveP(int from, int to) {
  int pc = board[from];
  Bitboard ft = bit(from) | bit(to);
  board[to] = pc;
  board[from] = NO_PIECE;
  pcs[pc] ^= ft;
  occ[colorOf(pc)] ^= ft;
  all ^= ft;
}
void Position::putH(StateInfo &n, int pc, int sq) {
  put(pc, sq);
  addDirty(n, pc, NO_SQ, sq);
  n.key ^= Zobrist::psq[pc][sq];
  if (typeOf(pc) == PAWN) n.pawnKey ^= Zobrist::psq[pc][sq];
  n.psqMg += Eval::PSQ_MG[pc][sq];
  n.psqEg += Eval::PSQ_EG[pc][sq];
  n.phase += Eval::PHASE_W[typeOf(pc)];
}
void Position::removeH(StateInfo &n, int sq) {
  int pc = board[sq];
  remove(sq);
  addDirty(n, pc, sq, NO_SQ);
  n.key ^= Zobrist::psq[pc][sq];
  if (typeOf(pc) == PAWN) n.pawnKey ^= Zobrist::psq[pc][sq];
  n.psqMg -= Eval::PSQ_MG[pc][sq];
  n.psqEg -= Eval::PSQ_EG[pc][sq];
  n.phase -= Eval::PHASE_W[typeOf(pc)];
}
void Position::moveH(StateInfo &n, int from, int to) {
  int pc = board[from];
  moveP(from, to);
  addDirty(n, pc, from, to);
  uint64_t k = Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];
  n.key ^= k;
  if (typeOf(pc) == PAWN) n.pawnKey ^= k;
  n.psqMg += Eval::PSQ_MG[pc][to] - Eval::PSQ_MG[pc][from];
  n.psqEg += Eval::PSQ_EG[pc][to] - Eval::PSQ_EG[pc][from];
}

Bitboard Position::attackersTo(int sq, Bitboard o) const {
  return (PawnAtt[BLACK][sq] & pcs[makePiece(WHITE, PAWN)]) |
         (PawnAtt[WHITE][sq] & pcs[makePiece(BLACK, PAWN)]) |
         (KnightAtt[sq] & (pcs[makePiece(WHITE, KNIGHT)] | pcs[makePiece(BLACK, KNIGHT)])) |
         (KingAtt[sq] & (pcs[makePiece(WHITE, KING)] | pcs[makePiece(BLACK, KING)])) |
         (bishopAtt(sq, o) & (pcs[makePiece(WHITE, BISHOP)] | pcs[makePiece(BLACK, BISHOP)] |
                              pcs[makePiece(WHITE, QUEEN)] | pcs[makePiece(BLACK, QUEEN)])) |
         (rookAtt(sq, o) & (pcs[makePiece(WHITE, ROOK)] | pcs[makePiece(BLACK, ROOK)] |
                            pcs[makePiece(WHITE, QUEEN)] | pcs[makePiece(BLACK, QUEEN)]));
}

bool Position::attacked(int sq, int by, Bitboard o) const {
  if (PawnAtt[by ^ 1][sq] & pieces(by, PAWN)) return true;
  if (KnightAtt[sq] & pieces(by, KNIGHT)) return true;
  if (KingAtt[sq] & pieces(by, KING)) return true;
  Bitboard q = pieces(by, QUEEN);
  if (bishopAtt(sq, o) & (pieces(by, BISHOP) | q)) return true;
  if (rookAtt(sq, o) & (pieces(by, ROOK) | q)) return true;
  return false;
}

void Position::updateCheckInfo() {
  StateInfo &n = st[sp];
  int us = side, them = side ^ 1;
  int k = king(us);
  n.checkers = attackersTo(k, all) & occ[them];
  n.pinned = 0;
  Bitboard snipers = (rookAtt(k, 0) & (pieces(them, ROOK) | pieces(them, QUEEN))) |
                     (bishopAtt(k, 0) & (pieces(them, BISHOP) | pieces(them, QUEEN)));
  while (snipers) {
    int s = popLsb(snipers);
    Bitboard b = BetweenBB[k][s] & all;
    if (b && !moreThanOne(b) && (b & occ[us])) n.pinned |= b;
  }
}

bool Position::setFen(const std::string &fen) {
  std::memset(board, 0, sizeof(board));
  std::memset(pcs, 0, sizeof(pcs));
  occ[0] = occ[1] = all = 0;
  sp = 0;
  StateInfo &n = st[0];
  std::memset(&n, 0, sizeof(n));
  std::istringstream is(fen);
  std::string placement, stm, cas, eps;
  int half = 0, full = 1;
  if (!(is >> placement >> stm)) return false;
  if (!(is >> cas)) cas = "-";
  if (!(is >> eps)) eps = "-";
  is >> half >> full;
  int r = 7, f = 0;
  for (char c : placement) {
    if (c == '/') { r--; f = 0; continue; }
    if (c >= '1' && c <= '8') { f += c - '0'; continue; }
    const char *pcsStr = "PNBRQKpnbrqk";
    const char *p = std::strchr(pcsStr, c);
    if (!p || f > 7 || r < 0) return false;
    int idx = int(p - pcsStr);
    int pc = makePiece(idx >= 6 ? BLACK : WHITE, (idx % 6) + 1);
    putH(n, pc, r * 8 + f);
    f++;
  }
  if (popcount(pieces(WHITE, KING)) != 1 || popcount(pieces(BLACK, KING)) != 1) return false;
  side = stm == "b" ? BLACK : WHITE;
  if (side == BLACK) n.key ^= Zobrist::sideKey;
  n.castle = 0;
  for (char c : cas) {
    if (c == 'K' && board[4] == makePiece(WHITE, KING) && board[7] == makePiece(WHITE, ROOK)) n.castle |= WK;
    if (c == 'Q' && board[4] == makePiece(WHITE, KING) && board[0] == makePiece(WHITE, ROOK)) n.castle |= WQ;
    if (c == 'k' && board[60] == makePiece(BLACK, KING) && board[63] == makePiece(BLACK, ROOK)) n.castle |= BK;
    if (c == 'q' && board[60] == makePiece(BLACK, KING) && board[56] == makePiece(BLACK, ROOK)) n.castle |= BQ;
  }
  n.key ^= Zobrist::castle[n.castle];
  n.ep = NO_SQ;
  if (eps.size() == 2 && eps[0] >= 'a' && eps[0] <= 'h') {
    int e = (eps[1] - '1') * 8 + (eps[0] - 'a');
    // Only keep an en-passant square if a capture is actually possible.
    if (e >= 0 && e < 64 && (PawnAtt[side ^ 1][e] & pieces(side, PAWN))) {
      n.ep = e;
      n.key ^= Zobrist::epFile[fileOf(e)];
    }
  }
  n.half = half;
  n.captured = NO_PIECE;
  n.move = NO_MOVE;
  n.dirtyN = 0;
  n.serial = nextSerial();
  updateCheckInfo();
  // Side not to move must not be in check.
  if (attacked(king(side ^ 1), side)) return false;
  return true;
}

std::string Position::fen() const {
  std::string r;
  for (int rk = 7; rk >= 0; rk--) {
    int empty = 0;
    for (int f = 0; f < 8; f++) {
      int pc = board[rk * 8 + f];
      if (!pc) { empty++; continue; }
      if (empty) { r += char('0' + empty); empty = 0; }
      char c = " PNBRQK"[typeOf(pc)];
      r += colorOf(pc) == BLACK ? char(c + 32) : c;
    }
    if (empty) r += char('0' + empty);
    if (rk) r += '/';
  }
  r += side == WHITE ? " w " : " b ";
  int c = s().castle;
  if (!c) r += '-';
  if (c & WK) r += 'K';
  if (c & WQ) r += 'Q';
  if (c & BK) r += 'k';
  if (c & BQ) r += 'q';
  r += ' ';
  r += s().ep == NO_SQ ? "-" : sqName(s().ep);
  r += ' ' + std::to_string(s().half) + " 1";
  return r;
}

void Position::make(Move m) {
  const StateInfo &o = st[sp];
  StateInfo &n = st[sp + 1];
  n.key = o.key ^ Zobrist::sideKey;
  n.pawnKey = o.pawnKey;
  n.psqMg = o.psqMg;
  n.psqEg = o.psqEg;
  n.phase = o.phase;
  n.half = o.half + 1;
  n.move = m;
  n.captured = NO_PIECE;
  n.ep = NO_SQ;
  n.dirtyN = 0;
  n.serial = nextSerial();
  if (o.ep != NO_SQ) n.key ^= Zobrist::epFile[fileOf(o.ep)];
  int from = mFrom(m), to = mTo(m), flag = mFlag(m);
  int us = side, them = side ^ 1;
  int pc = board[from];
  if (flag == F_CASTLE) {
    int rf = to > from ? from + 3 : from - 4;
    int rt = to > from ? from + 1 : from - 1;
    moveH(n, from, to);
    moveH(n, rf, rt);
  } else {
    if (flag == F_EP) {
      int cs = to + (us == WHITE ? -8 : 8);
      n.captured = board[cs];
      removeH(n, cs);
      n.half = 0;
    } else if (isCapture(m)) {
      n.captured = board[to];
      removeH(n, to);
      n.half = 0;
    }
    moveH(n, from, to);
    if (typeOf(pc) == PAWN) {
      n.half = 0;
      if (isPromo(m)) {
        removeH(n, to);
        putH(n, makePiece(us, promoType(m)), to);
      } else if (flag == F_DOUBLE) {
        int e = (from + to) >> 1;
        if (PawnAtt[us][e] & pieces(them, PAWN)) {
          n.ep = e;
          n.key ^= Zobrist::epFile[fileOf(e)];
        }
      }
    }
  }
  n.castle = o.castle & castleMask[from] & castleMask[to];
  if (n.castle != o.castle) n.key ^= Zobrist::castle[o.castle] ^ Zobrist::castle[n.castle];
  side = them;
  sp++;
  updateCheckInfo();
}

void Position::unmake(Move m) {
  int from = mFrom(m), to = mTo(m), flag = mFlag(m);
  int captured = st[sp].captured;
  sp--;
  side ^= 1;
  int us = side;
  if (flag == F_CASTLE) {
    int rf = to > from ? from + 3 : from - 4;
    int rt = to > from ? from + 1 : from - 1;
    moveP(to, from);
    moveP(rt, rf);
    return;
  }
  if (isPromo(m)) {
    remove(to);
    put(makePiece(us, PAWN), to);
  }
  moveP(to, from);
  if (flag == F_EP) put(captured, to + (us == WHITE ? -8 : 8));
  else if (captured) put(captured, to);
}

void Position::makeNull() {
  const StateInfo &o = st[sp];
  StateInfo &n = st[sp + 1];
  n = o;
  n.key ^= Zobrist::sideKey;
  if (o.ep != NO_SQ) n.key ^= Zobrist::epFile[fileOf(o.ep)];
  n.ep = NO_SQ;
  n.half = 0;
  n.move = NO_MOVE;
  n.captured = NO_PIECE;
  n.dirtyN = 0;
  n.serial = nextSerial();
  side ^= 1;
  sp++;
  updateCheckInfo();
}

void Position::unmakeNull() {
  sp--;
  side ^= 1;
}

bool Position::legal(Move m) const {
  int from = mFrom(m), to = mTo(m);
  int us = side, them = side ^ 1;
  int k = king(us);
  const StateInfo &n = st[sp];
  if (from == k) {
    if (mFlag(m) == F_CASTLE) return true;  // path checked in generation
    return !attacked(to, them, all ^ bit(from));
  }
  if (mFlag(m) == F_EP) {
    int cs = to + (us == WHITE ? -8 : 8);
    Bitboard o = (all ^ bit(from) ^ bit(cs)) | bit(to);
    Bitboard a = (bishopAtt(k, o) & (pieces(them, BISHOP) | pieces(them, QUEEN))) |
                 (rookAtt(k, o) & (pieces(them, ROOK) | pieces(them, QUEEN))) |
                 (KnightAtt[k] & pieces(them, KNIGHT)) |
                 (PawnAtt[us][k] & pieces(them, PAWN) & ~bit(cs));
    return a == 0;
  }
  if (n.checkers) {
    if (moreThanOne(n.checkers)) return false;
    int c = lsb(n.checkers);
    if (!((BetweenBB[k][c] | n.checkers) & bit(to))) return false;
  }
  if ((n.pinned & bit(from)) && !(LineBB[k][from] & bit(to))) return false;
  return true;
}

bool Position::isRepetition() const {
  const uint64_t k = st[sp].key;
  int lim = sp - st[sp].half;
  if (lim < 0) lim = 0;
  for (int i = sp - 2; i >= lim; i -= 2)
    if (st[i].key == k) return true;
  return false;
}

bool Position::hasNonPawn(int c) const {
  return (occ[c] & ~pieces(c, PAWN) & ~pieces(c, KING)) != 0;
}

// Static exchange: plays captures on the target square, cheapest attacker
// first (x-ray sliders appear as pieces are removed), returns material gain.
int Position::see(Move m) const {
  static const int V[7] = {0, 100, 320, 330, 500, 900, 20000};
  int from = mFrom(m), to = mTo(m);
  int gain[40];
  int d = 0;
  Bitboard o = all;
  int victim = mFlag(m) == F_EP ? PAWN : typeOf(board[to]);
  gain[0] = V[victim];
  int attackerVal = V[typeOf(board[from])];
  o ^= bit(from);
  if (mFlag(m) == F_EP) o ^= bit(to + (side == WHITE ? -8 : 8));
  Bitboard diag = pieces(WHITE, BISHOP) | pieces(BLACK, BISHOP) | pieces(WHITE, QUEEN) | pieces(BLACK, QUEEN);
  Bitboard orth = pieces(WHITE, ROOK) | pieces(BLACK, ROOK) | pieces(WHITE, QUEEN) | pieces(BLACK, QUEEN);
  Bitboard att = attackersTo(to, o) & o;
  int stm = side ^ 1;
  while (d < 38) {
    d++;
    gain[d] = attackerVal - gain[d - 1];
    if (std::max(-gain[d - 1], gain[d]) < 0) break;
    Bitboard mine = att & occ[stm];
    if (!mine) break;
    int t, s = -1;
    for (t = PAWN; t <= KING; t++) {
      Bitboard b = mine & pieces(stm, t);
      if (b) { s = lsb(b); break; }
    }
    attackerVal = V[t];
    o ^= bit(s);
    att |= (bishopAtt(to, o) & diag) | (rookAtt(to, o) & orth);
    att &= o;
    stm ^= 1;
  }
  while (--d) gain[d - 1] = -std::max(-gain[d - 1], gain[d]);
  return gain[0];
}

Move Position::parseUci(const std::string &u) const {
  MoveList ml;
  generate(*this, ml, false);
  for (int i = 0; i < ml.n; i++)
    if (moveToUci(ml.m[i]) == u && legal(ml.m[i])) return ml.m[i];
  return NO_MOVE;
}

void Position::rebase() {
  // Keep the last (half + 2) states so repetition checks stay correct.
  if (sp < STACK_SIZE / 2) return;
  int keep = st[sp].half + 2;
  if (keep > sp) keep = sp;
  int start = sp - keep;
  std::memmove(&st[0], &st[start], sizeof(StateInfo) * (keep + 1));
  sp = keep;
}
