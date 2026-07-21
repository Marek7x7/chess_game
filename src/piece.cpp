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
