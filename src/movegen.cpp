/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * movegen.cpp: see movegen.h.
 */
#include "movegen.h"

static inline void addPromos(MoveList &ml, int from, int to, bool cap) {
  int base = cap ? F_PROMO_CAP : F_PROMO;
  ml.add(mkMove(from, to, base + 3));  // queen first
  ml.add(mkMove(from, to, base + 0));
  ml.add(mkMove(from, to, base + 1));
  ml.add(mkMove(from, to, base + 2));
}

void generate(const Position &pos, MoveList &ml, bool capturesOnly) {
  const int us = pos.side, them = us ^ 1;
  const Bitboard own = pos.occ[us], enemy = pos.occ[them], all = pos.all, empty = ~all;
  const StateInfo &st = pos.s();

  // Pawns.
  Bitboard pawns = pos.pieces(us, PAWN);
  const int up = us == WHITE ? 8 : -8;
  const Bitboard promoRank = us == WHITE ? rankBB(7) : rankBB(0);
  const Bitboard thirdRank = us == WHITE ? rankBB(2) : rankBB(5);
  Bitboard single = (us == WHITE ? pawns << 8 : pawns >> 8) & empty;
  Bitboard b = single & promoRank;
  while (b) { int to = popLsb(b); addPromos(ml, to - up, to, false); }
  if (!capturesOnly) {
    b = single & ~promoRank;
    while (b) { int to = popLsb(b); ml.add(mkMove(to - up, to, F_QUIET)); }
    b = (us == WHITE ? (single & thirdRank) << 8 : (single & thirdRank) >> 8) & empty;
    while (b) { int to = popLsb(b); ml.add(mkMove(to - 2 * up, to, F_DOUBLE)); }
  }
  Bitboard capL, capR;  // towards file a / file h
  if (us == WHITE) {
    capL = ((pawns & ~FILE_A) << 7) & enemy;
    capR = ((pawns & ~FILE_H) << 9) & enemy;
  } else {
    capL = ((pawns & ~FILE_A) >> 9) & enemy;
    capR = ((pawns & ~FILE_H) >> 7) & enemy;
  }
  const int dl = us == WHITE ? 7 : -9, dr = us == WHITE ? 9 : -7;
  b = capL;
  while (b) {
    int to = popLsb(b);
    if (bit(to) & promoRank) addPromos(ml, to - dl, to, true);
    else ml.add(mkMove(to - dl, to, F_CAPTURE));
  }
  b = capR;
  while (b) {
    int to = popLsb(b);
    if (bit(to) & promoRank) addPromos(ml, to - dr, to, true);
    else ml.add(mkMove(to - dr, to, F_CAPTURE));
  }
  if (st.ep != NO_SQ) {
    b = PawnAtt[them][st.ep] & pawns;
    while (b) ml.add(mkMove(popLsb(b), st.ep, F_EP));
  }

  // Pieces.
  const Bitboard targets = capturesOnly ? enemy : ~own;
  for (int t = KNIGHT; t <= KING; t++) {
    Bitboard p = pos.pieces(us, t);
    while (p) {
      int from = popLsb(p);
      Bitboard a;
      switch (t) {
        case KNIGHT: a = KnightAtt[from]; break;
        case BISHOP: a = bishopAtt(from, all); break;
        case ROOK: a = rookAtt(from, all); break;
        case QUEEN: a = queenAtt(from, all); break;
        default: a = KingAtt[from]; break;
      }
      a &= targets;
      while (a) {
        int to = popLsb(a);
        ml.add(mkMove(from, to, (enemy & bit(to)) ? F_CAPTURE : F_QUIET));
      }
    }
  }

  // Castling (path empty, king not in check and not passing attacked squares).
  if (!capturesOnly && st.castle && !st.checkers) {
    if (us == WHITE) {
      if ((st.castle & WK) && !(all & 0x60ULL) && !pos.attacked(5, them) && !pos.attacked(6, them))
        ml.add(mkMove(4, 6, F_CASTLE));
      if ((st.castle & WQ) && !(all & 0x0EULL) && !pos.attacked(3, them) && !pos.attacked(2, them))
        ml.add(mkMove(4, 2, F_CASTLE));
    } else {
      if ((st.castle & BK) && !(all & (0x60ULL << 56)) && !pos.attacked(61, them) && !pos.attacked(62, them))
        ml.add(mkMove(60, 62, F_CASTLE));
      if ((st.castle & BQ) && !(all & (0x0EULL << 56)) && !pos.attacked(59, them) && !pos.attacked(58, them))
        ml.add(mkMove(60, 58, F_CASTLE));
    }
  }
}

uint64_t perft(Position &pos, int depth) {
  MoveList ml;
  generate(pos, ml, false);
  uint64_t n = 0;
  if (depth == 1) {
    for (int i = 0; i < ml.n; i++) n += pos.legal(ml.m[i]);
    return n;
  }
  for (int i = 0; i < ml.n; i++) {
    Move m = ml.m[i];
    if (!pos.legal(m)) continue;
    pos.make(m);
    n += perft(pos, depth - 1);
    pos.unmake(m);
  }
  return n;
}
