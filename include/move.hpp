#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "piece.hpp"

// One byte per field (8 bytes in all): move lists are built, sorted, copied and
// stored millions of times a second.
struct Move {
    int8_t fromX = 0, fromY = 0;
    int8_t toX = 0, toY = 0;
    PieceType promotion = PieceType::None;
    bool isCastle = false;
    bool isEnPassant = false;
    PieceType captured = PieceType::None;

    Move() = default;
    Move(int fx, int fy, int tx, int ty, PieceType promo = PieceType::None, bool castle = false,
         bool enPassant = false, PieceType capturedType = PieceType::None)
        : fromX(static_cast<int8_t>(fx)),
          fromY(static_cast<int8_t>(fy)),
          toX(static_cast<int8_t>(tx)),
          toY(static_cast<int8_t>(ty)),
          promotion(promo),
          isCastle(castle),
          isEnPassant(enPassant),
          captured(capturedType) {}
};
static_assert(sizeof(Move) == 8, "Move is meant to fit in 8 bytes");

// The five fields that identify a move (squares and promotion) are its first five bytes: one 8-byte compare under a mask.
static_assert(offsetof(Move, fromX) == 0 && offsetof(Move, toY) == 3 && offsetof(Move, promotion) == 4,
              "sameMove() compares the first five bytes");
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "sameMove() masks the low five bytes");
inline bool sameMove(const Move& a, const Move& b) {
    uint64_t x, y;
    std::memcpy(&x, &a, sizeof x);
    std::memcpy(&y, &b, sizeof y);
    return ((x ^ y) & 0xFF'FFFF'FFFFull) == 0;
}

// Fixed-capacity move list: a stack array instead of a per-node heap
// allocation. 218 is the known maximum legal moves in any reachable chess
// position; 256 keeps headroom for pseudo-legal generation (which can
// include a handful of king-safety-violating moves beyond that count).
// Capacity is trusted, not runtime-checked (verified via perft under ASan
// instead — see benchmark_baseline.md).
class MoveList {
public:
    static constexpr int kCapacity = 256;

    MoveList() = default;
    // Copies only the moves in use: the full buffer is 8 KB, and a list is
    // built, returned and assigned several times per search node.
    MoveList(const MoveList& other) : count_(other.count_) { std::memcpy(raw_, other.raw_, bytes(count_)); }
    MoveList& operator=(const MoveList& other) {
        count_ = other.count_;
        std::memcpy(raw_, other.raw_, bytes(count_));
        return *this;
    }

    void push_back(const Move& m) { std::memcpy(raw_ + bytes(count_++), &m, sizeof(Move)); }
    size_t size() const { return static_cast<size_t>(count_); }
    bool empty() const { return count_ == 0; }
    void shrinkTo(size_t n) { count_ = static_cast<int>(n); } // keeps the first n moves
    const Move& front() const { return (*this)[0]; }
    const Move& operator[](size_t i) const { return data()[i]; }
    Move& operator[](size_t i) { return data()[i]; }

    Move* begin() { return data(); }
    Move* end() { return data() + count_; }
    const Move* begin() const { return data(); }
    const Move* end() const { return data() + count_; }

private:
    static size_t bytes(int moves) { return static_cast<size_t>(moves) * sizeof(Move); }
    Move* data() { return reinterpret_cast<Move*>(raw_); }
    const Move* data() const { return reinterpret_cast<const Move*>(raw_); }

    // Uninitialised storage (Move has default member initialisers, so a
    // Move[kCapacity] member would be filled in on every construction).
    alignas(Move) unsigned char raw_[kCapacity * sizeof(Move)];
    int count_ = 0;
};
