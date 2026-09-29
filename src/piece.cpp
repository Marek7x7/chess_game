#include "piece.hpp"

#include <cctype>

char Piece::symbol() const {
    char c = ".pnbrqk"[static_cast<int>(type)];
    return color == Color::White ? static_cast<char>(std::toupper(c)) : c;
}

int pieceValue(PieceType t) {
    switch (t) {
        case PieceType::Pawn: return 100;
        case PieceType::Knight: return 320;
        case PieceType::Bishop: return 330;
        case PieceType::Rook: return 500;
        case PieceType::Queen: return 900;
        default: return 0;
    }
}
