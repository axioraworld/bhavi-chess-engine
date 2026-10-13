/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * eval.cpp: middlegame/endgame tapered evaluation. Terms (all from the JS
 * engine v4, written for this project): material and piece-square tables,
 * pawn structure (doubled, isolated, backward, passed, protected and
 * connected passers, king distance to passers, blocked passers), mobility,
 * king-zone attacks, pawn shield and open files near the king, rooks on
 * open/semi-open files and the 7th rank, knight/bishop outposts, bishop
 * pair, early queen sortie, tempo, and draw-ish material scaling.
 */
#include "eval.h"
#include "perthread.h"
#include "nnue.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Eval {

static const int MG_VAL[7] = {0, 85, 330, 345, 480, 990, 0};
static const int EG_VAL[7] = {0, 105, 300, 320, 540, 980, 0};
const int PHASE_W[7] = {0, 0, 1, 1, 2, 4, 0};

// White's view, rank 8 first (as on a diagram). Queen: formula.
static const int PST_SRC_MG[7][64] = {
  {},
  { 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    4, 6, 10, 18, 18, 10, 6, 4,
    2, 4, 8, 16, 16, 8, 4, 2,
    0, 0, 6, 14, 14, 6, 0, 0,
    2, 0, 2, 6, 6, 2, 0, 2,
    2, 4, 4, -8, -8, 4, 4, 2,
    0, 0, 0, 0, 0, 0, 0, 0},
  {-40, -25, -20, -15, -15, -20, -25, -40,
    -25, -10, 0, 5, 5, 0, -10, -25,
    -15, 5, 12, 16, 16, 12, 5, -15,
    -10, 5, 14, 20, 20, 14, 5, -10,
    -10, 2, 12, 18, 18, 12, 2, -10,
    -15, 2, 8, 10, 10, 8, 2, -15,
    -25, -10, 0, 4, 4, 0, -10, -25,
    -40, -22, -18, -15, -15, -18, -22, -40},
  {-15, -8, -8, -8, -8, -8, -8, -15,
    -8, 0, 0, 0, 0, 0, 0, -8,
    -8, 0, 5, 8, 8, 5, 0, -8,
    -8, 5, 5, 8, 8, 5, 5, -8,
    -8, 0, 10, 8, 8, 10, 0, -8,
    -8, 10, 8, 6, 6, 8, 10, -8,
    -8, 12, 0, 2, 2, 0, 12, -8,
    -15, -8, -12, -8, -8, -12, -8, -15},
  { 5, 5, 5, 5, 5, 5, 5, 5,
    4, 6, 6, 6, 6, 6, 6, 4,
    -4, 0, 0, 0, 0, 0, 0, -4,
    -4, 0, 0, 0, 0, 0, 0, -4,
    -4, 0, 0, 0, 0, 0, 0, -4,
    -4, 0, 0, 0, 0, 0, 0, -4,
    -4, 0, 0, 0, 0, 0, 0, -4,
    -2, 0, 2, 6, 6, 4, 0, -2},
  {},
  {-40, -45, -45, -50, -50, -45, -45, -40,
    -40, -45, -45, -50, -50, -45, -45, -40,
    -35, -40, -40, -45, -45, -40, -40, -35,
    -30, -35, -35, -40, -40, -35, -35, -30,
    -20, -25, -25, -30, -30, -25, -25, -20,
    -10, -15, -15, -20, -20, -15, -15, -10,
    10, 10, -5, -10, -10, -5, 10, 10,
    20, 30, 15, -5, 0, -5, 30, 20},
};
static const int EG_CENTRE[7] = {0, 0, 6, 4, 1, 4, 14};
static const int PAWN_EG_RANK[8] = {0, 0, 2, 6, 12, 20, 30, 0};
static const int PASSED_MG[8] = {0, 5, 8, 14, 28, 50, 80, 0};
static const int PASSED_EG[8] = {0, 10, 18, 32, 56, 95, 145, 0};
static const int MOB_MG[7] = {0, 0, 4, 5, 2, 1, 0};
static const int MOB_EG[7] = {0, 0, 4, 5, 4, 2, 0};
static const int MOB_BASE[7] = {0, 0, 4, 6, 6, 12, 0};
static const int KZ_WEIGHT[7] = {0, 0, 2, 2, 3, 5, 0};
static const int NP_VAL[7] = {0, 0, 330, 345, 480, 990, 0};

