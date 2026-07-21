#pragma once

#include <cstdint>
#include <vector>
#include <iosfwd>

#include "piece.hpp"
#include "move.hpp"

enum class GameStatus { Ongoing, Check, Checkmate, Stalemate };

class Board {
public:
    Board();

    Piece at(int x, int y) const { return squares[x][y]; }
    Color sideToMove() const { return turn; }
    uint64_t hashKey() const { return hash; }

    static bool inBounds(int x, int y) { return x >= 0 && x < 8 && y >= 0 && y < 8; }

    // All moves for `color` that don't leave that color's own king in check.
    std::vector<Move> legalMoves(Color color) const;

    bool isSquareAttacked(int x, int y, Color byColor) const;
    bool isInCheck(Color color) const;

    // Applies a move produced by legalMoves(); does not re-validate legality.
    void makeMove(const Move& m);

    GameStatus status() const; // status of the side to move

    // Finds the legal move matching the given squares/promotion, or nullptr fields if none.
    bool findLegalMove(int fromX, int fromY, int toX, int toY, PieceType promotion, Move& out) const;

    void print(std::ostream& os) const;

private:
    Piece squares[8][8];
    Color turn;
    bool castleKingside[2];
    bool castleQueenside[2];
    int enPassantX, enPassantY; // target square a capturing pawn would move to; (-1,-1) if none
    uint64_t hash;

    static int colorIndex(Color c) { return c == Color::White ? 0 : 1; }

    std::vector<Move> pseudoLegalMoves(Color color) const;
    void addSlidingMoves(std::vector<Move>& moves, int x, int y, Color color,
                         const int dirs[][2], int numDirs) const;
    void addPawnMoves(std::vector<Move>& moves, int x, int y, Color color) const;
    void addKingMoves(std::vector<Move>& moves, int x, int y, Color color) const;
};
