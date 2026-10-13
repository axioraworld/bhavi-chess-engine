/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * search.cpp: negamax alpha-beta with principal variation search,
 * transposition table, internal iterative reduction, reverse futility
 * pruning, razoring, null-move pruning, futility and late-move pruning,
 * late move reductions (history-aware), check extension, mate distance
 * pruning, quiescence search with delta and SEE pruning, move ordering
 * (TT move, promotions, MVV-LVA with SEE, killers, countermove, history
 * with gravity), aspiration windows and soft/hard time management.
 * Same ideas and constants as the JS engine v4.
 *
 * Threads (Lazy SMP): N workers search the same root at the same time and
 * share only the transposition table; each keeps its own move-ordering
 * tables, evaluation caches and NNUE accumulators. Worker 0 owns the clock,
 * prints info lines and picks the final move; odd helpers search one ply
 * deeper per iteration so the threads spread over more of the tree.
 */
#include "search.h"
#include "eval.h"
#include "movegen.h"
#include "tt.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace Search {

std::atomic<bool> stopFlag{false};
std::mutex ioMutex;
int moveOverhead = 30;
// Search feature bitmask (hidden UCI option "SFeatures"); 0 = stage-6 search.
int features = 131076;
enum {
  F_QTT = 1,       // transposition table in quiescence search
  F_TTEVAL = 2,    // static eval from TT + TT-score-corrected eval for pruning
  F_SE = 4,        // singular extensions (+ multi-cut, negative extension)
  F_CONT = 8,      // continuation history (1 and 2 ply) + capture history
  F_LMR = 16,      // LMR: reduce bad captures, more after a capture TT move
  F_SEEP = 32,     // SEE pruning of quiets and captures in the main search
  F_NMP = 64,      // larger, eval-dependent null-move reduction
  F_TM = 128,      // low-clock safety cap on the hard time limit
  F_ASP = 256,     // narrower aspiration window, gentler widening
  F_RFP = 512,     // reverse futility to depth 8 with smaller margins
  F_HPRUNE = 1024, // history pruning of bad quiets at low depth
  F_LMP = 2048,    // late-move + futility pruning extended to depth 6
  F_STAB = 4096,   // best-move stability shortens the soft time limit
  F_HBONUS = 8192, // larger, linear history bonus (history actually steers LMR)
  // ---- stage 8 ----
  F_PCORR = 16384,   // pawn correction history
  F_IMPR = 32768,    // improving (ss-4 fallback) in RFP + futility margins
  F_TM2 = 65536,     // time management fix (overhead 50, hard cap, no late iteration, fast helper stop)
  F_NPCORR = 131072, // non-pawn (per colour) correction history
  F_PROBCUT = 262144,// ProbCut
};
static const int CORR_SIZE = 16384;
static inline uint64_t mix64(uint64_t x) {
  x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
  return x;
}
static inline bool on(int f) { return (features & f) != 0; }

static const int MAX_PLY = 100;
static const int MATERIAL[7] = {0, 100, 312, 328, 505, 895, 0};
static const int SEE_VAL[7] = {0, 100, 320, 330, 500, 900, 20000};
static int LMR[64][64];

// ---- state shared by all workers during one search ----
static std::atomic<bool> stopAll{false};     // worker 0 tells helpers to finish
static std::atomic<bool> ponderMode{false};  // clock ignored until ponderhit
static std::atomic<int64_t> t0ns{0};         // search start (reset by ponderhit)
static int64_t hardMs, softMs;
static bool clockMode;  // playing on a clock (not fixed movetime)
static uint64_t maxNodes;
static Limits limits;
static bool printing;
static Position rootPosition;
static Result result;

static inline int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
static inline int64_t elapsed() { return (nowNs() - t0ns.load(std::memory_order_acquire)) / 1000000; }

struct Picker {
  MoveList ml;
  int key[256];
};

static inline void historyUpdate(int &h, int bonus) { h += bonus - (h * std::abs(bonus)) / 16384; }
static inline void hist16(int16_t &h, int bonus) {
  int v = h;
  v += bonus - (v * std::abs(bonus)) / 16384;
  h = int16_t(std::max(-32000, std::min(32000, v)));
}

static std::string scoreStr(int s) {
  if (s > MATE_BOUND) return "mate " + std::to_string((MATE - s + 1) / 2);
  if (s < -MATE_BOUND) return "mate -" + std::to_string((MATE + s) / 2);
  return "cp " + std::to_string(s);
}

struct Worker {
  int id = 0;
  Position pos;
  Move killer1[MAX_PLY + 8], killer2[MAX_PLY + 8];
  int history[16][64];
  Move countermove[16][64];
  int16_t capHist[16][64][8];
  int16_t contHist[16][64][16][64];
  int stackPc[MAX_PLY + 8], stackTo[MAX_PLY + 8];
  Move excluded[MAX_PLY + 8];
  int16_t pawnCorr[2][CORR_SIZE];
  int16_t npCorr[2][2][CORR_SIZE];  // [stm][colour][key]
  int rootDepth = 1;
  int evalStack[MAX_PLY + 24];
  Move pvTable[MAX_PLY + 8][MAX_PLY + 8];
  int pvLen[MAX_PLY + 8];
  std::atomic<uint64_t> nodes{0};
  int ply = 0, selDepth = 0;
  bool halt = false;
  // root iteration results
  Move rootMove = NO_MOVE;
  int rootScore = 0;
  Move rootPv[MAX_PLY + 8];
  int rootPvLen = 0;
  // completed results
  Move best = NO_MOVE;
  int bestScore = 0, doneDepth = 0;
  Move bestPv[MAX_PLY + 8];
  int bestPvLen = 0;

