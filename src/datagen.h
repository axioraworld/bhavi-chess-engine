/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * datagen.h: self-play training-data generator (original code).
 */
#pragma once
#include <cstdint>
#include <string>
// Plays games for `seconds` with `nodes` per move and appends records to `out`.
void runDatagen(const std::string &out, double seconds, uint64_t nodes, uint64_t seed);
