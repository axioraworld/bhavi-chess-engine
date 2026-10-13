/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * movegen.h: pseudo-legal move generation (Position::legal() filters).
 */
#pragma once
#include "position.h"

struct MoveList {
  Move m[256];
  int n = 0;
  void add(Move x) { m[n++] = x; }
};

// capturesOnly: captures plus all promotions (used by quiescence).
void generate(const Position &pos, MoveList &ml, bool capturesOnly);
uint64_t perft(Position &pos, int depth);