enum {
  DBL_MG = -10, DBL_EG = -20, ISO_MG = -12, ISO_EG = -15, BWD_MG = -8, BWD_EG = -10,
  PROT_EG_B = 10, PROT_EG_R = 3, CONN_MG_R = 3, CONN_EG_R = 5, BLOCK_EG_R = -4,
  ROPEN_MG = 25, ROPEN_EG = 10, RSEMI_MG = 12, RSEMI_EG = 6, R7_MG = 18, R7_EG = 28,
  NOUT_MG = 22, NOUT_EG = 12, BOUT_MG = 10, BOUT_EG = 5,
  SH_P1 = 12, SH_P2 = 6, SH_NONE = -10, SH_OPEN = -25, SH_SEMI = -15, SH_UP = -20,
  BPAIR_MG = 30, BPAIR_EG = 50, EQ_MG = -8, TEMPO = 10,
};
static const int LAZY_MARGIN = 450;

int PSQ_MG[16][64], PSQ_EG[16][64];
static void initKingZones();

static int jsRound(double x) { return int(std::floor(x + 0.5)); }

void init() {
  initKingZones();
  int pstMg[7][64], pstEg[7][64];
  for (int t = 1; t <= 6; t++)
    for (int r = 0; r < 8; r++)
      for (int f = 0; f < 8; f++) {
        double cd = std::max(std::fabs(f - 3.5), std::fabs(r - 3.5)) - 0.5;
        int i = r * 8 + f;
        pstMg[t][i] = t != QUEEN ? PST_SRC_MG[t][(7 - r) * 8 + f] : jsRound((1.5 - cd) * 3);
        pstEg[t][i] = t == PAWN ? PAWN_EG_RANK[r] : jsRound((1.5 - cd) * EG_CENTRE[t]);
      }
  std::memset(PSQ_MG, 0, sizeof(PSQ_MG));
  std::memset(PSQ_EG, 0, sizeof(PSQ_EG));
  for (int t = 1; t <= 6; t++)
    for (int sq = 0; sq < 64; sq++) {
      PSQ_MG[makePiece(WHITE, t)][sq] = MG_VAL[t] + pstMg[t][sq];
      PSQ_EG[makePiece(WHITE, t)][sq] = EG_VAL[t] + pstEg[t][sq];
      PSQ_MG[makePiece(BLACK, t)][sq] = -(MG_VAL[t] + pstMg[t][sq ^ 56]);
      PSQ_EG[makePiece(BLACK, t)][sq] = -(EG_VAL[t] + pstEg[t][sq ^ 56]);
    }
}

// Caches: full evaluation by position key, pawn terms by pawn key.
struct EvalEntry { uint64_t key; int value; };
struct PawnEntry { uint64_t key; int mg, eg; Bitboard passed; };
static const int EC_SIZE = 1 << 16;
static const int PH_SIZE = 1 << 14;
// One set of caches per search thread.
struct Caches {
  EvalEntry eval[EC_SIZE] = {};
  PawnEntry pawn[PH_SIZE] = {};
};
static PerThread<Caches> caches;

void clearCaches() {
  caches.forEach([](Caches &c) {
    std::memset(c.eval, 0, sizeof(c.eval));
    std::memset(c.pawn, 0, sizeof(c.pawn));
  });
}

static inline int kdist(int a, int b) {
  return std::max(std::abs(fileOf(a) - fileOf(b)), std::abs(rankOf(a) - rankOf(b)));
}

