#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nnue_arch.hpp"
#include "piece.hpp"

// Neural-network evaluation (see nnue_arch.hpp for the shape). A network is either compiled into the binary
// (cmake -DCHESS_NNUE_FILE=<path>) or loaded from a file at run time (the UCI option EvalFile). While one is
// loaded the engine evaluates with it instead of the hand-crafted terms.
namespace nnue {

// The accumulator: both sides' views of the position, [0] White's and [1] Black's. Board keeps one up to date
// as moves are made (only while a network is loaded).
struct alignas(64) Accumulator { // a cache line, so that 512-bit loads never straddle two
    int16_t v[2][kHidden];
};

// The feature changes of one move: pieces that left a square (subs) and pieces that arrived (adds). A move changes
// at most two of each (castling: king and rook; a capture or en passant: the mover and the victim).
struct Change {
    struct Feature {
        int8_t color, type, x, y; // colour 0 White / 1 Black, type as PieceType
    };
    Feature adds[2], subs[2];
    int8_t numAdds = 0, numSubs = 0;
    void add(Color c, PieceType t, int x, int y) {
        adds[numAdds++] = {static_cast<int8_t>(c == Color::White ? 0 : 1), static_cast<int8_t>(t), static_cast<int8_t>(x), static_cast<int8_t>(y)};
    }
    void sub(Color c, PieceType t, int x, int y) {
        subs[numSubs++] = {static_cast<int8_t>(c == Color::White ? 0 : 1), static_cast<int8_t>(t), static_cast<int8_t>(x), static_cast<int8_t>(y)};
    }
};

// dst = src with `change` applied. They must be different accumulators.
void apply(Accumulator& dst, const Accumulator& src, const Change& change);

// The accumulators along the line of moves being played: the top slot belongs to the current position. A move
// steps up and only records its feature changes; a slot is computed (from the one below it) when something asks
// for the current accumulator, so a move that is taken back, or a node that returns before it evaluates, costs no
// vector work (about half of all moves in a search). Taking a move back just steps down, with no copying.
// Copying a stack copies only the slots in use.
class AccumulatorStack {
public:
    AccumulatorStack() : slots(1), changes(1) {}
    AccumulatorStack(const AccumulatorStack& o)
        : slots(o.slots.begin(), o.slots.begin() + o.top + 1),
          changes(o.changes.begin(), o.changes.begin() + o.top + 1),
          top(o.top),
          clean(o.clean) {}
    AccumulatorStack& operator=(const AccumulatorStack& o) {
        if (this != &o) {
            slots.assign(o.slots.begin(), o.slots.begin() + o.top + 1);
            changes.assign(o.changes.begin(), o.changes.begin() + o.top + 1);
            top = o.top;
            clean = o.clean;
        }
        return *this;
    }
    // The current position's accumulator, computed first if it is still pending.
    Accumulator& current() {
        while (clean < top) {
            ++clean;
            apply(slots[clean], slots[clean - 1], changes[clean]);
        }
        return slots[top];
    }
    // The record for the next move, cleared, to be filled in before push() (so that no copy is needed).
    // Invalidates references to the slots.
    Change& next() {
        if (top + 1 == static_cast<int>(slots.size())) {
            slots.resize(slots.size() * 2);
            changes.resize(changes.size() * 2);
        }
        Change& c = changes[top + 1];
        c.numAdds = c.numSubs = 0;
        return c;
    }
    void push() { ++top; }
    void pop() {
        if (--top < clean) clean = top;
    }

private:
    std::vector<Accumulator> slots;
    std::vector<Change> changes; // changes[i] turns slot i - 1 into slot i
    int top = 0;
    int clean = 0; // slots 0..clean are computed; the ones above are still pending
};

// True while a network is loaded and in use.
bool active();
// Loads a network file. On failure returns false with the reason in `error` and leaves the current evaluation
// as it was. The caller must not be searching.
bool loadFile(const std::string& path, std::string& error);
// Uses the network built into the binary, if there is one.
bool loadBuiltin();
// Fills the network with deterministic pseudo-random weights (for tests that need some network).
void loadRandom(uint64_t seed);
// Back to the hand-crafted evaluation.
void unload();
// A short description of what is loaded ("none", "builtin", the file name, "random").
const std::string& netName();

// An accumulator holding no pieces (just the biases).
void reset(Accumulator& acc);
// Adds or removes the feature "a piece of colour c and type t on file x, rank y".
void add(Accumulator& acc, Color c, PieceType t, int x, int y);
void sub(Accumulator& acc, Color c, PieceType t, int x, int y);
// The evaluation, in centipawns from the point of view of `sideToMove`, for a position with the given
// accumulator and number of pieces on the board.
int evaluate(const Accumulator& acc, Color sideToMove, int pieceCount);
// Which instruction set evaluate() was compiled for: "avx512-vnni", "avx512", "avx2" or "scalar".
const char* kernelName();

} // namespace nnue
