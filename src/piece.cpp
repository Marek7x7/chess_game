#include "piece.hpp"

char Piece::symbol() const {
    char c = '.';
    switch (type) {
        case PieceType::Pawn:   c = 'p'; break;
        case PieceType::Knight: c = 'n'; break;
        case PieceType::Bishop: c = 'b'; break;
        case PieceType::Rook:   c = 'r'; break;
        case PieceType::Queen:  c = 'q'; break;
        case PieceType::King:   c = 'k'; break;
        default: return '.';
    }
    return color == Color::White ? static_cast<char>(c - 'a' + 'A') : c;
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
