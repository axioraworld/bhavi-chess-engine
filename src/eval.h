/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * eval.h: hand-crafted tapered evaluation, ported from the JS engine v4
 * (same terms and weights).
 */
#pragma once
#include "position.h"

constexpr int MATE = 30000;
constexpr int INF = 32000;
constexpr int MATE_BOUND = MATE - 1000;

namespace Eval {
extern int PSQ_MG[16][64], PSQ_EG[16][64];
extern const int PHASE_W[7];
void init();
extern int nnueWeight;  // percent of NNUE in the blend with HCE (100 = pure NNUE)
void clearCaches();
// Score from the side to move's view. With a window (alpha, beta) a lazy
// estimate may be returned when material + tables are far outside it.
int evaluate(const Position &pos, int alpha = -INF, int beta = INF);
int evaluateFull(const Position &pos);
// NNUE (if enabled and loaded) or hand-crafted, no cache, no lazy cut.
int evaluateStatic(const Position &pos);
}  // namespace Eval
