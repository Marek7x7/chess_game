#pragma once

#include <cstdint>

#if defined(__BMI2__)
#include <immintrin.h>
#endif

// Bitboards over Board's own square numbering: bit x * 8 + y is file x
// (a = 0), rank y (rank 1 = 0). Attack sets for the leaper pieces come from
// small tables; the sliders' from tables indexed by the occupancy along the
// piece's lines (PEXT: the relevant occupancy bits packed into an index).
namespace bb {

constexpr uint64_t bit(int x, int y) { return uint64_t(1) << (x * 8 + y); }
inline int popcount(uint64_t b) { return __builtin_popcountll(b); }
inline int lsb(uint64_t b) { return __builtin_ctzll(b); } // b != 0
inline uint64_t clearLsb(uint64_t b) { return b & (b - 1); }

extern uint64_t knightAttacks[64];
extern uint64_t kingAttacks[64];
extern uint64_t pawnAttacks[2][64]; // [color][sq]: squares a pawn of that color standing on sq attacks
extern uint64_t between[64][64];   // the squares strictly between two squares on one line (else 0)
extern uint64_t rookRays[64], bishopRays[64]; // the attack sets of a rook / bishop on an empty board

struct SliderTable {
    uint64_t mask[64];   // squares whose occupancy changes the attack set
    uint32_t offset[64]; // start of the square's block in `attacks`
    uint64_t* attacks;
};
extern SliderTable rookTable, bishopTable;

inline uint64_t pext(uint64_t value, uint64_t mask) {
#if defined(__BMI2__)
    return _pext_u64(value, mask);
#else
    uint64_t result = 0;
    for (uint64_t bitPos = 1; mask; bitPos <<= 1) {
        uint64_t lowest = mask & (~mask + 1);
        if (value & lowest) result |= bitPos;
        mask &= mask - 1;
    }
    return result;
#endif
}

inline uint64_t rookAttacks(int sq, uint64_t occupied) {
    return rookTable.attacks[rookTable.offset[sq] + pext(occupied, rookTable.mask[sq])];
}
inline uint64_t bishopAttacks(int sq, uint64_t occupied) {
    return bishopTable.attacks[bishopTable.offset[sq] + pext(occupied, bishopTable.mask[sq])];
}
inline uint64_t queenAttacks(int sq, uint64_t occupied) { return rookAttacks(sq, occupied) | bishopAttacks(sq, occupied); }

} // namespace bb
