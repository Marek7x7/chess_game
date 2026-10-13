#pragma once

#include <cstddef>
#include <cstring>

#include "piece.hpp"

struct Move {
    int fromX, fromY;
    int toX, toY;
    PieceType promotion = PieceType::None;
    bool isCastle = false;
    bool isEnPassant = false;
    PieceType captured = PieceType::None;
};

inline bool sameMove(const Move& a, const Move& b) {
    return a.fromX == b.fromX && a.fromY == b.fromY && a.toX == b.toX && a.toY == b.toY &&
           a.promotion == b.promotion;
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