  void clearTables() {
    std::memset(killer1, 0, sizeof(killer1));
    std::memset(killer2, 0, sizeof(killer2));
    std::memset(history, 0, sizeof(history));
    std::memset(countermove, 0, sizeof(countermove));
    std::memset(capHist, 0, sizeof(capHist));
    std::memset(contHist, 0, sizeof(contHist));
    std::memset(pawnCorr, 0, sizeof(pawnCorr));
    std::memset(npCorr, 0, sizeof(npCorr));
    for (int i = 0; i < MAX_PLY + 8; i++) stackPc[i] = -1, stackTo[i] = 0, excluded[i] = NO_MOVE;
  }

  int quietScore(Move m) const {
    int pc = pos.board[mFrom(m)], to = mTo(m);
    int s = history[pc][to];
    if (ply >= 1 && stackPc[ply - 1] >= 0) s += contHist[stackPc[ply - 1]][stackTo[ply - 1]][pc][to];
    if (ply >= 2 && stackPc[ply - 2] >= 0) s += contHist[stackPc[ply - 2]][stackTo[ply - 2]][pc][to];
    return s;
  }
  void quietUpdate(Move m, int bonus) {
    int pc = pos.board[mFrom(m)], to = mTo(m);
    historyUpdate(history[pc][to], bonus);
    if (ply >= 1 && stackPc[ply - 1] >= 0) hist16(contHist[stackPc[ply - 1]][stackTo[ply - 1]][pc][to], bonus);
    if (ply >= 2 && stackPc[ply - 2] >= 0) hist16(contHist[stackPc[ply - 2]][stackTo[ply - 2]][pc][to], bonus);
  }
  int16_t &capEntry(Move m) {
    int victim = mFlag(m) == F_EP ? PAWN : typeOf(pos.board[mTo(m)]);
    return capHist[pos.board[mFrom(m)]][mTo(m)][victim];
  }

  uint64_t npKey(int c) const {
    uint64_t k = 0x9e3779b97f4a7c15ULL * (c + 1);
    for (int t = KNIGHT; t <= KING; t++) k = mix64(k ^ pos.pieces(c, t) ^ (uint64_t(t) << 58));
    return k;
  }
  // Correction (centipawns) added to the raw static eval.
  int correction() const {
    int c = 0;
    const int us = pos.side;
    if (on(F_PCORR)) c += pawnCorr[us][pos.s().pawnKey % CORR_SIZE];
    if (on(F_NPCORR)) c += (npCorr[us][WHITE][npKey(WHITE) % CORR_SIZE] + npCorr[us][BLACK][npKey(BLACK) % CORR_SIZE]) / 2;
    return c / 16;
  }
  static inline void corrUpdate(int16_t &e, int bonus) {
    int v = e;
    v += bonus - v * std::abs(bonus) / 1024;
    e = int16_t(std::max(-1024, std::min(1024, v)));
  }
  void updateCorrection(int best, int raw, int depth) {
    const int bonus = std::max(-256, std::min(256, (best - raw) * depth / 8));
    const int us = pos.side;
    if (on(F_PCORR)) corrUpdate(pawnCorr[us][pos.s().pawnKey % CORR_SIZE], bonus);
    if (on(F_NPCORR)) {
      corrUpdate(npCorr[us][WHITE][npKey(WHITE) % CORR_SIZE], bonus);
      corrUpdate(npCorr[us][BLACK][npKey(BLACK) % CORR_SIZE], bonus);
    }
  }

