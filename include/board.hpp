#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <iosfwd>

#include "nnue.hpp"
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

    // For input that may be malformed or illegal (a UCI "position fen"): returns false, leaving `out`
    // untouched, unless the text is a well-formed FEN of a legal setup (one king each, no pawn on a
    // back rank, the side not to move not in check, plausible piece counts). The castling field may be
    // missing and is cut down to the rights the pieces allow; so is an en passant square no pawn can use.
    static bool tryFromFEN(const std::string& fen, Board& out);

    // The position as a FEN string (the move number is always written as 1).
    std::string toFEN() const;

    Piece at(int x, int y) const { return squares[x][y]; }

    // Bitboards (see bitboard.hpp) of where `color`'s pieces of a type stand, of
    // all of one color's pieces, and of every piece. Kept in step with squares[][]
    // by setSquare(), the only place a square is written.
    uint64_t pieces(Color color, PieceType type) const { return pieceBB[colorIndex(color)][static_cast<int>(type)]; }
    uint64_t occupancy(Color color) const { return occBB[colorIndex(color)]; }
    uint64_t occupancy() const { return occBB[0] | occBB[1]; }
    // Verification only: do the bitboards match squares[][]?
    bool bitboardsConsistent() const;
    Color sideToMove() const { return turn; }
    int kingFile(Color c) const { return kingX[colorIndex(c)]; }
    int kingRank(Color c) const { return kingY[colorIndex(c)]; }
    // Plies since the last capture or pawn move (the fifty-move-rule count).
    int halfmoveClock() const { return halfmoveCount; }
    uint64_t hashKey() const { return hash; }
    // Zobrist hash of the pawns alone (both colors), for caching evaluation
    // terms that depend only on where the pawns are.
    uint64_t pawnKey() const { return pawnHash; }

    // Material + piece-square-table evaluation, positive = good for White.
    // Non-king contributions are maintained incrementally in makeMove(); the
    // king's term is phase-interpolated and recomputed in O(1) from the
    // live-tracked king squares and phase (it can't be summed incrementally
    // like the other pieces, since its value changes with the game phase
    // even when the king itself doesn't move). Same for the bishop-pair
    // bonus, from the live-tracked bishop count of each side, and for the
    // endgame pawn values, from a live-tracked sum.
    int rawEval() const;

    // 0 (endgame) .. 1 (opening/middlegame), from incrementally-tracked
    // remaining non-pawn material.
    double gamePhase() const;

    // The neural-network accumulator for the current position. Kept up to date by makeMove()/unmakeMove() while a
    // network is loaded (nnue::active()); refreshNnue() rebuilds it, which is needed after a network gets loaded
    // for a board that already exists. nnueMatchesScratch() is verification only.
    const nnue::Accumulator& nnueAccumulator() const { return nnueAcc; }
    void refreshNnue();
    bool nnueMatchesScratch() const;

    // Recomputes rawEval() from scratch by scanning every square, ignoring
    // the incrementally-maintained fields entirely. Verification-only: used
    // to confirm the incremental score never drifts from a full recompute.
    int rawEvalFromScratch() const;

    // Recompute hashKey() / pawnKey() from scratch -- verification only,
    // like rawEvalFromScratch().
    uint64_t hashKeyFromScratch() const;
    uint64_t pawnKeyFromScratch() const;

    // One unsigned compare: a negative coordinate sets the sign bit of x | y, and
    // one that is 8 or more sets a bit above the low three.
    static bool inBounds(int x, int y) { return static_cast<unsigned>(x | y) < 8u; }

    // All moves for `color` that don't leave that color's own king in check.
    MoveList legalMoves(Color color) const;

    // The subset of legalMoves() that quiescence search looks at -- every
    // capture (en passant and capture-promotions included) plus non-capture
    // queen promotions -- in the same relative order legalMoves() lists
    // them, without paying the legality check for every quiet move.
    MoveList legalNoisyMoves(Color color) const;

    // legalMoves() / legalNoisyMoves() before the legality filter, same order: outside check
    // the extra moves are the few that leave the king attacked (a pinned piece leaving its
    // line, a king stepping onto an attacked square). A caller that tests each move it
    // actually plays (makeMove, then isInCheck of the mover) skips the filter for the
    // moves it never reaches. In check, use legalMoves(): nearly every move is illegal then.
    MoveList pseudoLegal(Color color, bool noisyOnly) const { return pseudoLegalMoves(color, noisyOnly); }

    // Bit x * 8 + y of every piece of `color` pinned against its king by an enemy slider: moving it
    // off the pin line leaves the king attacked. (Meaningful when the king is not in check.)
    uint64_t pinnedPieces(Color color) const;

    bool isSquareAttacked(int x, int y, Color byColor) const;
    bool isInCheck(Color color) const;

    // Applies a move produced by legalMoves(); does not re-validate legality.
    void makeMove(const Move& m);

    // State needed to exactly reverse one makeMove() call: whatever the move
    // itself doesn't already encode (captured piece type/color are on Move,
    // but the moving piece's original type is needed to undo promotions).
    struct UndoState {
        uint64_t prevHash;
        uint64_t prevPawnHash;
        bool prevCastleKingside[2];
        bool prevCastleQueenside[2];
        int prevEnPassantX, prevEnPassantY;
        PieceType movedType; // Pawn if this move was a promotion
        Color movedColor;
        int prevNonKingScore;
        int prevPhaseUnits;
        int prevKingX[2], prevKingY[2];
        int prevBishopCount[2];
        int prevEgShift;
        int prevHalfmoveCount;
        nnue::Accumulator prevAcc; // only filled while a network is loaded
    };

    // Same as makeMove(const Move&), but also records enough state in `undo`
    // to reverse the move with unmakeMove(). For hot search paths that would
    // otherwise copy the whole board per node.
    void makeMove(const Move& m, UndoState& undo);

    // Reverses the most recent makeMove(m, undo) call. `m` and `undo` must be
    // the exact pair from that call, applied with no other move in between.
    void unmakeMove(const Move& m, const UndoState& undo);

    // "Passes" -- flips the side to move without moving any piece, for
    // null-move pruning. Clears the en passant target the same way a real
    // move would (the capture window is always exactly one ply). Piece
    // placement, material/eval, and king squares are all unaffected since
    // nothing moved.
    struct NullMoveUndo {
        uint64_t prevHash;
        int prevEnPassantX, prevEnPassantY;
    };
    void makeNullMove(NullMoveUndo& undo);
    void unmakeNullMove(const NullMoveUndo& undo);

    // True if `color` has any piece other than pawns/king. Used to guard
    // null-move pruning: passing is unsound in king+pawn endgames (the side
    // to move is often in zugzwang there -- any move, including a "free"
    // one, makes the position worse), so null-move must never be tried when
    // this is false.
    bool hasNonPawnMaterial(Color color) const;

    GameStatus status() const; // status of the side to move

    // Finds the legal move matching the given squares/promotion, or nullptr fields if none.
    bool findLegalMove(int fromX, int fromY, int toX, int toY, PieceType promotion, Move& out) const;

    void print(std::ostream& os) const;

