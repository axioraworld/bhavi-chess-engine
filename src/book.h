/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * book.h: reader for opening books in the Polyglot (.bin) format, so the
 * engine can play varied, instant opening moves in an app (UCI OwnBook /
 * BookFile). Written from the public file-format description.
 */
#pragma once
#include "position.h"
#include <string>

namespace Book {
bool load(const std::string &path);  // false if missing or invalid
void unload();
bool loaded();
size_t size();
uint64_t key(const Position &pos);   // Polyglot hash of a position
// A book move for pos (legal, weighted random among the book entries; with
// best = true the highest-weight move), or NO_MOVE.
Move probe(const Position &pos, bool best = false);
}  // namespace Book