  inline void addNode() { nodes.store(nodes.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }

  bool outOfTime() {
    if (halt) return true;
    if (on(F_TM2) && id != 0 && stopAll.load(std::memory_order_relaxed)) return halt = true;
    if ((nodes.load(std::memory_order_relaxed) & 1023) == 0) {
      if (stopAll.load(std::memory_order_relaxed) || stopFlag.load(std::memory_order_relaxed)) halt = true;
      else if (id == 0) {
        if ((hardMs > 0 && !ponderMode.load(std::memory_order_acquire) && elapsed() >= hardMs) ||
            (maxNodes && nodes.load(std::memory_order_relaxed) >= maxNodes))
          halt = true;
        if (halt && on(F_TM2)) stopAll.store(true, std::memory_order_relaxed);  // helpers stop at once
      }
    }
    return halt;
  }

  int prevKeyPiece(int &to) const {
    Move pm = pos.s().move;
    if (!pm) return -1;
    to = mTo(pm);
    return pos.board[to];
  }

  void scoreMoves(Picker &pk, Move ttBest) const {
    Move k1 = killer1[ply], k2 = killer2[ply];
    int pto = 0;
    int ppc = prevKeyPiece(pto);
    Move cm = ppc >= 0 ? countermove[ppc][pto] : NO_MOVE;
    for (int i = 0; i < pk.ml.n; i++) {
      Move m = pk.ml.m[i];
      int &k = pk.key[i];
      if (m == ttBest) { k = 20000000; continue; }
      if (isPromo(m)) { k = 9000000 + MATERIAL[promoType(m)]; continue; }
      if (isCapture(m)) {
        int victim = mFlag(m) == F_EP ? PAWN : typeOf(pos.board[mTo(m)]);
        int attacker = typeOf(pos.board[mFrom(m)]);
        int mvvLva = MATERIAL[victim] * 10 - attacker;
        if (on(F_CONT)) mvvLva += capHist[pos.board[mFrom(m)]][mTo(m)][victim] / 32;
        if (SEE_VAL[attacker] > SEE_VAL[victim] && pos.see(m) < 0) k = 6000000 + mvvLva;
        else k = 8000000 + mvvLva;
      } else if (m == k1) k = 7000000;
      else if (m == k2) k = 6900000;
      else if (m == cm) k = 6800000;
      else k = on(F_CONT) ? quietScore(m) : history[pos.board[mFrom(m)]][mTo(m)];
    }
  }

  static inline Move pickNext(Picker &pk, int i) {
    int bi = i, bk = pk.key[i];
    for (int j = i + 1; j < pk.ml.n; j++)
      if (pk.key[j] > bk) { bk = pk.key[j]; bi = j; }
    if (bi != i) {
      std::swap(pk.ml.m[i], pk.ml.m[bi]);
      std::swap(pk.key[i], pk.key[bi]);
    }
    return pk.ml.m[i];
  }

  void updatePv(Move m) {
    pvTable[ply][0] = m;
    int n = pvLen[ply + 1];
    for (int i = 0; i < n; i++) pvTable[ply][i + 1] = pvTable[ply + 1][i];
    pvLen[ply] = n + 1;
  }

  int quiesce(int alpha, int beta, int qply) {
    addNode();
    pvLen[ply] = 0;
    if (ply > selDepth) selDepth = ply;
    if (outOfTime()) return 0;
    if (ply >= MAX_PLY) return Eval::evaluate(pos);
    Move ttMove = NO_MOVE;
    if (on(F_QTT)) {
      TTData e;
      if (TT.probe(pos.key(), e)) {
        ttMove = e.move;
        if (beta - alpha == 1) {
          int s = e.score;
          if (s > MATE_BOUND) s -= ply;
          else if (s < -MATE_BOUND) s += ply;
          if (e.flag == TT_EXACT || (e.flag == TT_LOWER && s >= beta) || (e.flag == TT_UPPER && s <= alpha)) return s;
        }
      }
    }
    Picker pk;
    if (pos.inCheck()) {
      generate(pos, pk.ml, false);
      scoreMoves(pk, ttMove);
      int best = -INF, legal = 0;
      for (int i = 0; i < pk.ml.n; i++) {
        Move m = pickNext(pk, i);
        if (!pos.legal(m)) continue;
        legal++;
        if (qply > 3) break;
        stackPc[ply] = pos.board[mFrom(m)];
        stackTo[ply] = mTo(m);
        pos.make(m);
        ply++;
        int score = -quiesce(-beta, -alpha, qply + 1);
        ply--;
        pos.unmake(m);
        if (halt) return 0;
        if (score > best) best = score;
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
      }
      if (legal == 0) return -MATE + ply;
      if (qply > 3) return Eval::evaluate(pos);
      return best;
    }
    int stand = Eval::evaluate(pos, alpha, beta);
    if ((on(F_PCORR) || on(F_NPCORR)) && std::abs(stand) < MATE_BOUND) stand += correction();
    if (stand >= beta) {
      if (on(F_QTT) && !ttMove) TT.store(pos.key(), 0, stand, TT_LOWER, NO_MOVE);
      return stand;
    }
    const int origAlpha = alpha;
    Move bestMove = NO_MOVE;
    if (stand > alpha) alpha = stand;
    generate(pos, pk.ml, true);
    scoreMoves(pk, ttMove);
    for (int i = 0; i < pk.ml.n; i++) {
      Move m = pickNext(pk, i);
      if (!isPromo(m)) {
        int victim = mFlag(m) == F_EP ? PAWN : typeOf(pos.board[mTo(m)]);
        if (stand + MATERIAL[victim] + 200 <= alpha) continue;  // delta pruning
        if (SEE_VAL[typeOf(pos.board[mFrom(m)])] > SEE_VAL[victim] && pos.see(m) < 0) continue;
      }
      if (!pos.legal(m)) continue;
      stackPc[ply] = pos.board[mFrom(m)];
      stackTo[ply] = mTo(m);
      pos.make(m);
      ply++;
      int score = -quiesce(-beta, -alpha, qply + 1);
      ply--;
      pos.unmake(m);
      if (halt) return 0;
      if (score > alpha) { alpha = score; bestMove = m; }
      if (alpha >= beta) break;
    }
    if (on(F_QTT)) {
      int st = alpha;
      if (st > MATE_BOUND) st += ply;
      else if (st < -MATE_BOUND) st -= ply;
      TT.store(pos.key(), 0, st, alpha >= beta ? TT_LOWER : alpha > origAlpha ? TT_EXACT : TT_UPPER, bestMove);
    }
    return alpha;
  }

  int negamax(int depth, int alpha, int beta, bool canNull) {
    if (depth <= 0) return quiesce(alpha, beta, 0);
    addNode();
    pvLen[ply] = 0;
    if (outOfTime()) return 0;
    if (ply > 0) {
      if (pos.s().half >= 100 || pos.isRepetition()) return 0;
      int lo = -MATE + ply, hi = MATE - ply - 1;  // mate distance pruning
      if (alpha < lo) alpha = lo;
      if (beta > hi) beta = hi;
      if (alpha >= beta) return alpha;
    }
    if (ply >= MAX_PLY) return Eval::evaluate(pos);
    const bool pvNode = beta - alpha > 1;
    const int us = pos.side;
    const bool inCheck = pos.inCheck();

    const Move excl = excluded[ply];
    Move ttBest = NO_MOVE;
    TTData e;
    const bool ttHit = TT.probe(pos.key(), e);
    int ttScore = 0;
    if (ttHit) {
      ttBest = e.move;
      ttScore = e.score;
      if (ttScore > MATE_BOUND) ttScore -= ply;
      else if (ttScore < -MATE_BOUND) ttScore += ply;
      if (!pvNode && !excl && e.depth >= depth) {
        int s = ttScore;
        int f = e.flag;
        if (f == TT_EXACT || (f == TT_LOWER && s >= beta) || (f == TT_UPPER && s <= alpha)) return s;
      }
    }

    // Internal iterative reduction: no stored best move -> one ply shallower.
    if (depth >= 4 && !ttBest) depth--;

    int staticEval;
    if (inCheck) staticEval = -INF;
    else if (on(F_TTEVAL) && ttHit && e.eval != EVAL_NONE && e.eval > -INF) staticEval = e.eval;
    else staticEval = Eval::evaluate(pos);
    // staticEval stays raw (stored in the TT); corrEval adds correction history.
    int corrEval = staticEval;
    if (!inCheck && (on(F_PCORR) || on(F_NPCORR)))
      corrEval = std::max(-MATE_BOUND + 1, std::min(MATE_BOUND - 1, staticEval + correction()));
    evalStack[ply] = corrEval;
    bool improving = !inCheck && ply >= 2 && evalStack[ply - 2] != -INF && corrEval > evalStack[ply - 2];
    if (on(F_IMPR) && !inCheck && ply >= 2) {
      int prev = evalStack[ply - 2];
      if (prev == -INF && ply >= 4) prev = evalStack[ply - 4];
      improving = prev != -INF ? corrEval > prev : true;
    }
    // Static eval corrected by a TT bound that is tighter.
    int eval = corrEval;
    if (on(F_TTEVAL) && ttHit && !inCheck && std::abs(ttScore) < MATE_BOUND &&
        (e.flag == TT_EXACT || (e.flag == TT_LOWER && ttScore > eval) || (e.flag == TT_UPPER && ttScore < eval)))
      eval = ttScore;

    if (!pvNode && !inCheck && beta < MATE_BOUND && beta > -MATE_BOUND) {
      // Reverse futility pruning.
      if (on(F_RFP)) {
        if (depth <= 8 && eval - (70 * depth - (improving ? 50 : 0)) >= beta) return eval;
      } else if (on(F_IMPR)) {
        if (depth <= 6 && eval - (85 * depth - (improving ? 60 : 0)) >= beta) return eval;
      } else if (depth <= 6 && eval - (85 * depth - (improving ? 30 : 0)) >= beta) return eval;
      // Razoring.
      if (!excl && depth <= 2 && eval + 250 * depth <= alpha) {
        int q = quiesce(alpha, alpha + 1, 0);
        if (halt) return 0;
        if (q <= alpha) return q;
      }
      // Null-move pruning.
      if (!excl && canNull && depth >= 3 && eval >= beta && pos.hasNonPawn(us)) {
        int R = on(F_NMP) ? 4 + depth / 3 + std::min((eval - beta) / 200, 3) : 3 + (depth >> 2);
        stackPc[ply] = -1;
        pos.makeNull();
        ply++;
        int s = -negamax(depth - 1 - R, -beta, -beta + 1, false);
        ply--;
        pos.unmakeNull();
        if (halt) return 0;
        if (s >= beta) return s > MATE_BOUND ? beta : s;
      }
      // ProbCut: a good capture that beats beta + 200 in a reduced search.
      if (on(F_PROBCUT) && !excl && depth >= 5) {
        const int pcBeta = beta + 200;
        if (!(ttHit && e.depth >= depth - 3 && ttScore < pcBeta)) {
          Picker pc;
          generate(pos, pc.ml, true);
          scoreMoves(pc, ttBest);
          for (int i = 0; i < pc.ml.n; i++) {
            Move m = pickNext(pc, i);
            if (isPromo(m) && promoType(m) != QUEEN) continue;
            if (pos.see(m) < std::max(0, pcBeta - eval)) continue;
            if (!pos.legal(m)) continue;
            stackPc[ply] = pos.board[mFrom(m)];
            stackTo[ply] = mTo(m);
            pos.make(m);
            ply++;
            int s = -quiesce(-pcBeta, -pcBeta + 1, 0);
            if (!halt && s >= pcBeta) s = -negamax(depth - 4, -pcBeta, -pcBeta + 1, true);
            ply--;
            pos.unmake(m);
            if (halt) return 0;
            if (s >= pcBeta) {
              TT.store(pos.key(), depth - 3, s, TT_LOWER, m, staticEval);
              return s;
            }
          }
        }
      }
    }

    Picker pk;
    generate(pos, pk.ml, false);
    scoreMoves(pk, ttBest);
    int pto = 0;
    int ppc = prevKeyPiece(pto);
    const int origAlpha = alpha;
    int best = -INF;
    Move bestMove = NO_MOVE;
    int legal = 0, quietsTried = 0;
    const int pruneDepth = on(F_LMP) ? 6 : 3;
    const bool futile = !pvNode && !inCheck && depth <= pruneDepth &&
                        eval + 100 + 110 * depth + (on(F_IMPR) && improving ? 50 : 0) <= alpha;
    const int lmpLimit = 3 + depth * depth * (improving ? 2 : 1);
    const bool ttCapture = ttBest && !isQuiet(ttBest);
    for (int i = 0; i < pk.ml.n; i++) {
      Move m = pickNext(pk, i);
      if (m == excl) continue;
      if (!pos.legal(m)) continue;
      const bool quiet = isQuiet(m);
      // Singular extension: is the TT move clearly better than all others?
      int ext = 0;
      if (on(F_SE) && m == ttBest && !excl && ply > 0 && depth >= 7 && ply < 2 * rootDepth && ttHit &&
          e.depth >= depth - 3 && (e.flag == TT_LOWER || e.flag == TT_EXACT) && std::abs(ttScore) < MATE_BOUND) {
        const int sBeta = ttScore - 2 * depth;
        excluded[ply] = m;
        int s = negamax((depth - 1) / 2, sBeta - 1, sBeta, false);
        excluded[ply] = NO_MOVE;
        if (halt) return 0;
        if (s < sBeta) ext = 1;
        else if (sBeta >= beta) return sBeta;  // multi-cut
        else if (ttScore >= beta) ext = -1;
      }
      // SEE pruning (decided before the move; applied if it gives no check).
      bool seeBad = false;
      if (on(F_SEEP) && !pvNode && !inCheck && legal >= 1 && best > -MATE_BOUND && depth <= 8 && m != ttBest &&
          !isPromo(m) && mFlag(m) != F_CASTLE)
        seeBad = pos.see(m) < (quiet ? -50 * depth : -100 * depth);
      const int hist = quiet ? (on(F_CONT) ? quietScore(m) : history[pos.board[mFrom(m)]][mTo(m)]) : 0;
      stackPc[ply] = pos.board[mFrom(m)];
      stackTo[ply] = mTo(m);
      pos.make(m);
      legal++;
      const bool givesCheck = pos.inCheck();
      if (quiet) quietsTried++;
      // Futility pruning and late move pruning for quiet, non-checking moves.
      if (legal > 1 && quiet && !givesCheck && !inCheck && !pvNode && best > -MATE_BOUND) {
        if (futile || (depth <= pruneDepth && quietsTried > lmpLimit) ||
            (on(F_HPRUNE) && depth <= 4 && hist < -1500 * depth && pk.key[i] < 6800000)) {
          pos.unmake(m);
          continue;
        }
      }
      if (seeBad && !givesCheck && legal > 1) {
        pos.unmake(m);
        continue;
      }
      ply++;
      const int newDepth = depth - 1 + (ext ? ext : (givesCheck ? 1 : 0));
      int score;
      if (legal == 1) {
        score = -negamax(newDepth, -beta, -alpha, true);
      } else {
        int r = 0;
        const bool badCap = on(F_LMR) && !quiet && !isPromo(m) && pk.key[i] < 6500000;
        if (depth >= 3 && (quiet || badCap) && !inCheck && !givesCheck) {
          int n = legal < 63 ? legal : 63;
          r = LMR[depth < 63 ? depth : 63][n];
          if (pvNode) r--;
          if (!improving) r++;
          if (badCap) r--;
          else if (pk.key[i] >= 6800000) r--;  // killer or countermove
          else r -= pk.key[i] / (on(F_CONT) ? 16000 : 6000);  // good history reduces less
          if (on(F_LMR) && ttCapture) r++;
          if (r > newDepth - 1) r = newDepth - 1;
          if (r < 0) r = 0;
        }
        score = -negamax(newDepth - r, -alpha - 1, -alpha, true);
        if (!halt && score > alpha && r > 0) score = -negamax(newDepth, -alpha - 1, -alpha, true);
        if (!halt && score > alpha && score < beta) score = -negamax(newDepth, -beta, -alpha, true);
      }
      ply--;
      pos.unmake(m);
      if (halt) return 0;
      if (score > best) {
        best = score;
        bestMove = m;
        if (score > alpha) {
          alpha = score;
          updatePv(m);
          if (alpha >= beta) {
            const int bonus = on(F_HBONUS) ? std::min(170 * depth - 120, 1500) : depth * depth > 1200 ? 1200 : depth * depth;
            if (quiet) {
              if (killer1[ply] != m) {
                killer2[ply] = killer1[ply];
                killer1[ply] = m;
              }
              if (on(F_CONT)) {
                quietUpdate(m, bonus);
                for (int j = 0; j < i; j++)
                  if (isQuiet(pk.ml.m[j]) && pk.ml.m[j] != excl) quietUpdate(pk.ml.m[j], -bonus);
              } else {
                historyUpdate(history[pos.board[mFrom(m)]][mTo(m)], bonus);
                for (int j = 0; j < i; j++) {  // penalise quiet moves tried before
                  Move q = pk.ml.m[j];
                  if (isQuiet(q)) historyUpdate(history[pos.board[mFrom(q)]][mTo(q)], -bonus);
                }
              }
              if (ppc >= 0) countermove[ppc][pto] = m;
            } else if (on(F_CONT) && !isPromo(m)) {
              hist16(capEntry(m), bonus);
            }
            if (on(F_CONT) && !isPromo(m))  // captures tried before the cut-off move
              for (int j = 0; j < i; j++) {
                Move q = pk.ml.m[j];
                if (!isQuiet(q) && !isPromo(q) && q != excl) hist16(capEntry(q), -bonus);
              }
            break;
          }
        }
      }
    }
    if (legal == 0) return excl ? alpha : inCheck ? -MATE + ply : 0;
    if (excl) return best;
    int st = best;
    if (st > MATE_BOUND) st += ply;
    else if (st < -MATE_BOUND) st -= ply;
    TT.store(pos.key(), depth, st, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, bestMove,
             inCheck ? EVAL_NONE : staticEval);
    if ((on(F_PCORR) || on(F_NPCORR)) && !inCheck && (!bestMove || isQuiet(bestMove)) && std::abs(best) < MATE_BOUND &&
        !(best >= beta && best <= corrEval) && !(best <= origAlpha && best >= corrEval))
      updateCorrection(best, staticEval, depth);
    return best;
  }

  // One root iteration with window (alpha, beta). Returns true if it finished.
  bool rootSearch(std::vector<Move> &moves, int depth, int alpha, int beta) {
    const int alpha0 = alpha;
    Move current = moves[0];
    int currentScore = -INF;
    for (size_t i = 0; i < moves.size(); i++) {
      Move m = moves[i];
      stackPc[0] = pos.board[mFrom(m)];
      stackTo[0] = mTo(m);
      pos.make(m);
      ply = 1;
      int newDepth = depth - 1 + (pos.inCheck() ? 1 : 0);
      int score;
      if (i == 0) score = -negamax(newDepth, -beta, -alpha, true);
      else {
        score = -negamax(newDepth, -alpha - 1, -alpha, true);
        if (!halt && score > alpha && score < beta) score = -negamax(newDepth, -beta, -alpha, true);
      }
      ply = 0;
      pos.unmake(m);
      if (halt) {
        rootMove = currentScore > alpha0 ? current : NO_MOVE;
        rootScore = currentScore;
        return false;
      }
      if (score > currentScore) {
        currentScore = score;
        current = m;
        if (score > alpha) {
          rootPv[0] = m;
          rootPvLen = 1;
          for (int k = 0; k < pvLen[1] && k < MAX_PLY; k++) rootPv[rootPvLen++] = pvTable[1][k];
          alpha = score;
        }
        if (alpha >= beta) break;
      }
    }
    rootMove = current;
    rootScore = currentScore;
    return true;
  }

  uint64_t totalNodes() const;
  void iterate();
};

static std::vector<std::unique_ptr<Worker>> workers;
static std::vector<std::thread> poolThreads;
static std::mutex poolMu;
static std::condition_variable poolCv, doneCv;
static uint64_t jobId = 0;
static int helpersActive = 0;
static bool quitPool = false, isSearching = false;

uint64_t Worker::totalNodes() const {
  uint64_t n = 0;
  for (auto &w : workers) n += w->nodes.load(std::memory_order_relaxed);
  return n;
}

// Iterative deepening for one worker.
void Worker::iterate() {
  pos = rootPosition;
  halt = false;
  nodes = 0;
  ply = 0;
  selDepth = 0;
  best = NO_MOVE;
  bestScore = 0;
  doneDepth = 0;
  bestPvLen = 0;
  std::memset(killer1, 0, sizeof(killer1));
  std::memset(killer2, 0, sizeof(killer2));
  for (auto &row : history)
    for (auto &h : row) h >>= 1;
  for (int i = 0; i < MAX_PLY + 8; i++) stackPc[i] = -1, excluded[i] = NO_MOVE;
  int stable = 0;

  MoveList ml;
  generate(pos, ml, false);
  std::vector<Move> legalMoves;
  for (int i = 0; i < ml.n; i++)
    if (pos.legal(ml.m[i])) legalMoves.push_back(ml.m[i]);
  if (legalMoves.empty()) return;
  evalStack[0] = pos.inCheck() ? -INF : Eval::evaluate(pos);

  Move rootTt = NO_MOVE;
  TTData e;
  if (TT.probe(pos.key(), e)) rootTt = e.move;
  std::vector<std::pair<int, Move>> keyed;
  for (Move m : legalMoves) {
    int k = 0;
    if (m == rootTt) k = 1000000000;
    else if (isPromo(m)) k = 9000000 + MATERIAL[promoType(m)];
    else if (isCapture(m))
      k = 8000000 + MATERIAL[mFlag(m) == F_EP ? PAWN : typeOf(pos.board[mTo(m)])] * 10 - typeOf(pos.board[mFrom(m)]);
    keyed.push_back({k, m});
  }
  std::stable_sort(keyed.begin(), keyed.end(), [](auto &a, auto &b) { return a.first > b.first; });
  std::vector<Move> moves;
  for (auto &p : keyed) moves.push_back(p.second);

  best = moves[0];
  bestPv[0] = best;
  bestPvLen = 1;
  const bool main = id == 0;
  const int maxDepth = std::min(limits.depth > 0 ? limits.depth : 64, MAX_PLY - 4);
  auto bringToFront = [&](Move m) {
    auto it = std::find(moves.begin(), moves.end(), m);
    if (it != moves.end()) std::rotate(moves.begin(), it, it + 1);
  };
  for (int iter = 1; iter <= maxDepth; iter++) {
    // Odd helpers run one ply ahead of the others.
    const int depth = main ? iter : std::min(maxDepth, iter + (id & 1));
    if (main && hardMs > 0 && !ponderMode.load() && elapsed() >= hardMs) break;
    if (main && on(F_TM2) && clockMode && iter > 1 && !ponderMode.load() && elapsed() >= softMs) break;
    if (stopFlag.load() || stopAll.load()) break;
    bringToFront(best);
    rootDepth = depth;
    int delta = on(F_ASP) ? 15 : 25, alpha = -INF, beta = INF;
    if (depth >= 5 && std::abs(bestScore) < MATE_BOUND) {
      alpha = bestScore - delta;
      beta = bestScore + delta;
    }
    rootPvLen = 0;
    bool finished;
    for (;;) {
      finished = rootSearch(moves, depth, alpha, beta);
      if (!finished) break;
      if (rootScore <= alpha) {
        beta = (alpha + beta) / 2;
        alpha = std::max(rootScore - delta, -INF);
      } else if (rootScore >= beta) {
        beta = std::min(rootScore + delta, INF);
        bringToFront(rootMove);
      } else break;
      delta += on(F_ASP) ? delta / 2 : delta;
      if (delta > 800) { alpha = -INF; beta = INF; }
    }
    if (!finished) {
      // Partial iteration: keep a better move found at the root (PVS proved it).
      if (rootMove && rootMove != best) {
        best = rootMove;
        if (rootPvLen && rootPv[0] == best) {
          bestPvLen = rootPvLen;
          std::memcpy(bestPv, rootPv, sizeof(Move) * rootPvLen);
        } else { bestPv[0] = best; bestPvLen = 1; }
      }
      break;
    }
    const bool changed = rootMove != best;
    stable = changed ? 0 : stable + 1;
    const int drop = depth >= 5 ? bestScore - rootScore : 0;
    best = rootMove;
    bestScore = rootScore;
    doneDepth = depth;
    if (rootPvLen && rootPv[0] == best) {
      bestPvLen = rootPvLen;
      std::memcpy(bestPv, rootPv, sizeof(Move) * rootPvLen);
    } else { bestPv[0] = best; bestPvLen = 1; }
    TT.store(pos.key(), depth, bestScore, TT_EXACT, best);
    if (!main) {
      if (depth >= maxDepth) break;
      continue;
    }
    if (printing) {
      int64_t ms = elapsed();
      uint64_t n = totalNodes();
      std::ostringstream os;
      os << "info depth " << depth << " seldepth " << selDepth << " score " << scoreStr(bestScore) << " nodes " << n
         << " nps " << (ms > 0 ? n * 1000 / ms : n) << " hashfull " << TT.hashfull() << " time " << ms << " pv";
      for (int i = 0; i < bestPvLen; i++) os << ' ' << moveToUci(bestPv[i]);
      { std::lock_guard<std::mutex> io(ioMutex); std::cout << os.str() << std::endl; }
    }
    if (std::abs(bestScore) > MATE_BOUND && !limits.infinite && limits.depth == 0) break;
    if (softMs > 0 && !ponderMode.load()) {
      double factor = 1;
      if (changed && depth >= 5) factor += 0.6;
      if (drop > 30) factor += 0.5;
      if (on(F_STAB) && !changed && drop <= 30) factor *= stable >= 6 ? 0.7 : stable >= 3 ? 0.85 : 1.0;
      // On a clock, softMs is the average time per move we can afford: do not
      // start an iteration that would probably not finish near that target.
      if (elapsed() > softMs * factor * (clockMode ? 0.6 : 1.0)) break;
    }
  }
}

// Worker 0: run the search, then finish the helpers and report.
static void mainSearch(Worker &w) {
  w.iterate();
  // "go infinite" / "go ponder" must not answer before "stop" / "ponderhit".
  while ((limits.infinite || ponderMode.load()) && !stopFlag.load())
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  stopAll = true;
  {
    std::unique_lock<std::mutex> lk(poolMu);
    doneCv.wait(lk, [] { return helpersActive == 0; });
  }
  // Pick the move: worker 0's, unless a helper finished a deeper iteration
  // with a better score.
  Worker *bestW = &w;
  for (auto &h : workers) {
    if (h.get() == &w || !h->best || !h->doneDepth) continue;
    if (h->doneDepth > bestW->doneDepth && h->bestScore > bestW->bestScore) bestW = h.get();
  }
  Result r;
  r.best = bestW->best;
  r.score = bestW->bestScore;
  r.depth = w.doneDepth;
  r.nodes = w.totalNodes();
  if (printing) {
    if (!r.best) {
      { std::lock_guard<std::mutex> io(ioMutex); std::cout << "info depth 0 score " << (w.pos.inCheck() ? "mate 0" : "cp 0") << "\nbestmove 0000" << std::endl; }
    } else {
      if (bestW != &w) {
        int64_t ms = elapsed();
        std::ostringstream os;
        os << "info depth " << bestW->doneDepth << " score " << scoreStr(bestW->bestScore) << " nodes " << r.nodes
           << " nps " << (ms > 0 ? r.nodes * 1000 / ms : r.nodes) << " time " << ms << " pv";
        for (int i = 0; i < bestW->bestPvLen; i++) os << ' ' << moveToUci(bestW->bestPv[i]);
        { std::lock_guard<std::mutex> io(ioMutex); std::cout << os.str() << std::endl; }
      }
      std::ostringstream os;
      os << "bestmove " << moveToUci(r.best);
      if (bestW->bestPvLen > 1 && bestW->bestPv[0] == r.best) os << " ponder " << moveToUci(bestW->bestPv[1]);
      { std::lock_guard<std::mutex> io(ioMutex); std::cout << os.str() << std::endl; }
    }
  }
  std::lock_guard<std::mutex> lk(poolMu);
  result = r;
  isSearching = false;
  doneCv.notify_all();
}

static void poolLoop(Worker *w, uint64_t seen) {
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(poolMu);
      poolCv.wait(lk, [&] { return quitPool || jobId != seen; });
      if (quitPool) return;
      seen = jobId;
    }
    if (w->id == 0) mainSearch(*w);
    else {
      w->iterate();
      std::lock_guard<std::mutex> lk(poolMu);
      if (--helpersActive == 0) doneCv.notify_all();
    }
  }
}