// King zone: files k-1..k+1, ranks k-1..k+2 (forward for that side).
static Bitboard KING_ZONE[2][64];
static void initKingZones() {
  for (int c = 0; c < 2; c++)
    for (int k = 0; k < 64; k++) {
      Bitboard z = 0;
      int f = fileOf(k), r = rankOf(k);
      for (int df = -1; df <= 1; df++)
        for (int dr = -1; dr <= 2; dr++) {
          int ff = f + df, rr = r + (c == WHITE ? dr : -dr);
          if (ff >= 0 && ff < 8 && rr >= 0 && rr < 8) z |= bit(rr * 8 + ff);
        }
      KING_ZONE[c][k] = z;
    }
}

struct FileInfo {
  int wCnt[8], bCnt[8], wMin[8], wMax[8], bMin[8], bMax[8];
};

static int shield(const Position &pos, int k, bool white, const FileInfo &fi) {
  int f = fileOf(k), r = rankOf(k);
  int rr = white ? r : 7 - r;
  if (rr > 1) return SH_UP;
  int v = 0;
  int own = makePiece(white ? WHITE : BLACK, PAWN);
  int step = white ? 8 : -8;
  for (int ff = f - 1; ff <= f + 1; ff++) {
    if (ff < 0 || ff > 7) continue;
    int s1 = r * 8 + ff + step;
    int s2 = s1 + step;
    if (pos.board[s1] == own) v += SH_P1;
    else if (s2 >= 0 && s2 < 64 && pos.board[s2] == own) v += SH_P2;
    else v += SH_NONE;
    int ownCnt = white ? fi.wCnt[ff] : fi.bCnt[ff];
    int oppCnt = white ? fi.bCnt[ff] : fi.wCnt[ff];
    if (ownCnt == 0) v += oppCnt == 0 ? SH_OPEN : SH_SEMI;
  }
  return v;
}

static int earlyQueen(const Position &pos, bool white) {
  int c = white ? WHITE : BLACK;
  int base = white ? 0 : 56;
  if (pos.board[base + 3] == makePiece(c, QUEEN)) return 0;
  int u = 0;
  if (pos.board[base + 1] == makePiece(c, KNIGHT)) u++;
  if (pos.board[base + 6] == makePiece(c, KNIGHT)) u++;
  if (pos.board[base + 2] == makePiece(c, BISHOP)) u++;
  if (pos.board[base + 5] == makePiece(c, BISHOP)) u++;
  return u >= 2 ? u : 0;
}

