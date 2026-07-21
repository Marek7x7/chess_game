#pragma once

#include "piece.hpp"

struct Move {
    int fromX, fromY;
    int toX, toY;
    PieceType promotion = PieceType::None;
    bool isCastle = false;
    bool isEnPassant = false;
    PieceType captured = PieceType::None;
};
