/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * Original chess engine written for this project.
 *
 * core.h: basic types, squares, moves and bitboard helpers.
 */
#pragma once
#include <cstdint>
#include <string>

using Bitboard = uint64_t;
using Move = uint16_t;

enum Color { WHITE = 0, BLACK = 1 };
enum PieceType { NO_TYPE = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };
constexpr int NO_PIECE = 0;
constexpr int NO_SQ = 64;

inline int makePiece(int c, int t) { return (c << 3) | t; }
inline int typeOf(int p) { return p & 7; }
inline int colorOf(int p) { return p >> 3; }
inline int fileOf(int s) { return s & 7; }
inline int rankOf(int s) { return s >> 3; }

// Move = from (6 bits) | to (6 bits) | flag (4 bits).
// flag bit 2 = capture, bit 3 = promotion (low two bits = promoted piece - KNIGHT).
enum { F_QUIET = 0, F_DOUBLE = 1, F_CASTLE = 2, F_CAPTURE = 4, F_EP = 5, F_PROMO = 8, F_PROMO_CAP = 12 };
constexpr Move NO_MOVE = 0;
inline Move mkMove(int from, int to, int flag) { return Move(from | (to << 6) | (flag << 12)); }
inline int mFrom(Move m) { return m & 63; }
inline int mTo(Move m) { return (m >> 6) & 63; }
inline int mFlag(Move m) { return m >> 12; }
inline bool isCapture(Move m) { return (m >> 12) & 4; }
inline bool isPromo(Move m) { return (m >> 12) & 8; }
inline int promoType(Move m) { return KNIGHT + ((m >> 12) & 3); }
inline bool isQuiet(Move m) { return !((m >> 12) & 12); }

inline int lsb(Bitboard b) { return __builtin_ctzll(b); }
inline int popcount(Bitboard b) { return __builtin_popcountll(b); }
inline int popLsb(Bitboard &b) { int s = __builtin_ctzll(b); b &= b - 1; return s; }
inline Bitboard bit(int s) { return 1ULL << s; }
inline bool moreThanOne(Bitboard b) { return b & (b - 1); }

constexpr Bitboard FILE_A = 0x0101010101010101ULL;
constexpr Bitboard FILE_H = FILE_A << 7;
constexpr Bitboard RANK_1 = 0xFFULL;
inline Bitboard fileBB(int f) { return FILE_A << f; }
inline Bitboard rankBB(int r) { return RANK_1 << (8 * r); }

extern Bitboard PawnAtt[2][64], KnightAtt[64], KingAtt[64];
extern Bitboard BetweenBB[64][64], LineBB[64][64];

struct Magic {
  Bitboard mask, magic;
  Bitboard *table;
  int shift;
};
extern Magic BishopMagic[64], RookMagic[64];

inline Bitboard bishopAtt(int s, Bitboard occ) {
  const Magic &m = BishopMagic[s];
  return m.table[((occ & m.mask) * m.magic) >> m.shift];
}
inline Bitboard rookAtt(int s, Bitboard occ) {
  const Magic &m = RookMagic[s];
  return m.table[((occ & m.mask) * m.magic) >> m.shift];
}
inline Bitboard queenAtt(int s, Bitboard occ) { return bishopAtt(s, occ) | rookAtt(s, occ); }

void initBitboards();
std::string sqName(int s);
std::string moveToUci(Move m);
