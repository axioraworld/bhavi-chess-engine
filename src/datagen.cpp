/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * datagen.cpp: the engine plays itself from random openings (8-11 random
 * plies, then 3 % random moves), searching a fixed number of nodes per move.
 * Quiet positions (not in check, best move not a capture or promotion,
 * |score| < 3000) are written with the search score (White's view) and the
 * final game result. Record (68 bytes, little endian): int16 feats[32]
 * (colour*384 + (type-1)*64 + square, -1 padded), int8 stm (1 = White),
 * int16 score, int8 result (+1/0/-1).
 * Stops when the time budget is used up (checked between games) or when a
 * file named "datagen.stop" exists in the output file's folder, so a run can
 * be ended cleanly without leaving a half-written record.
 */
#include "datagen.h"
#include "eval.h"
#include "movegen.h"
#include "search.h"
#include "tt.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#pragma pack(push, 1)
struct Record {
  int16_t feats[32];
  int8_t stm;
  int16_t score;
  int8_t result;
};
#pragma pack(pop)
static_assert(sizeof(Record) == 68, "record size");

static bool insufficient(const Position &p) {
  if (p.pieces(WHITE, PAWN) | p.pieces(BLACK, PAWN) | p.pieces(WHITE, ROOK) | p.pieces(BLACK, ROOK) |
      p.pieces(WHITE, QUEEN) | p.pieces(BLACK, QUEEN))
    return false;
  return popcount(p.occ[WHITE]) <= 2 && popcount(p.occ[BLACK]) <= 2 &&
         popcount(p.all) <= 3;  // K vs K, K+minor vs K
}

static std::string stopFileFor(const std::string &out) {
  size_t p = out.find_last_of("/\\");
  return (p == std::string::npos ? std::string() : out.substr(0, p + 1)) + "datagen.stop";
}

static bool stopRequested(const std::string &stopFile) {
  FILE *s = std::fopen(stopFile.c_str(), "rb");
  if (!s) return false;
  std::fclose(s);
  return true;
}

void runDatagen(const std::string &out, double seconds, uint64_t nodes, uint64_t seed) {
  const std::string stopFile = stopFileFor(out);
  FILE *f = std::fopen(out.c_str(), "ab");
  if (!f) { std::printf("cannot open %s\n", out.c_str()); return; }
  std::mt19937_64 rng(seed);
  auto t0 = std::chrono::steady_clock::now();
  auto secs = [&]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
  static Position pos;  // static: large object, keep off the stack
  uint64_t games = 0, written = 0;
  std::vector<Record> recs;
  while (secs() < seconds && !stopRequested(stopFile)) {
    pos.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    Search::clear();
    bool ok = true;
    int nrand = 8 + int(rng() % 4);
    for (int i = 0; i < nrand && ok; i++) {
      MoveList ml;
      generate(pos, ml, false);
      std::vector<Move> legal;
      for (int k = 0; k < ml.n; k++)
        if (pos.legal(ml.m[k])) legal.push_back(ml.m[k]);
      if (legal.empty()) { ok = false; break; }
      pos.make(legal[rng() % legal.size()]);
    }
    if (!ok) continue;
    recs.clear();
    int result = 2;  // 2 = unknown
    int big = 0, small = 0, plies = 0;
    while (true) {
      MoveList ml;
      generate(pos, ml, false);
      std::vector<Move> legal;
      for (int k = 0; k < ml.n; k++)
        if (pos.legal(ml.m[k])) legal.push_back(ml.m[k]);
      if (legal.empty()) {
        result = pos.inCheck() ? (pos.side == WHITE ? -1 : 1) : 0;
        break;
      }
      if (pos.s().half >= 100 || pos.isRepetition() || insufficient(pos) || plies > 320) { result = 0; break; }
      Search::Limits lim;
      lim.nodes = nodes;
      lim.depth = 64;
      int score = 0;
      Move best = Search::go(pos, lim, false, nullptr, nullptr, &score);
      if (!best) { result = 0; break; }
      int wscore = pos.side == WHITE ? score : -score;
      bool quiet = !pos.inCheck() && !isCapture(best) && !isPromo(best);
      if (quiet && std::abs(score) < 3000) {
        Record r;
        std::memset(r.feats, 0xff, sizeof(r.feats));
        int n = 0;
        for (Bitboard b = pos.all; b && n < 32;) {
          int sq = popLsb(b);
          int pc = pos.board[sq];
          r.feats[n++] = int16_t(colorOf(pc) * 384 + (typeOf(pc) - 1) * 64 + sq);
        }
        r.stm = pos.side == WHITE ? 1 : 0;
        r.score = int16_t(wscore);
        r.result = 0;
        recs.push_back(r);
      }
      big = std::abs(score) > 1500 ? big + 1 : 0;
      small = (std::abs(score) < 15 && plies > 120) ? small + 1 : 0;
      if (big >= 6) { result = wscore > 0 ? 1 : -1; break; }
      if (small >= 20) { result = 0; break; }
      Move m = best;
      if (rng() % 100 < 3) m = legal[rng() % legal.size()];
      pos.make(m);
      pos.rebase();
      plies++;
    }
    for (auto &r : recs) r.result = int8_t(result);
    std::fwrite(recs.data(), sizeof(Record), recs.size(), f);
    std::fflush(f);
    games++;
    written += recs.size();
  }
  std::fclose(f);
  std::printf("datagen done games %llu positions %llu seconds %.0f\n", (unsigned long long)games,
              (unsigned long long)written, secs());
}
