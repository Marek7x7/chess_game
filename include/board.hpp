#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <iosfwd>

#include "piece.hpp"
#include "move.hpp"

enum class GameStatus { Ongoing, Check, Checkmate, Stalemate };

class Board {
public:
    Board();

    // Parses Forward-Edwards Notation into a Board. For test/benchmark use
    // (setting up known positions such as perft test suites); trusts its
    // input to be well-formed since callers pass fixed literal FEN strings.
    static Board fromFEN(const std::string& fen);

    Piece at(int x, int y) const { return squares[x][y]; }
    Color sideToMove() const { return turn; }
    uint64_t hashKey() const { return hash; }

    // Material + piece-square-table evaluation, positive = good for White.
    // Non-king contributions are maintained incrementally in makeMove(); the
    // king's term is phase-interpolated and recomputed in O(1) from the
    // live-tracked king squares and phase (it can't be summed incrementally
    // like the other pieces, since its value changes with the game phase
    // even when the king itself doesn't move).
    int rawEval() const;

    // 0 (endgame) .. 1 (opening/middlegame), from incrementally-tracked
    // remaining non-pawn material.
    double gamePhase() const;

    // Recomputes rawEval() from scratch by scanning every square, ignoring
    // the incrementally-maintained fields entirely. Verification-only: used
    // to confirm the incremental score never drifts from a full recompute.
    int rawEvalFromScratch() const;

    static bool inBounds(int x, int y) { return x >= 0 && x < 8 && y >= 0 && y < 8; }

    // All moves for `color` that don't leave that color's own king in check.
    MoveList legalMoves(Color color) const;

    bool isSquareAttacked(int x, int y, Color byColor) const;
    bool isInCheck(Color color) const;

    // Applies a move produced by legalMoves(); does not re-validate legality.
    void makeMove(const Move& m);

    // State needed to exactly reverse one makeMove() call: whatever the move
    // itself doesn't already encode (captured piece type/color are on Move,
    // but the moving piece's original type is needed to undo promotions).
    struct UndoState {
        uint64_t prevHash;
        bool prevCastleKingside[2];
        bool prevCastleQueenside[2];
        int prevEnPassantX, prevEnPassantY;
        PieceType movedType; // Pawn if this move was a promotion
        Color movedColor;
        int prevNonKingScore;
        int prevPhaseUnits;
        int prevKingX[2], prevKingY[2];
    };

    // Same as makeMove(const Move&), but also records enough state in `undo`
    // to reverse the move with unmakeMove(). For hot search paths that would
    // otherwise copy the whole board per node.
    void makeMove(const Move& m, UndoState& undo);

    // Reverses the most recent makeMove(m, undo) call. `m` and `undo` must be
    // the exact pair from that call, applied with no other move in between.
    void unmakeMove(const Move& m, const UndoState& undo);

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

    // Running material+PST sum over every piece except kings, positive =
    // good for White; and running count of phase units (knight/bishop=1,
    // rook=2, queen=4) among remaining non-pawn, non-king material, used for
    // both gamePhase() and the king's phase-interpolated PST term.
    int nonKingScore;
    int phaseUnits;
    int kingX[2], kingY[2]; // [White, Black]

    static int colorIndex(Color c) { return c == Color::White ? 0 : 1; }

    void recomputeHash();
    void recomputeEval();
    int kingTerm() const; // White king's PST bonus minus Black's, at current phase
    MoveList pseudoLegalMoves(Color color) const;
    void addSlidingMoves(MoveList& moves, int x, int y, Color color,
                         const int dirs[][2], int numDirs) const;
    void addPawnMoves(MoveList& moves, int x, int y, Color color) const;
    void addKingMoves(MoveList& moves, int x, int y, Color color) const;
};
