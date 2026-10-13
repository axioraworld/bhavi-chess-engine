/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * search.h: iterative deepening alpha-beta (PVS) search, ported from the
 * JS engine v4, run by a pool of threads (Lazy SMP: every thread searches
 * the same root and they share the lock-free transposition table).
 */
#pragma once
#include "position.h"
#include <atomic>

#include <mutex>
namespace Search {
// Serialises all UCI output: the search thread prints info/bestmove while the
// main thread may answer isready etc. (std::cout is not thread-safe).
extern std::mutex ioMutex;
struct Limits {
  int movetime = 0;          // ms, 0 = none
  int time[2] = {-1, -1};    // wtime/btime, -1 = none
  int inc[2] = {0, 0};
  int movestogo = 0;         // 0 = sudden death
  int depth = 0;             // 0 = none
  uint64_t nodes = 0;        // 0 = none
  bool infinite = false;
  bool ponder = false;       // "go ponder": search without a clock until ponderhit/stop
};
struct Result {
  Move best = NO_MOVE;
  int score = 0, depth = 0;
  uint64_t nodes = 0;
};
extern std::atomic<bool> stopFlag;  // set by UCI "stop"
extern int moveOverhead;
extern int features;  // search feature bitmask (hidden option SFeatures)
void init();
void setThreads(int n);  // only while no search is running
int threads();
void clear();            // ucinewgame: TT, histories, caches
// Starts a search in the background and returns immediately; the main
// search thread prints UCI info lines and "bestmove" if printInfo is set.
void start(const Position &pos, const Limits &lim, bool printInfo = true);
void wait();             // blocks until the running search (if any) has finished
void stop();             // stop + wait, then clears stopFlag
void ponderhit();        // the expected move was played: switch to the normal clock
bool searching();
Result lastResult();
// Blocking search used by bench and datagen.
Move go(Position &pos, const Limits &lim, bool printInfo = true, uint64_t *nodesOut = nullptr,
        int *depthOut = nullptr, int *scoreOut = nullptr);
void shutdown();
}  // namespace Search
