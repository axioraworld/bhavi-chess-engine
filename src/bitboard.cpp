/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * bitboard.cpp: attack tables. Sliding attacks use magic multiplication;
 * the magic numbers are searched at start-up by trial (fixed seed), so no
 * tables are taken from elsewhere.
 */
#include "core.h"
#include <cstdlib>
#include <vector>

Bitboard PawnAtt[2][64], KnightAtt[64], KingAtt[64];
Bitboard BetweenBB[64][64], LineBB[64][64];
Magic BishopMagic[64], RookMagic[64];
static Bitboard bishopTable[5248];
static Bitboard rookTable[102400];

static const int ROOK_DIR[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
static const int BISHOP_DIR[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

// Walk rays square by square (only used to build tables).
static Bitboard slowAttacks(int s, Bitboard occ, const int dir[4][2]) {
  Bitboard a = 0;
  for (int d = 0; d < 4; d++) {
    int f = fileOf(s) + dir[d][0], r = rankOf(s) + dir[d][1];
    while (f >= 0 && f < 8 && r >= 0 && r < 8) {
      int t = r * 8 + f;
      a |= bit(t);
      if (occ & bit(t)) break;
      f += dir[d][0];
      r += dir[d][1];
    }
  }
  return a;
}

// Squares whose occupancy matters (rays without the final edge square).
static Bitboard relevantMask(int s, const int dir[4][2]) {
  Bitboard m = 0;
  for (int d = 0; d < 4; d++) {
    int f = fileOf(s) + dir[d][0], r = rankOf(s) + dir[d][1];
    while (f + dir[d][0] >= 0 && f + dir[d][0] < 8 && r + dir[d][1] >= 0 && r + dir[d][1] < 8) {
      m |= bit(r * 8 + f);
      f += dir[d][0];
      r += dir[d][1];
    }
  }
  return m;
}

static uint64_t rngState = 0x9E3779B97F4A7C15ULL;
static uint64_t rng64() {  // xorshift64*
  rngState ^= rngState >> 12;
  rngState ^= rngState << 25;
  rngState ^= rngState >> 27;
  return rngState * 2685821657736338717ULL;
}

static void buildMagics(Magic *mags, Bitboard *table, const int dir[4][2]) {
  Bitboard *next = table;
  std::vector<Bitboard> occs, atts, used;
  for (int s = 0; s < 64; s++) {
    Magic &m = mags[s];
    m.mask = relevantMask(s, dir);
    int bits = popcount(m.mask);
    m.shift = 64 - bits;
    m.table = next;
    int n = 1 << bits;
    occs.assign(n, 0);
    atts.assign(n, 0);
    Bitboard sub = 0;
    int i = 0;
    do {  // enumerate all subsets of the mask
      occs[i] = sub;
      atts[i] = slowAttacks(s, sub, dir);
      i++;
      sub = (sub - m.mask) & m.mask;
    } while (sub);
    used.assign(n, 0);
    std::vector<int> stamp(n, 0);
    for (int attempt = 1;; attempt++) {
      Bitboard cand = rng64() & rng64() & rng64();
      if (popcount((m.mask * cand) >> 56) < 6) continue;
      bool ok = true;
      for (int k = 0; k < n && ok; k++) {
        unsigned idx = unsigned((occs[k] * cand) >> m.shift);
        if (stamp[idx] != attempt) {
          stamp[idx] = attempt;
          used[idx] = atts[k];
        } else if (used[idx] != atts[k]) {
          ok = false;
        }
      }
      if (ok) {
        m.magic = cand;
        for (int k = 0; k < n; k++) m.table[(occs[k] * cand) >> m.shift] = atts[k];
        break;
      }
    }
    next += n;
  }
}

void initBitboards() {
  for (int s = 0; s < 64; s++) {
    int f = fileOf(s), r = rankOf(s);
    auto add = [&](Bitboard &b, int df, int dr) {
      int ff = f + df, rr = r + dr;
      if (ff >= 0 && ff < 8 && rr >= 0 && rr < 8) b |= bit(rr * 8 + ff);
    };
    PawnAtt[WHITE][s] = PawnAtt[BLACK][s] = KnightAtt[s] = KingAtt[s] = 0;
    add(PawnAtt[WHITE][s], -1, 1); add(PawnAtt[WHITE][s], 1, 1);
    add(PawnAtt[BLACK][s], -1, -1); add(PawnAtt[BLACK][s], 1, -1);
    const int kn[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
    for (auto &d : kn) add(KnightAtt[s], d[0], d[1]);
    for (int df = -1; df <= 1; df++)
      for (int dr = -1; dr <= 1; dr++)
        if (df || dr) add(KingAtt[s], df, dr);
  }
  buildMagics(BishopMagic, bishopTable, BISHOP_DIR);
  buildMagics(RookMagic, rookTable, ROOK_DIR);
  for (int a = 0; a < 64; a++)
    for (int b = 0; b < 64; b++) {
      BetweenBB[a][b] = LineBB[a][b] = 0;
      if (a == b) continue;
      if (bishopAtt(a, 0) & bit(b)) {
        BetweenBB[a][b] = bishopAtt(a, bit(b)) & bishopAtt(b, bit(a));
        LineBB[a][b] = (bishopAtt(a, 0) & bishopAtt(b, 0)) | bit(a) | bit(b);
      } else if (rookAtt(a, 0) & bit(b)) {
        BetweenBB[a][b] = rookAtt(a, bit(b)) & rookAtt(b, bit(a));
        LineBB[a][b] = (rookAtt(a, 0) & rookAtt(b, 0)) | bit(a) | bit(b);
      }
    }
}

std::string sqName(int s) {
  std::string r;
  r += char('a' + fileOf(s));
  r += char('1' + rankOf(s));
  return r;
}

std::string moveToUci(Move m) {
  if (m == NO_MOVE) return "0000";
  std::string r = sqName(mFrom(m)) + sqName(mTo(m));
  if (isPromo(m)) r += "nbrq"[promoType(m) - KNIGHT];
  return r;
}
