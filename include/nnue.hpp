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
        int color, type, x, y; // colour 0 White / 1 Black, type as PieceType
    };
    Feature adds[2], subs[2];
    int numAdds = 0, numSubs = 0;
    void add(Color c, PieceType t, int x, int y) { adds[numAdds++] = {c == Color::White ? 0 : 1, static_cast<int>(t), x, y}; }
    void sub(Color c, PieceType t, int x, int y) { subs[numSubs++] = {c == Color::White ? 0 : 1, static_cast<int>(t), x, y}; }
};

// The accumulators along the line of moves being played: the top slot belongs to the current position. A move
// steps up and writes the new slot from the one below it; taking the move back just steps down, with no copying.
// Copying a stack copies only the slots in use.
class AccumulatorStack {
public:
    AccumulatorStack() : slots(1) {}
    AccumulatorStack(const AccumulatorStack& o) : slots(o.slots.begin(), o.slots.begin() + o.top + 1), top(o.top) {}
    AccumulatorStack& operator=(const AccumulatorStack& o) {
        if (this != &o) {
            slots.assign(o.slots.begin(), o.slots.begin() + o.top + 1);
            top = o.top;
        }
        return *this;
    }
    Accumulator& current() { return slots[top]; }
    const Accumulator& current() const { return slots[top]; }
    const Accumulator& below() const { return slots[top - 1]; } // after push()
    void push() { // invalidates references to the slots
        if (top + 1 == static_cast<int>(slots.size())) slots.resize(slots.size() * 2);
        ++top;
    }
    void pop() { --top; }

private:
    std::vector<Accumulator> slots;
    int top = 0;
};

// dst = src with `change` applied. They must be different accumulators.
void apply(Accumulator& dst, const Accumulator& src, const Change& change);

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