static int evalImpl(const Position &pos, int lazyAlpha, int lazyBeta, bool &lazyHit) {
  const StateInfo &st = pos.s();
  int mg = st.psqMg, eg = st.psqEg, phase = st.phase;
  const Bitboard wp = pos.pieces(WHITE, PAWN), bp = pos.pieces(BLACK, PAWN);
  const int wPawns = popcount(wp), bPawns = popcount(bp);
  int wNonPawn = 0, bNonPawn = 0;
  for (int t = KNIGHT; t <= QUEEN; t++) {
    wNonPawn += NP_VAL[t] * popcount(pos.pieces(WHITE, t));
    bNonPawn += NP_VAL[t] * popcount(pos.pieces(BLACK, t));
  }
  if (wPawns == 0 && bPawns == 0 && wNonPawn <= 345 && bNonPawn <= 345) return 0;

  if (lazyBeta < INF || lazyAlpha > -INF) {
    int ph = phase > 24 ? 24 : phase;
    int rough = (mg * ph + eg * (24 - ph)) / 24;
    if (pos.side == BLACK) rough = -rough;
    if (rough - LAZY_MARGIN >= lazyBeta || rough + LAZY_MARGIN <= lazyAlpha) {
      lazyHit = true;
      return rough;
    }
  }

  const int wk = pos.king(WHITE), bk = pos.king(BLACK);
  const Bitboard zoneW = KING_ZONE[WHITE][wk], zoneB = KING_ZONE[BLACK][bk];
  const Bitboard pawnAtkW = ((wp & ~FILE_A) << 7) | ((wp & ~FILE_H) << 9);
  const Bitboard pawnAtkB = ((bp & ~FILE_A) >> 9) | ((bp & ~FILE_H) >> 7);

  FileInfo fi;
  for (int f = 0; f < 8; f++) {
    Bitboard w = wp & fileBB(f), b = bp & fileBB(f);
    fi.wCnt[f] = popcount(w);
    fi.bCnt[f] = popcount(b);
    fi.wMin[f] = w ? rankOf(lsb(w)) : 8;
    fi.wMax[f] = w ? rankOf(63 - __builtin_clzll(w)) : -1;
    fi.bMin[f] = b ? rankOf(lsb(b)) : 8;
    fi.bMax[f] = b ? rankOf(63 - __builtin_clzll(b)) : -1;
  }

  // Pawn structure (pawn-only part cached by pawn key).
  PawnEntry &pe = caches.get().pawn[st.pawnKey & (PH_SIZE - 1)];
  Bitboard passed;
  if (pe.key == st.pawnKey && st.pawnKey) {
    mg += pe.mg;
    eg += pe.eg;
    passed = pe.passed;
  } else {
    int pmg = 0, peg = 0;
    passed = 0;
    for (int f = 0; f < 8; f++) {
      int dw = fi.wCnt[f] > 1 ? fi.wCnt[f] - 1 : 0;
      int db = fi.bCnt[f] > 1 ? fi.bCnt[f] - 1 : 0;
      pmg += DBL_MG * (dw - db);
      peg += DBL_EG * (dw - db);
    }
    Bitboard all = wp | bp;
    while (all) {
      int sq = popLsb(all);
      int f = fileOf(sq), r = rankOf(sq);
      bool white = (wp & bit(sq)) != 0;
      int s = white ? 1 : -1;
      int rr = white ? r : 7 - r;
      int left = f > 0 ? f - 1 : -1, right = f < 7 ? f + 1 : -1;
      const int *ownCnt = white ? fi.wCnt : fi.bCnt;
      bool isolated = (left < 0 || ownCnt[left] == 0) && (right < 0 || ownCnt[right] == 0);
      bool backward = false, isPassed = false, connected = false;
      if (white) {
        if (!isolated) {
          bool behindOk = (left < 0 || fi.wCnt[left] == 0 || fi.wMin[left] > r) &&
                          (right < 0 || fi.wCnt[right] == 0 || fi.wMin[right] > r);
          backward = behindOk && r < 6 && (pawnAtkB & bit(sq + 8));
        }
        isPassed = fi.bCnt[f] == 0 || fi.bMax[f] < r;
        if (isPassed && left >= 0 && fi.bCnt[left] && fi.bMax[left] > r) isPassed = false;
        if (isPassed && right >= 0 && fi.bCnt[right] && fi.bMax[right] > r) isPassed = false;
        if (isPassed)
          connected = (left >= 0 && fi.wCnt[left] && std::abs(fi.wMax[left] - r) <= 1) ||
                      (right >= 0 && fi.wCnt[right] && std::abs(fi.wMax[right] - r) <= 1);
      } else {
        if (!isolated) {
          bool behindOk = (left < 0 || fi.bCnt[left] == 0 || fi.bMax[left] < r) &&
                          (right < 0 || fi.bCnt[right] == 0 || fi.bMax[right] < r);
          backward = behindOk && rr < 6 && (pawnAtkW & bit(sq - 8));
        }
        isPassed = fi.wCnt[f] == 0 || fi.wMin[f] > r;
        if (isPassed && left >= 0 && fi.wCnt[left] && fi.wMin[left] < r) isPassed = false;
        if (isPassed && right >= 0 && fi.wCnt[right] && fi.wMin[right] < r) isPassed = false;
        if (isPassed)
          connected = (left >= 0 && fi.bCnt[left] && std::abs(fi.bMin[left] - r) <= 1) ||
                      (right >= 0 && fi.bCnt[right] && std::abs(fi.bMin[right] - r) <= 1);
      }
      if (isolated) { pmg += s * ISO_MG; peg += s * ISO_EG; }
      else if (backward) { pmg += s * BWD_MG; peg += s * BWD_EG; }
      if (isPassed) {
        pmg += s * PASSED_MG[rr];
        peg += s * PASSED_EG[rr];
        if ((white ? pawnAtkW : pawnAtkB) & bit(sq)) peg += s * (PROT_EG_B + rr * PROT_EG_R);
        if (connected) { pmg += s * rr * CONN_MG_R; peg += s * rr * CONN_EG_R; }
        passed |= bit(sq);
      }
    }
    pe.key = st.pawnKey;
    pe.mg = pmg;
    pe.eg = peg;
    pe.passed = passed;
    mg += pmg;
    eg += peg;
  }
  // Passers: king distances and blockers (depend on more than pawns).
  for (Bitboard b = passed; b;) {
    int sq = popLsb(b);
    bool white = (wp & bit(sq)) != 0;
    int rr = white ? rankOf(sq) : 7 - rankOf(sq);
    int front = white ? sq + 8 : sq - 8;
    if (front < 0 || front > 63) continue;
    int kd = white ? ((kdist(bk, front) * 5 - kdist(wk, front) * 2) * rr) >> 2
                   : -(((kdist(wk, front) * 5 - kdist(bk, front) * 2) * rr) >> 2);
    eg += kd;
    if (pos.board[front]) eg += (white ? 1 : -1) * rr * BLOCK_EG_R;
  }

  // Pieces: mobility, king-zone attacks, rooks, outposts.
  int atkUnits[2] = {0, 0}, attackers[2] = {0, 0};
  for (int c = WHITE; c <= BLACK; c++) {
    const bool white = c == WHITE;
    const int s = white ? 1 : -1;
    const Bitboard own = pos.occ[c];
    const Bitboard enemyPawnAtk = white ? pawnAtkB : pawnAtkW;
    const Bitboard ownPawnAtk = white ? pawnAtkW : pawnAtkB;
    const Bitboard enemyZone = white ? zoneB : zoneW;
    for (int t = KNIGHT; t <= QUEEN; t++) {
      Bitboard p = pos.pieces(c, t);
      while (p) {
        int sq = popLsb(p);
        Bitboard a = t == KNIGHT ? KnightAtt[sq]
                   : t == BISHOP ? bishopAtt(sq, pos.all)
                   : t == ROOK   ? rookAtt(sq, pos.all)
                                 : queenAtt(sq, pos.all);
        int zoneHits = popcount(a & enemyZone);
        int mob = popcount(a & ~own & ~enemyPawnAtk);
        int mobc = mob - MOB_BASE[t];
        int pmg = mobc * MOB_MG[t], peg = mobc * MOB_EG[t];
        int f = fileOf(sq), r = rankOf(sq);
        int rr = white ? r : 7 - r;
        if (t == ROOK) {
          int ownC = white ? fi.wCnt[f] : fi.bCnt[f];
          int oppC = white ? fi.bCnt[f] : fi.wCnt[f];
          if (ownC == 0 && oppC == 0) { pmg += ROPEN_MG; peg += ROPEN_EG; }
          else if (ownC == 0) { pmg += RSEMI_MG; peg += RSEMI_EG; }
          if (rr == 6) {
            int ekr = white ? rankOf(bk) : 7 - rankOf(wk);
            bool pawns7 = white ? (bp & rankBB(6)) != 0 : (wp & rankBB(1)) != 0;
            if (ekr == 7 || pawns7) { pmg += R7_MG; peg += R7_EG; }
          }
        } else if ((t == KNIGHT || t == BISHOP) && rr >= 3 && rr <= 5) {
          if (ownPawnAtk & bit(sq)) {
            bool safe = true;
            for (int ff = f - 1; ff <= f + 1; ff += 2) {
              if (ff < 0 || ff > 7) continue;
              if (white ? (fi.bCnt[ff] && fi.bMax[ff] > r) : (fi.wCnt[ff] && fi.wMin[ff] < r)) safe = false;
            }
            if (safe) {
              pmg += t == KNIGHT ? NOUT_MG : BOUT_MG;
              peg += t == KNIGHT ? NOUT_EG : BOUT_EG;
            }
          }
        }
        if (zoneHits) {
          attackers[c]++;
          atkUnits[c] += KZ_WEIGHT[t] + zoneHits;
        }
        mg += s * pmg;
        eg += s * peg;
      }
    }
  }

  // King safety (middlegame): attackers near the king, pawn shield, open files.
  const bool wQueen = pos.pieces(WHITE, QUEEN) != 0, bQueen = pos.pieces(BLACK, QUEEN) != 0;
  if (attackers[WHITE] >= 2) {
    int p = (atkUnits[WHITE] * atkUnits[WHITE]) >> 2;
    if (!wQueen) p >>= 1;
    mg += p > 500 ? 500 : p;
  }
  if (attackers[BLACK] >= 2) {
    int p = (atkUnits[BLACK] * atkUnits[BLACK]) >> 2;
    if (!bQueen) p >>= 1;
    mg -= p > 500 ? 500 : p;
  }
  mg += shield(pos, wk, true, fi) - shield(pos, bk, false, fi);

  if (popcount(pos.pieces(WHITE, BISHOP)) >= 2) { mg += BPAIR_MG; eg += BPAIR_EG; }
  if (popcount(pos.pieces(BLACK, BISHOP)) >= 2) { mg -= BPAIR_MG; eg -= BPAIR_EG; }

  if (wQueen) mg += earlyQueen(pos, true) * EQ_MG;
  if (bQueen) mg -= earlyQueen(pos, false) * EQ_MG;

  mg += pos.side == WHITE ? TEMPO : -TEMPO;

  if (phase > 24) phase = 24;
  int score = (mg * phase + eg * (24 - phase)) / 24;
  if (score > 0 && wPawns == 0 && wNonPawn - bNonPawn <= 345) score >>= 2;
  else if (score < 0 && bPawns == 0 && bNonPawn - wNonPawn <= 345) score = -((-score) >> 2);
  return pos.side == WHITE ? score : -score;
}

