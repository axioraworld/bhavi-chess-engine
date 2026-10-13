/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * tt.h: transposition table shared by all search threads without locks.
 * 64-byte buckets of four 16-byte entries. Each entry stores its data word
 * and (key XOR data); a probe accepts an entry only if the two words agree,
 * so an entry torn by two threads writing at once is simply ignored.
 * Replacement prefers empty, stale (older search) and shallow entries.
 */
#pragma once
#include "core.h"
#include <cstddef>

enum { TT_NONE = 0, TT_EXACT = 1, TT_LOWER = 2, TT_UPPER = 3 };

struct TTData {
  Move move;
  int score;
  int depth;
  int flag;
  int gen;
  int eval;  // static eval stored with the entry (EVAL_NONE if unknown)
};
constexpr int EVAL_NONE = -32768;

struct TTEntry {
  uint64_t check;  // key ^ data
  uint64_t data;   // move | score << 16 | eval << 32 | depth << 48 | flag << 56 | gen << 58 (6 bits)
};

struct alignas(64) TTBucket {
  TTEntry e[4];
};

class TransTable {
 public:
  ~TransTable();
  void resize(size_t mb);
  void clear();
  void newSearch() { gen_ = (gen_ + 1) & 63; }
  bool probe(uint64_t key, TTData &out) const;
  void store(uint64_t key, int depth, int score, int flag, Move move, int eval = EVAL_NONE);
  int hashfull() const;
  size_t sizeMb() const { return n_ * sizeof(TTBucket) >> 20; }

 private:
  TTBucket *table_ = nullptr;
  size_t n_ = 0;
  uint8_t gen_ = 0;
  TTBucket &bucket(uint64_t key) const { return table_[(unsigned __int128)key * n_ >> 64]; }
};

extern TransTable TT;
