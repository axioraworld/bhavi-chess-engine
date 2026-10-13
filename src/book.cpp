/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * book.cpp: Polyglot book reader. A book is a sorted list of 16-byte
 * big-endian records (key 64, move 16, weight 16, learn 32). The key mixes
 * 781 fixed random numbers: one per (piece, square), four castling rights,
 * eight en-passant files (only when a pawn can actually capture) and the
 * side to move. Moves: to-file 0-2, to-rank 3-5, from-file 6-8,
 * from-rank 9-11, promotion 12-14 (1 = knight ... 4 = queen); castling is
 * written as "king takes own rook".
 */
#include "book.h"
#include "movegen.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

namespace Book {

static const uint64_t RND[781] = {
#include "polyglot_keys.inc"
};

struct Entry {
  uint64_t key;
  uint16_t move, weight;
};
static std::vector<Entry> entries;

static uint64_t be(const unsigned char *p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++) v = v << 8 | p[i];
  return v;
}

bool load(const std::string &path) {
  entries.clear();
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  unsigned char r[16];
  while (std::fread(r, 1, 16, f) == 16) entries.push_back({be(r, 8), uint16_t(be(r + 8, 2)), uint16_t(be(r + 10, 2))});
  std::fclose(f);
  // Books are sorted by key already; sort anyway so a bad file cannot break lookups.
  std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.key < b.key; });
  return !entries.empty();
}

void unload() { entries.clear(); }
bool loaded() { return !entries.empty(); }
size_t size() { return entries.size(); }

uint64_t key(const Position &pos) {
  uint64_t k = 0;
  for (Bitboard b = pos.all; b;) {
    int sq = popLsb(b);
    int pc = pos.board[sq];
    int kind = (typeOf(pc) - 1) * 2 + (colorOf(pc) == WHITE ? 1 : 0);
    k ^= RND[64 * kind + sq];
  }
  int c = pos.s().castle;
  if (c & WK) k ^= RND[768];
  if (c & WQ) k ^= RND[769];
  if (c & BK) k ^= RND[770];
  if (c & BQ) k ^= RND[771];
  int ep = pos.s().ep;
  if (ep != NO_SQ && (PawnAtt[pos.side ^ 1][ep] & pos.pieces(pos.side, PAWN))) k ^= RND[772 + fileOf(ep)];
  if (pos.side == WHITE) k ^= RND[780];
  return k;
}

// Matches a Polyglot move against the legal moves of pos.
static Move decode(const Position &pos, uint16_t pm) {
  int to = (pm & 7) + 8 * ((pm >> 3) & 7);
  int from = ((pm >> 6) & 7) + 8 * ((pm >> 9) & 7);
  int promo = (pm >> 12) & 7;
  MoveList ml;
  generate(pos, ml, false);
  for (int i = 0; i < ml.n; i++) {
    Move m = ml.m[i];
    if (mFrom(m) != from || !pos.legal(m)) continue;
    int mt = mTo(m);
    if (mFlag(m) == F_CASTLE) {
      // Book writes castling as king to rook square (e1h1, e1a1, ...).
      int rookSq = mt > from ? from + 3 : from - 4;
      if (to != rookSq && to != mt) continue;
    } else if (mt != to) continue;
    if (isPromo(m) != (promo != 0)) continue;
    if (promo && promoType(m) != KNIGHT + promo - 1) continue;
    return m;
  }
  return NO_MOVE;
}

Move probe(const Position &pos, bool best) {
  if (entries.empty()) return NO_MOVE;
  uint64_t k = key(pos);
  auto it = std::lower_bound(entries.begin(), entries.end(), k, [](const Entry &e, uint64_t v) { return e.key < v; });
  std::vector<std::pair<Move, int>> cand;
  int total = 0;
  for (; it != entries.end() && it->key == k; ++it) {
    Move m = decode(pos, it->move);
    if (!m || it->weight == 0) continue;
    cand.push_back({m, it->weight});
    total += it->weight;
  }
  if (cand.empty()) return NO_MOVE;
  if (best) return std::max_element(cand.begin(), cand.end(), [](auto &a, auto &b) { return a.second < b.second; })->first;
  static std::mt19937 rng(uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()));
  int r = int(rng() % uint32_t(total));
  for (auto &c : cand) {
    if (r < c.second) return c.first;
    r -= c.second;
  }
  return cand.back().first;
}

}  // namespace Book
