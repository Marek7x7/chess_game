#pragma once

#include <cstddef>

#include "piece.hpp"

struct Move {
    int fromX, fromY;
    int toX, toY;
    PieceType promotion = PieceType::None;
    bool isCastle = false;
    bool isEnPassant = false;
    PieceType captured = PieceType::None;
};

// Fixed-capacity move list: a stack array instead of a per-node heap
// allocation. 218 is the known maximum legal moves in any reachable chess
// position; 256 keeps headroom for pseudo-legal generation (which can
// include a handful of king-safety-violating moves beyond that count).
// Capacity is trusted, not runtime-checked (verified via perft under ASan
// instead — see benchmark_baseline.md).
class MoveList {
public:
    static constexpr int kCapacity = 256;

    void push_back(const Move& m) { moves_[count_++] = m; }
    size_t size() const { return static_cast<size_t>(count_); }
    bool empty() const { return count_ == 0; }
    const Move& front() const { return moves_[0]; }
    const Move& operator[](size_t i) const { return moves_[i]; }
    Move& operator[](size_t i) { return moves_[i]; }

    Move* begin() { return moves_; }
    Move* end() { return moves_ + count_; }
    const Move* begin() const { return moves_; }
    const Move* end() const { return moves_ + count_; }

private:
    Move moves_[kCapacity];
    int count_ = 0;
};
