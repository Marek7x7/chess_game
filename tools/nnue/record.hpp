#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "board.hpp"

// One training position for the network, 32 bytes (the format of the data files made by pgn2data and the
// engine's datagen mode and read by the trainer). Squares are numbered as in the engine: file * 8 + rank.
namespace nnue::data {

struct Record {
    uint64_t occupancy; // bit sq set: a piece stands on sq
    uint8_t pieces[16]; // one nibble per set bit of occupancy, lowest square first: colour * 8 + (type - 1)
    int16_t score;      // search score in centipawns, side to move's point of view
    int8_t result;      // the game's result, side to move's point of view: +1 win, 0 draw, -1 loss
    uint8_t stm;        // side to move: 0 White, 1 Black
    uint8_t depth;      // depth of the search that produced the score (0 if unknown)
    uint8_t ply;        // plies played before this position, capped at 255
    uint16_t reserved;
};
static_assert(sizeof(Record) == 32, "Record must stay 32 bytes");

inline Record makeRecord(const Board& board, int score, int result, int depth, int ply) {
    Record r;
    std::memset(&r, 0, sizeof r);
    int n = 0;
    for (int sq = 0; sq < 64; sq++) {
        const Piece p = board.at(sq / 8, sq % 8);
        if (p.isEmpty()) continue;
        r.occupancy |= uint64_t(1) << sq;
        const unsigned nibble = (p.color == Color::White ? 0u : 8u) + static_cast<unsigned>(p.type) - 1u;
        r.pieces[n / 2] |= static_cast<uint8_t>(nibble << ((n % 2) * 4));
        n++;
    }
    r.score = static_cast<int16_t>(score < -32000 ? -32000 : (score > 32000 ? 32000 : score));
    r.result = static_cast<int8_t>(result);
    r.stm = board.sideToMove() == Color::White ? 0 : 1;
    r.depth = static_cast<uint8_t>(depth < 0 ? 0 : (depth > 255 ? 255 : depth));
    r.ply = static_cast<uint8_t>(ply < 0 ? 0 : (ply > 255 ? 255 : ply));
    return r;
}

// Calls f(sq, colour 0|1, type 1..6) for every piece of the record.
template <class F>
inline void forEachPiece(const Record& r, F&& f) {
    uint64_t occ = r.occupancy;
    int n = 0;
    while (occ) {
        const int sq = __builtin_ctzll(occ);
        occ &= occ - 1;
        const unsigned nibble = (r.pieces[n / 2] >> ((n % 2) * 4)) & 0xF;
        f(sq, static_cast<int>(nibble >> 3), static_cast<int>(nibble & 7) + 1);
        n++;
    }
}

inline int pieceCount(const Record& r) { return __builtin_popcountll(r.occupancy); }

// The position as a FEN (no castling rights, en passant or clocks: the network knows none of them).
inline std::string toFen(const Record& r) {
    char grid[8][8] = {};
    forEachPiece(r, [&](int sq, int color, int type) {
        const char letters[] = "pnbrqk";
        char c = letters[type - 1];
        grid[sq % 8][sq / 8] = color == 0 ? static_cast<char>(c - 32) : c;
    });
    std::string fen;
    for (int rank = 7; rank >= 0; rank--) {
        int empty = 0;
        for (int file = 0; file < 8; file++) {
            const char c = grid[rank][file];
            if (!c) {
                empty++;
                continue;
            }
            if (empty) fen += static_cast<char>('0' + empty);
            empty = 0;
            fen += c;
        }
        if (empty) fen += static_cast<char>('0' + empty);
        if (rank) fen += '/';
    }
    fen += r.stm == 0 ? " w - - 0 1" : " b - - 0 1";
    return fen;
}

} // namespace nnue::data