void init() {
  for (int d = 0; d < 64; d++)
    for (int m = 0; m < 64; m++)
      LMR[d][m] = d < 3 || m < 3 ? 0 : int(std::floor(0.75 + std::log(d) * std::log(m) / 2.25));
  setThreads(1);
}

void shutdown() {
  stop();
  {
    std::lock_guard<std::mutex> lk(poolMu);
    quitPool = true;
  }
  poolCv.notify_all();
  for (auto &t : poolThreads) t.join();
  poolThreads.clear();
  workers.clear();
  quitPool = false;
}

void setThreads(int n) {
  n = std::max(1, std::min(256, n));
  if (int(workers.size()) == n) return;
  shutdown();
  for (int i = 0; i < n; i++) {
    workers.push_back(std::make_unique<Worker>());
    workers.back()->id = i;
    workers.back()->clearTables();
  }
  // New threads must not pick up the last (finished) job.
  for (int i = 0; i < n; i++) poolThreads.emplace_back(poolLoop, workers[i].get(), jobId);
}

int threads() { return int(workers.size()); }

void clear() {
  wait();
  for (auto &w : workers) w->clearTables();
  TT.clear();
  Eval::clearCaches();
}

void start(const Position &pos, const Limits &lim, bool printInfo) {
  wait();
  t0ns.store(nowNs(), std::memory_order_release);
  limits = lim;
  printing = printInfo;
  rootPosition = pos;
  maxNodes = lim.nodes;
  ponderMode = lim.ponder;
  TT.newSearch();

  // Time: fixed movetime uses ~95-100 % of the budget; with a clock, a soft
  // limit (checked between iterations, stretched when the best move changes
  // or the score drops) and a hard limit (checked during the search).
  hardMs = 0;
  softMs = 0;
  clockMode = false;
  const int us = pos.side;
  if (!lim.infinite) {
    if (lim.movetime > 0) {
      hardMs = std::max<int64_t>(1, lim.movetime - moveOverhead);
      softMs = hardMs * 95 / 100;
    } else if (lim.time[us] >= 0) {
      // Plan over n moves: the remaining clock plus the increments still to
      // come, minus the move overhead (GUI/network lag) for every one of them.
      const double over = on(F_TM2) ? std::max(moveOverhead, 50) : moveOverhead;
      const double raw = lim.time[us], inc = lim.inc[us];
      const int n = lim.movestogo > 0 ? std::min(lim.movestogo, 50) : 30;
      double avail = raw - over * n + inc * (n - 1);
      avail = std::max(avail, std::min(raw * 0.3, raw - over));  // very low clock
      avail = std::max(avail, 1.0);
      double soft = avail / n;
      double hard;
      if (lim.movestogo > 0) hard = std::min(avail * (n == 1 ? 0.6 : 0.4), soft * 4);
      else hard = std::min(avail * 0.25, soft * 4);
      hard = std::min(hard, raw - over);  // never plan past the end of the clock
      // Low clock: never spend more than a quarter of the clock plus most of
      // one increment, so the clock settles well above the move overhead.
      if (on(F_TM)) hard = std::min(hard, raw * 0.25 + inc * 0.6);
      if (on(F_TM2)) hard = std::min({hard, soft * 3, raw * 0.2});
      hard = std::max(hard, 1.0);
      soft = std::min(soft, hard);
      hardMs = int64_t(hard);
      softMs = std::max<int64_t>(1, int64_t(soft));
      clockMode = true;
      // Only one legal move: answer almost at once.
      MoveList ml;
      generate(pos, ml, false);
      int legal = 0;
      for (int i = 0; i < ml.n; i++) legal += pos.legal(ml.m[i]);
      if (legal == 1) {
        hardMs = std::min<int64_t>(hardMs, 20);
        softMs = std::min<int64_t>(softMs, 5);
      }
    }
  }

  for (auto &w : workers) w->nodes = 0;
  std::lock_guard<std::mutex> lk(poolMu);
  stopAll = false;
  helpersActive = int(workers.size()) - 1;
  isSearching = true;
  jobId++;
  poolCv.notify_all();
}

void wait() {
  std::unique_lock<std::mutex> lk(poolMu);
  doneCv.wait(lk, [] { return !isSearching; });
}

void stop() {
  stopFlag = true;
  wait();
  stopFlag = false;
}

void ponderhit() {
  // Our clock starts now: the time budget is measured from this moment.
  t0ns.store(nowNs(), std::memory_order_release);
  ponderMode.store(false, std::memory_order_release);
}

bool searching() {
  std::lock_guard<std::mutex> lk(poolMu);
  return isSearching;
}

Result lastResult() {
  std::lock_guard<std::mutex> lk(poolMu);
  return result;
}

Move go(Position &pos, const Limits &lim, bool printInfo, uint64_t *nodesOut, int *depthOut, int *scoreOut) {
  start(pos, lim, printInfo);
  wait();
  Result r = lastResult();
  if (nodesOut) *nodesOut = r.nodes;
  if (depthOut) *depthOut = r.depth;
  if (scoreOut) *scoreOut = r.score;
  return r.best;
}

}  // namespace Search
