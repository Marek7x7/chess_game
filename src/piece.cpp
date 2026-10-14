#include "piece.hpp"

#include <cctype>

#include "tune.hpp"

char Piece::symbol() const {
    char c = ".pnbrqk"[static_cast<int>(type)];
    return color == Color::White ? static_cast<char>(std::toupper(c)) : c;
}

// Material values in centipawns, indexed by PieceType (None, Pawn, Knight,
// Bishop, Rook, Queen, King). The pawn's 100 is the unit everything else is
// measured in, so it is not tuned.
TUNE_ARR(kPieceValue, 7, {0, 100, 326, 346, 573, 1100, 0});

int pieceValue(PieceType t) { return kPieceValue[static_cast<int>(t)]; }
