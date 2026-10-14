#include "bitboard.hpp"

#include <cstdlib>
#include <vector>

namespace bb {

uint64_t knightAttacks[64];
uint64_t kingAttacks[64];
uint64_t pawnAttacks[2][64];
uint64_t between[64][64];
uint64_t rookRays[64], bishopRays[64];
SliderTable rookTable, bishopTable;

namespace {

bool onBoard(int x, int y) { return x >= 0 && x < 8 && y >= 0 && y < 8; }

// Attack set of a slider on (x, y) with the given blockers, by walking the rays.
uint64_t slideAttacks(int x, int y, uint64_t occupied, const int (*dirs)[2]) {
    uint64_t attacks = 0;
    for (int d = 0; d < 4; d++) {
        for (int nx = x + dirs[d][0], ny = y + dirs[d][1]; onBoard(nx, ny); nx += dirs[d][0], ny += dirs[d][1]) {
            attacks |= bit(nx, ny);
            if (occupied & bit(nx, ny)) break;
        }
    }
    return attacks;
}

// The squares whose occupancy matters: every square on the rays except the
// last one on each (a blocker on the edge changes nothing).
uint64_t relevantMask(int x, int y, const int (*dirs)[2]) {
    uint64_t mask = 0;
    for (int d = 0; d < 4; d++) {
        for (int nx = x + dirs[d][0], ny = y + dirs[d][1]; onBoard(nx + dirs[d][0], ny + dirs[d][1]);
             nx += dirs[d][0], ny += dirs[d][1])
            mask |= bit(nx, ny);
    }
    return mask;
}

void buildSliderTable(SliderTable& table, const int (*dirs)[2], std::vector<uint64_t>& storage) {
    uint32_t offset = 0;
    for (int sq = 0; sq < 64; sq++) {
        const int x = sq / 8, y = sq % 8;
        table.mask[sq] = relevantMask(x, y, dirs);
        table.offset[sq] = offset;
        offset += uint32_t(1) << popcount(table.mask[sq]);
    }
    storage.assign(offset, 0);
    table.attacks = storage.data();
    for (int sq = 0; sq < 64; sq++) {
        const int x = sq / 8, y = sq % 8;
        const uint64_t mask = table.mask[sq];
        uint64_t subset = 0; // every subset of the mask, by the carry-rippler trick
        do {
            table.attacks[table.offset[sq] + pext(subset, mask)] = slideAttacks(x, y, subset, dirs);
            subset = (subset - mask) & mask;
        } while (subset);
    }
}

struct Tables {
    std::vector<uint64_t> rookStorage, bishopStorage;

    Tables() {
        static const int knight[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
        static const int rook[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        static const int bishop[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
        for (int sq = 0; sq < 64; sq++) {
            const int x = sq / 8, y = sq % 8;
            uint64_t n = 0, k = 0;
            for (const auto& j : knight)
                if (onBoard(x + j[0], y + j[1])) n |= bit(x + j[0], y + j[1]);
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++)
                    if ((dx || dy) && onBoard(x + dx, y + dy)) k |= bit(x + dx, y + dy);
            knightAttacks[sq] = n;
            kingAttacks[sq] = k;
            for (int color = 0; color < 2; color++) {
                const int dir = color == 0 ? 1 : -1;
                uint64_t p = 0;
                for (int dx : {-1, 1})
                    if (onBoard(x + dx, y + dir)) p |= bit(x + dx, y + dir);
                pawnAttacks[color][sq] = p;
            }
        }
        for (int a = 0; a < 64; a++) {
            for (int b = 0; b < 64; b++) {
                const int ax = a / 8, ay = a % 8, bx = b / 8, by = b % 8;
                const int dx = bx - ax, dy = by - ay;
                if (a == b || !(dx == 0 || dy == 0 || std::abs(dx) == std::abs(dy))) continue;
                const int stepX = (dx > 0) - (dx < 0), stepY = (dy > 0) - (dy < 0);
                uint64_t squares = 0;
                for (int x = ax + stepX, y = ay + stepY; x != bx || y != by; x += stepX, y += stepY) squares |= bit(x, y);
                between[a][b] = squares;
            }
        }
        buildSliderTable(rookTable, rook, rookStorage);
        buildSliderTable(bishopTable, bishop, bishopStorage);
        for (int sq = 0; sq < 64; sq++) {
            rookRays[sq] = rookAttacks(sq, 0);
            bishopRays[sq] = bishopAttacks(sq, 0);
        }
    }
};

// Built before main(); nothing else in the program uses a Board while static
// objects are being constructed.
const Tables tables;

} // namespace

} // namespace bb
