#pragma once

enum class Color { White, Black, None };

enum class PieceType { None, Pawn, Knight, Bishop, Rook, Queen, King };

struct Piece {
    PieceType type = PieceType::None;
    Color color = Color::None;

    bool isEmpty() const { return type == PieceType::None; }
    char symbol() const;
};

inline Color opponent(Color c) {
    return c == Color::White ? Color::Black : Color::White;
}