int evaluateFull(const Position &pos) {
  bool lazy = false;
  return evalImpl(pos, -INF, INF, lazy);
}

int nnueWeight = 100;

// NNUE value with exact draw knowledge (bare kings / single minor piece).
static int nnueEval(const Position &pos) {
  if (!pos.pieces(WHITE, PAWN) && !pos.pieces(BLACK, PAWN)) {
    int w = 0, b = 0;
    for (int t = KNIGHT; t <= QUEEN; t++) {
      w += NP_VAL[t] * popcount(pos.pieces(WHITE, t));
      b += NP_VAL[t] * popcount(pos.pieces(BLACK, t));
    }
    if (w <= 345 && b <= 345) return 0;
  }
  int v = NNUE::evaluate(pos);
  if (nnueWeight < 100) {  // optional blend with the hand-crafted evaluation
    bool lazy = false;
    v = (v * nnueWeight + evalImpl(pos, -INF, INF, lazy) * (100 - nnueWeight)) / 100;
  }
  return std::max(-MATE_BOUND + 1, std::min(MATE_BOUND - 1, v));
}

int evaluateStatic(const Position &pos) {
  return NNUE::enabled && NNUE::loaded() ? nnueEval(pos) : evaluateFull(pos);
}

int evaluate(const Position &pos, int alpha, int beta) {
  EvalEntry &e = caches.get().eval[pos.key() & (EC_SIZE - 1)];
  if (e.key == pos.key()) return e.value;
  int v;
  if (NNUE::enabled && NNUE::loaded()) {
    v = nnueEval(pos);
  } else {
    bool lazy = false;
    v = evalImpl(pos, alpha, beta, lazy);
    if (lazy) return v;
  }
  e.key = pos.key();
  e.value = v;
  return v;
}

}  // namespace Eval