private:
    Piece squares[8][8];
    uint64_t pieceBB[2][7]; // [color][PieceType]; index 0 (None) is unused
    uint64_t occBB[2];      // [color]
    Color turn;
    bool castleKingside[2];
    bool castleQueenside[2];
    int enPassantX, enPassantY; // target square a capturing pawn would move to; (-1,-1) if none
    uint64_t hash;
    uint64_t pawnHash; // see pawnKey()
    int halfmoveCount = 0;

    // Running material+PST sum over every piece except kings, positive =
    // good for White; and running count of phase units (knight/bishop=1,
    // rook=2, queen=4) among remaining non-pawn, non-king material, used for
    // both gamePhase() and the king's phase-interpolated PST term.
    int nonKingScore;
    int phaseUnits;
    int kingX[2], kingY[2]; // [White, Black]
    int bishopCount[2];     // [White, Black], for the bishop-pair bonus
    int egShift;            // sum of egShiftFor() over all pieces, blended in by phase
    nnue::Accumulator nnueAcc; // see nnueAccumulator()

    static int colorIndex(Color c) { return c == Color::White ? 0 : 1; }

    void setSquare(int x, int y, Piece piece); // writes squares[x][y] and both bitboards
    void recomputeBitboards();
    void recomputeHash();
    void computeHashes(uint64_t& full, uint64_t& pawns) const;
    void recomputeEval();
    int kingTerm() const; // White king's PST bonus minus Black's, at current phase

    // Lightweight make/unmake for pure legality filtering (legalMoves()):
    // updates piece placement and king-square tracking only -- the minimum
    // isInCheck() needs. Deliberately does NOT touch the Zobrist hash,
    // incremental eval score, castling rights, or en-passant target, since
    // legalMoves() immediately reverses the move after one isInCheck() call
    // and never needs any of that state. Do not use these outside a
    // make-check-unmake sequence that never observes hashKey()/rawEval()/
    // castling or en-passant state in between -- they are intentionally
    // left stale/wrong while a light move is applied.
    struct LightUndo {
        PieceType movedType; // Pawn if this move was a promotion
        Color movedColor;
    };
    void makeMoveLight(const Move& m, LightUndo& undo);
    void unmakeMoveLight(const Move& m, const LightUndo& undo);

    // noisyOnly: generate only what legalNoisyMoves() returns.
    MoveList pseudoLegalMoves(Color color, bool noisyOnly) const;
    MoveList filterLegal(const MoveList& pseudo, Color color) const;
    // firstDir/numDirs index bb::rays: bishop directions 0-3, rook directions 4-7.
    void addSlidingMoves(MoveList& moves, int x, int y, Color color, int firstDir, int numDirs, bool noisyOnly) const;
    void addPawnMoves(MoveList& moves, int x, int y, Color color, bool noisyOnly) const;
    void addKingMoves(MoveList& moves, int x, int y, Color color, bool noisyOnly) const;
};
