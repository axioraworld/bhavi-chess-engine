/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * nnue.h: efficiently updatable neural-network evaluation (original code).
 * Network: 768 piece-square inputs per perspective -> H hidden (shared
 * weights, one accumulator for the side to move and one for the opponent)
 * -> clipped ReLU or squared clipped ReLU -> 1 output. int16 weights and
 * accumulators, int32/int64 output. File formats "AXN2" (one output) and "AXN3" (v6:
 * NB output buckets chosen by piece count, see tools/train_v6.py).
 */
#pragma once
#include "position.h"
#include <string>

namespace NNUE {
constexpr int MAX_H = 1024;
constexpr int MAX_NB = 8;  // output buckets by piece count (AXN3)

extern bool enabled;  // UCI "Use NNUE"
bool loaded();
int hidden();
std::string netName();
bool loadFile(const std::string &path);
bool loadEmbedded();
// Evaluation from the side to move's view (centipawns); uses and updates the
// accumulator stack incrementally.
int evaluate(const Position &pos);
// Same, but from a full refresh (for tests).
int evaluateRefresh(const Position &pos);
// Walks all moves to `depth` and compares incremental vs refreshed values.
// Returns the number of mismatches; counts checked nodes in `checked`.
uint64_t verify(Position &pos, int depth, uint64_t &checked, bool leavesOnly = false);
}  // namespace NNUE
