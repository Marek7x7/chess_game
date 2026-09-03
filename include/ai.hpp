#pragma once

#include "board.hpp"
#include "move.hpp"
#include "transposition_table.hpp"

// Diagnostic-only switch: when true, findBestMove prints per-thread search
// diagnostics (depth reached, node counts, scores, root-move buckets, and
// the winning-thread rationale) to stderr. Off by default so normal GUI
// play is unaffected; the --replay driver in main.cpp turns it on.
extern bool g_aiDebugLog;

// sizeof(TTEntry) is 56 bytes (measured). This table is shared by every
// search thread within one findBestMove call (see below), so it must absorb
// the *combined* node volume of every thread searching the full root
// position, not one thread's slice of it. Diagnosis against real games
// showed ~90-100M total nodes visited per move at EXPERT settings (12
// threads x ~7-9M nodes each); 1<<22 (4,194,304) entries x 56 bytes ~= 224
// MiB comfortably covers that per-move working set. The table is meant to
// be constructed once per game/session and passed into findBestMove by
// reference (not rebuilt per move), so this size also has to be reasonable
// to hold for a whole game rather than just disposable per-call scratch.
constexpr size_t kTTSize = size_t(1) << 22;

// Picks a move for `aiColor`: an opening-book move if the position is a
// known line, otherwise an iterative-deepening alpha-beta search (with
// quiescence search, a transposition table, and move ordering) bounded by
// both `maxDepth` and `timeBudgetMs`, whichever is hit first. Caller must
// ensure aiColor has at least one legal move.
//
// `tt` should be owned by whatever persists across a whole game (not
// constructed fresh per call): later calls reuse entries earlier calls
// stored, particularly valuable when the opponent plays a predicted
// response. It is never cleared here; old entries simply age out as new
// searches overwrite them.
Move findBestMove(const Board& board, Color aiColor, int maxDepth, int timeBudgetMs, TranspositionTable& tt);

struct BenchResult {
    Move move{};
    int score = 0;
    long long nodes = 0;
};

// Single-threaded, fixed-depth (no time cutoff) search used only for
// benchmarking and cross-step regression comparisons. Does not share any
// state with, or affect the behavior of, findBestMove.
BenchResult benchSearch(const Board& board, Color color, int depth);
