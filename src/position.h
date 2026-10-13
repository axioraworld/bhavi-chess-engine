/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * position.h: board state (bitboards + mailbox), FEN, make/unmake with an
 * undo stack, Zobrist keys, incremental material/piece-square/phase sums,
 * check and pin information, repetition and fifty-move detection, SEE.
 */
#pragma once
#include "core.h"
#include <string>

constexpr int STACK_SIZE = 2048;
enum { WK = 1, WQ = 2, BK = 4, BQ = 8 };

struct StateInfo {
  uint64_t key, pawnKey;
  int castle, ep, half;
  int captured;  // captured piece (NO_PIECE if none)
  int psqMg, psqEg, phase;
  Bitboard checkers, pinned;  // for the side to move
  Move move;                  // move that led here (NO_MOVE for root / null)
  // NNUE: pieces changed by the move into this state (from or to == 64 means
  // added / removed) and a unique serial so accumulators can be matched.
  uint64_t serial;
  uint8_t dirtyN;
  uint8_t dpc[4], dfrom[4], dto[4];
};
uint64_t nextSerial();

struct Position {
  uint8_t board[64];
  Bitboard pcs[16];  // indexed by piece code
  Bitboard occ[2], all;
  int side;
  int sp;  // index of current state in st[]
  StateInfo st[STACK_SIZE];

  bool setFen(const std::string &fen);
  std::string fen() const;
  void make(Move m);
  void unmake(Move m);
  void makeNull();
  void unmakeNull();

  const StateInfo &s() const { return st[sp]; }
  uint64_t key() const { return st[sp].key; }
  bool inCheck() const { return st[sp].checkers != 0; }
  Bitboard pieces(int c, int t) const { return pcs[makePiece(c, t)]; }
  int king(int c) const { return lsb(pcs[makePiece(c, KING)]); }
  Bitboard attackersTo(int sq, Bitboard occupied) const;
  bool attacked(int sq, int byColor, Bitboard occupied) const;
  bool attacked(int sq, int byColor) const { return attacked(sq, byColor, all); }
  bool legal(Move m) const;
  bool isRepetition() const;
  bool hasNonPawn(int c) const;
  int see(Move m) const;
  Move parseUci(const std::string &u) const;
  void rebase();  // drop old history when the stack gets long

 private:
  void put(int pc, int sq);
  void remove(int sq);
  void moveP(int from, int to);
  void putH(StateInfo &n, int pc, int sq);
  void removeH(StateInfo &n, int sq);
  void moveH(StateInfo &n, int from, int to);
  void updateCheckInfo();
};

namespace Zobrist {
extern uint64_t psq[16][64], castle[16], epFile[8], sideKey;
void init();
}  // namespace Zobrist
