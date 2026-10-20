#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "board.hpp"
#include "move.hpp"
#include "transposition_table.hpp"

// Diagnostic-only switch: when true, findBestMove prints per-thread search
// diagnostics (depth reached, node counts, scores, root-move buckets, and
// the winning-thread rationale) to stderr. Off by default so normal GUI
// play is unaffected; the --replay driver in main.cpp turns it on.
extern bool g_aiDebugLog;

// Mate scores are encoded as kMateScore minus plies-to-mate (see ai.cpp);
// abs(score) > kMateThreshold means "this is a forced mate, not a normal
// centipawn eval." Exposed here so callers reporting scores (e.g. UCI's
// "score mate <n>") don't have to duplicate these magic numbers.
constexpr int kMateScore = 1'000'000;
constexpr int kMateThreshold = kMateScore - 1000;

// Entries are TranspositionTable::kEntryBytes (16) each, so 1<<22
// (4,194,304) entries is 64 MiB. This table is shared by every search
// thread within one findBestMove call (see below), so it must absorb the
// *combined* node volume of every thread searching the full root position,
// not one thread's slice of it (~90-100M nodes per move at EXPERT settings
// with 12 threads, measured before the search got its pruning). The table is meant to
// be constructed once per game/session and passed into findBestMove by
// reference (not rebuilt per move), so this size also has to be reasonable
// to hold for a whole game rather than just disposable per-call scratch.
constexpr size_t kTTSize = size_t(1) << 22;

// Reported once per completed iterative-deepening depth, from the "primary"
// search thread only (thread 0 -- the one with unperturbed, deterministic
// move ordering; other threads carry a small ordering jitter so they don't
// all walk an identical tree in lockstep). Lets a caller (UCI's "info"
// line, --replay's score/PV report) surface live search progress without
// findBestMove knowing anything about how that caller formats it.
struct SearchProgress {
    int depth = 0;
    int score = 0;      // centipawns, positive = good for aiColor
    long long nodes = 0; // cumulative nodes so far this call, over all search threads
    Move bestMove{};
};

// Every field's default reproduces findBestMove's pre-existing behavior
// exactly, so every pre-existing call site keeps compiling and behaving
// unchanged with an all-defaults SearchOptions{}.
struct SearchOptions {
    bool useOpeningBook = true; // false: skip the book, go straight to search (UCI's OwnBook=false default)
    int threadOverride = 0;     // 0 = auto (hardware_concurrency()-2, today's formula); >0 = use exactly this many
    // Not owned; caller must keep it alive until findBestMove returns (it's
    // synchronous/blocking from the caller's side even though it spawns
    // internal search threads -- they're all joined before it returns).
    const std::atomic<bool>* stopFlag = nullptr;
    std::function<void(const SearchProgress&)> onDepthComplete;
    // hashKey() of every position earlier in the game (including the one
    // being searched is harmless). The search scores reaching any of them again as a draw, so
    // a side that's ahead steers away from repeating instead of drifting
    // into a threefold. Empty = no game history known.
    std::vector<uint64_t> gameHistory;
    // > 0: once this many ms have passed, don't start another iterative-
    // deepening depth (timeBudgetMs stays the hard limit that aborts one
    // in progress). 0 = keep deepening until the hard limit.
    int softTimeMs = 0;
    // > 0: a search thread stops once it has searched this many nodes (UCI "go nodes"). The count is per
    // thread, so the limit is exact only with one thread.
    long long nodeLimit = 0;
    // Non-empty: only these root moves are searched (UCI "searchmoves"); ignored if none of them is legal.
    std::vector<Move> searchMoves;
    // Add 2 (then 4) plies to the depth cap in endgames, where the same time reaches further. The GUI's
    // depth-capped presets want that; a UCI "go depth N" means exactly N.
    bool endgameDepthBonus = true;
};

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
Move findBestMove(const Board& board, Color aiColor, int maxDepth, int timeBudgetMs, TranspositionTable& tt,
                   const SearchOptions& options = {});

// Reconstructs the principal variation for `rootBoard` by walking `tt` from
// the root position: repeatedly probe the current position's hash, follow
// the stored best move (re-checked for legality -- a hash collision could
// otherwise hand back a move that isn't legal here), apply it to a scratch
// copy, and stop at a TT miss, a stored-but-illegal/absent move, a repeated
// position (cycle guard), or `maxLen` moves. `history` holds the hashes of
// the game's earlier positions: the line also ends at a position that
// completes a threefold repetition with them, since the game would be over.
//
// Best-effort, not guaranteed-complete: tt entries can be overwritten
// between "search finished" and "this walk," which can truncate the PV
// early -- an accepted trade-off (the standard technique for this), not a
// bug. Read-only against `tt` (probes only, never stores); touches nothing
// in the negamax/quiescence hot path.
std::vector<Move> extractPV(const Board& rootBoard, const TranspositionTable& tt, int maxLen = 32,
                            const std::vector<uint64_t>& history = {});

// UCI-format move/score/PV text ("e7e8q" not "e7e8=Q", "cp <n>"/"mate <n>"
// not a bare centipawn integer). Shared by uci.cpp and --replay's
// score/PV reporting (main.cpp) so both produce identical formatting.
std::string formatUciMove(const Move& m);
std::string formatUciMoveList(const std::vector<Move>& moves);
std::string formatUciScore(int score);

struct BenchResult {
    Move move{};
    int score = 0;
    long long nodes = 0;
};

// Single-threaded, fixed-depth (no time cutoff) search used only for
// benchmarking and cross-step regression comparisons. Does not share any
// state with, or affect the behavior of, findBestMove.
BenchResult benchSearch(const Board& board, Color color, int depth);
