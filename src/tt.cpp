/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * tt.cpp: see tt.h. Words are read and written with relaxed atomic
 * operations, so concurrent access is well defined; the XOR check rejects
 * mixed halves from two different writers.
 */
#include "tt.h"
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

TransTable TT;

// 64-byte aligned allocation (Windows has no std::aligned_alloc).
#if defined(_WIN32)
#include <malloc.h>
static void *alignedAlloc(size_t n) { return _aligned_malloc(n, 64); }
static void alignedFree(void *p) { _aligned_free(p); }
#else
static void *alignedAlloc(size_t n) { return std::aligned_alloc(64, n); }
static void alignedFree(void *p) { std::free(p); }
#endif

static inline uint64_t ld(const uint64_t &w) { return __atomic_load_n(&w, __ATOMIC_RELAXED); }
static inline void st(uint64_t &w, uint64_t v) { __atomic_store_n(&w, v, __ATOMIC_RELAXED); }

static inline uint64_t pack(Move move, int score, int depth, int flag, int gen, int eval) {
  return uint64_t(move) | uint64_t(uint16_t(int16_t(score))) << 16 | uint64_t(uint16_t(int16_t(eval))) << 32 |
         uint64_t(uint8_t(depth)) << 48 | uint64_t(flag & 3) << 56 | uint64_t(gen & 63) << 58;
}
static inline int flagOf(uint64_t d) { return int((d >> 56) & 3); }
static inline void unpack(uint64_t d, TTData &o) {
  o.move = Move(d & 0xffff);
  o.score = int16_t(uint16_t(d >> 16));
  o.eval = int16_t(uint16_t(d >> 32));
  o.depth = int8_t(uint8_t(d >> 48));
  o.flag = flagOf(d);
  o.gen = int((d >> 58) & 63);
}

TransTable::~TransTable() { alignedFree(table_); }

void TransTable::resize(size_t mb) {
  if (mb < 1) mb = 1;
  alignedFree(table_);
  table_ = nullptr;
  n_ = mb * 1024 * 1024 / sizeof(TTBucket);
  while (!table_ && n_ > 0) {
    table_ = static_cast<TTBucket *>(alignedAlloc(n_ * sizeof(TTBucket)));
    if (!table_) n_ /= 2;  // not enough memory: fall back to a smaller table
  }
  clear();
}

void TransTable::clear() {
  // Large tables are zeroed by several threads at once.
  const size_t bytes = n_ * sizeof(TTBucket);
  const int nt = bytes >= (256u << 20) ? 4 : 1;
  if (nt == 1) std::memset(table_, 0, bytes);
  else {
    std::vector<std::thread> ts;
    const size_t chunk = n_ / nt;
    for (int i = 0; i < nt; i++) {
      size_t a = i * chunk, b = i + 1 == nt ? n_ : a + chunk;
      ts.emplace_back([this, a, b]() { std::memset(table_ + a, 0, (b - a) * sizeof(TTBucket)); });
    }
    for (auto &t : ts) t.join();
  }
  gen_ = 0;
}

bool TransTable::probe(uint64_t key, TTData &out) const {
  const TTBucket &b = bucket(key);
  for (const auto &e : b.e) {
    uint64_t d = ld(e.data);
    if ((ld(e.check) ^ d) == key && flagOf(d) != TT_NONE) {
      unpack(d, out);
      return true;
    }
  }
  return false;
}

void TransTable::store(uint64_t key, int depth, int score, int flag, Move move, int eval) {
  TTBucket &b = bucket(key);
  if (depth > 127) depth = 127;
  if (depth < 0) depth = 0;
  TTEntry *victim = nullptr;
  int worst = 1 << 30;
  for (auto &e : b.e) {
    uint64_t d = ld(e.data);
    TTData o;
    unpack(d, o);
    if ((ld(e.check) ^ d) == key && o.flag != TT_NONE) {
      // Same position: keep a deeper bound unless the new result is exact.
      if (o.depth > depth && flag != TT_EXACT && o.gen == gen_) return;
      if (!move) move = o.move;  // keep the old best move if we have none
      if (eval == EVAL_NONE) eval = o.eval;
      victim = &e;
      break;
    }
    int v = o.flag == TT_NONE ? -1000 : o.depth - 8 * ((gen_ - o.gen) & 63);
    if (v < worst) { worst = v; victim = &e; }
  }
  uint64_t d = pack(move, score, depth, flag, gen_, eval);
  st(victim->data, d);
  st(victim->check, key ^ d);
}

int TransTable::hashfull() const {
  int used = 0;
  for (size_t i = 0; i < 250 && i < n_; i++)
    for (const auto &e : table_[i].e) {
      uint64_t d = ld(e.data);
      used += flagOf(d) != TT_NONE && int((d >> 58) & 63) == gen_;
    }
  return used;
}
