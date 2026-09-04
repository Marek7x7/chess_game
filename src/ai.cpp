#include "ai.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <future>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "opening_book.hpp"
#include "transposition_table.hpp"

bool g_aiDebugLog = false;

namespace {

std::string sqStr(int x, int y) {
    std::string s;
    s += static_cast<char>('a' + x);
    s += static_cast<char>('1' + y);
    return s;
}

char pieceLetter(PieceType t) {
    switch (t) {
        case PieceType::Knight: return 'N';
        case PieceType::Bishop: return 'B';
        case PieceType::Rook: return 'R';
        case PieceType::Queen: return 'Q';
        case PieceType::King: return 'K';
        default: return '?';
    }
}

std::string moveStr(const Move& m) {
    std::string s = sqStr(m.fromX, m.fromY) + sqStr(m.toX, m.toY);
    if (m.promotion != PieceType::None) {
        s += '=';
        s += pieceLetter(m.promotion);
    }
    return s;
}

std::string movesStr(const std::vector<Move>& moves) {
    std::string s;
    for (size_t i = 0; i < moves.size(); i++) {
        if (i) s += ' ';
        s += moveStr(moves[i]);
    }
    return s;
}

// ---------------------------------------------------------------------
// Evaluation. Material + piece-square-table score is maintained
// incrementally by Board itself (see Board::rawEval()); this just applies
// the side-to-move sign flip.
// ---------------------------------------------------------------------

// Positive = good for `color`.
int evaluate(const Board& board, Color color) {
    int score = board.rawEval(); // positive = good for White
    return (color == Color::White) ? score : -score;
}

// ---------------------------------------------------------------------
// Search.
// ---------------------------------------------------------------------

constexpr int kMaxPly = 64;
constexpr int kMateScore = 1'000'000;
constexpr int kMateThreshold = kMateScore - 1000;
constexpr int kMaxCheckExtensions = 16;

// kTTSize is declared in ai.hpp (needed there so callers can construct the
// persistent TranspositionTable they now own and pass into findBestMove).

struct TimeUp {};

struct SearchContext {
    TranspositionTable& tt;
    Move killers[kMaxPly][2]{};
    int history[2][8][8][8][8]{};
    long long nodes = 0;
    std::chrono::steady_clock::time_point deadline;
};

int colorIdx(Color c) { return c == Color::White ? 0 : 1; }

bool sameMove(const Move& a, const Move& b) {
    return a.fromX == b.fromX && a.fromY == b.fromY && a.toX == b.toX && a.toY == b.toY &&
           a.promotion == b.promotion;
}

// Score used to order moves before searching them: TT move first, then
// captures via MVV-LVA, then promotions, then killer moves, then history.
int moveScore(const Board& board, const Move& m, const Move& ttMove, const SearchContext& ctx, int ply,
              Color color) {
    if (sameMove(m, ttMove)) return 1'000'000;
    if (m.captured != PieceType::None) {
        return 100'000 + pieceValue(m.captured) * 10 - pieceValue(board.at(m.fromX, m.fromY).type);
    }
    if (m.promotion == PieceType::Queen) return 90'000;
    if (ply < kMaxPly) {
        if (sameMove(m, ctx.killers[ply][0])) return 80'000;
        if (sameMove(m, ctx.killers[ply][1])) return 79'000;
    }
    return ctx.history[colorIdx(color)][m.fromX][m.fromY][m.toX][m.toY];
}

void orderMoves(const Board& board, MoveList& moves, const Move& ttMove, const SearchContext& ctx,
                 int ply, Color color) {
    std::sort(moves.begin(), moves.end(), [&](const Move& a, const Move& b) {
        return moveScore(board, a, ttMove, ctx, ply, color) > moveScore(board, b, ttMove, ctx, ply, color);
    });
}

void checkTime(SearchContext& ctx) {
    if ((ctx.nodes & 0xFFF) == 0 && std::chrono::steady_clock::now() >= ctx.deadline) throw TimeUp{};
}

// Converts a score found `ply` levels below the search root into one
// relative to mate-in-N-from-root, for stable storage/lookup in the TT
// across different root distances.
int toTT(int score, int ply) {
    if (score > kMateThreshold) return score + ply;
    if (score < -kMateThreshold) return score - ply;
    return score;
}
int fromTT(int score, int ply) {
    if (score > kMateThreshold) return score - ply;
    if (score < -kMateThreshold) return score + ply;
    return score;
}

int negamax(Board& board, int depth, int ply, int alpha, int beta, Color color, SearchContext& ctx,
            int extensionsLeft);

// Searches captures/promotions (all moves if in check) until the position is
// "quiet," to avoid evaluating mid-capture-sequence (the horizon effect).
//
// Mutates `board` in place via make/unmake instead of copying it per move.
// If checkTime() throws TimeUp mid-recursion, the unwind skips the pending
// unmakeMove() calls on every frame between here and searchRootFull's catch,
// leaving `board` with unbalanced moves applied. That's safe only because
// the catch site in searchRootFull never reads `board` again afterward — if
// that ever changes, this would need a scope-guard around each make/unmake.
int quiescence(Board& board, int alpha, int beta, Color color, int ply, SearchContext& ctx) {
    ctx.nodes++;
    checkTime(ctx);
    if (ply >= kMaxPly - 1) return evaluate(board, color);

    // Quiescence's own conceptual search depth is 0, so any stored entry
    // (from a full negamax search or a prior quiescence visit) is at least
    // as deep -- the `ttEntry.depth >= depth` gate that guards negamax's
    // probe is unconditionally true here and is omitted accordingly.
    uint64_t key = board.hashKey();
    Move ttMove{};
    TTEntry ttEntry;
    if (ctx.tt.probe(key, ttEntry)) {
        if (ttEntry.hasMove) ttMove = ttEntry.best;
        int ttScore = fromTT(ttEntry.score, ply);
        if (ttEntry.flag == TTFlag::Exact) return ttScore;
        if (ttEntry.flag == TTFlag::LowerBound) alpha = std::max(alpha, ttScore);
        else if (ttEntry.flag == TTFlag::UpperBound) beta = std::min(beta, ttScore);
        if (alpha >= beta) return ttScore;
    }

    bool inCheck = board.isInCheck(color);
    int standPat = 0;
    if (!inCheck) {
        standPat = evaluate(board, color);
        if (standPat >= beta) return standPat;
        if (standPat > alpha) alpha = standPat;
    }

    MoveList candidates;
    if (inCheck) {
        candidates = board.legalMoves(color);
        if (candidates.empty()) return -(kMateScore - ply);
    } else {
        for (const Move& m : board.legalMoves(color)) {
            if (m.captured != PieceType::None || m.promotion == PieceType::Queen) candidates.push_back(m);
        }
    }
    orderMoves(board, candidates, ttMove, ctx, ply, color);

    int origAlpha = alpha;
    Move bestMove{};
    bool haveBestMove = false;
    int best = inCheck ? INT_MIN : standPat;
    for (const Move& m : candidates) {
        Board::UndoState undo;
        board.makeMove(m, undo);
        int score = -quiescence(board, -beta, -alpha, opponent(color), ply + 1, ctx);
        board.unmakeMove(m, undo);
        if (score > best) {
            best = score;
            bestMove = m;
            haveBestMove = true;
        }
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
    }

    TTFlag flag = best <= origAlpha ? TTFlag::UpperBound : (best >= beta ? TTFlag::LowerBound : TTFlag::Exact);
    ctx.tt.store(key, 0, toTT(best, ply), flag, bestMove, haveBestMove);
    return best;
}

// A checking move often only looks safe because the fixed-depth search
// stops right after it, one ply before the opponent's forced reply exposes
// that it actually lost material for nothing. Extending search depth by one
// ply whenever the side to move is in check lets the search see past that
// forcing sequence. `extensionsLeft` bounds the total extensions per line so
// a long forced-check sequence can't blow up recursion depth or search time.
int negamax(Board& board, int depth, int ply, int alpha, int beta, Color color, SearchContext& ctx,
            int extensionsLeft) {
    ctx.nodes++;
    checkTime(ctx);

    if (extensionsLeft > 0 && board.isInCheck(color)) {
        depth++;
        extensionsLeft--;
    }

    if (depth <= 0) return quiescence(board, alpha, beta, color, ply, ctx);

    int origAlpha = alpha;
    uint64_t key = board.hashKey();
    Move ttMove{};
    TTEntry ttEntry;
    if (ctx.tt.probe(key, ttEntry)) {
        if (ttEntry.hasMove) ttMove = ttEntry.best;
        if (ttEntry.depth >= depth) {
            int score = fromTT(ttEntry.score, ply);
            if (ttEntry.flag == TTFlag::Exact) return score;
            if (ttEntry.flag == TTFlag::LowerBound) alpha = std::max(alpha, score);
            else if (ttEntry.flag == TTFlag::UpperBound) beta = std::min(beta, score);
            if (alpha >= beta) return score;
        }
    }

    MoveList moves = board.legalMoves(color);
    if (moves.empty()) {
        if (board.isInCheck(color)) return -(kMateScore - ply);
        return 0; // stalemate
    }
    orderMoves(board, moves, ttMove, ctx, ply, color);

    Move bestMove = moves.front();
    int best = INT_MIN;
    bool firstMove = true;
    for (const Move& m : moves) {
        Board::UndoState undo;
        board.makeMove(m, undo);
        // PVS: the first move (typically the TT move / best-ordered guess)
        // gets the full window, since it's expected to be the actual best
        // move and we want its exact score. Every later move is searched
        // with a zero/null window first -- a cheap "is this better than
        // what we already have?" test -- and only re-searched with the
        // full window if it unexpectedly fails high (score > alpha),
        // meaning it might actually be better and its exact value matters.
        // With good move ordering (which this engine already has via
        // TT/MVV-LVA/killers/history), most non-first moves fail low on
        // the null window and never need the expensive full re-search.
        int score;
        if (firstMove) {
            score = -negamax(board, depth - 1, ply + 1, -beta, -alpha, opponent(color), ctx, extensionsLeft);
        } else {
            score = -negamax(board, depth - 1, ply + 1, -alpha - 1, -alpha, opponent(color), ctx, extensionsLeft);
            if (score > alpha && score < beta) {
                score = -negamax(board, depth - 1, ply + 1, -beta, -alpha, opponent(color), ctx, extensionsLeft);
            }
        }
        board.unmakeMove(m, undo);
        firstMove = false;
        if (score > best) {
            best = score;
            bestMove = m;
        }
        if (best > alpha) alpha = best;
        if (alpha >= beta) {
            if (m.captured == PieceType::None && ply < kMaxPly) {
                if (!sameMove(m, ctx.killers[ply][0])) {
                    ctx.killers[ply][1] = ctx.killers[ply][0];
                    ctx.killers[ply][0] = m;
                }
                ctx.history[colorIdx(color)][m.fromX][m.fromY][m.toX][m.toY] += depth * depth;
            }
            break;
        }
    }

    TTFlag flag = best <= origAlpha ? TTFlag::UpperBound : (best >= beta ? TTFlag::LowerBound : TTFlag::Exact);
    ctx.tt.store(key, depth, toTT(best, ply), flag, bestMove, true);
    return best;
}

struct RootSearchResult {
    Move move{};
    int score = INT_MIN;
    int depthReached = 0;
    long long nodes = 0;
};

// Only ever called to compare results that already reached the SAME depth
// (see findBestMove) — an 8-ply score from one search and a 9-ply score
// from another aren't comparable, so this deliberately doesn't try to
// arbitrate across different depths at all. Within a shared depth, a
// proven mate always wins; among two mates the faster one (higher score,
// since mate score is kMateScore-ply) wins; otherwise higher score wins.
bool isBetterAtSameDepth(const RootSearchResult& a, const RootSearchResult& b) {
    bool aMate = a.score > kMateThreshold;
    bool bMate = b.score > kMateThreshold;
    if (aMate != bMate) return aMate;
    return a.score > b.score;
}

// Runs a full iterative-deepening search over the *entire* root move list —
// every thread sees every candidate move, Lazy-SMP style — sharing `tt`
// with every other concurrently-running thread. A cutoff or refutation one
// thread stores becomes an instant pruning opportunity for every other
// thread searching the same subtree, instead of each thread redoing the
// same work in isolation. `threadIndex` seeds a small move-ordering jitter
// for threads other than 0, so helper threads don't all walk an identical
// tree in lockstep; thread 0 always searches with the unperturbed,
// TT/killer/history-driven ordering as the deterministic baseline. Jitter
// only ever reorders candidates — it never removes one from consideration
// at any depth — so every thread's search remains fully sound.
RootSearchResult searchRootFull(Board& board, Color aiColor, const std::vector<Move>& rootMoves, int maxDepth,
                                 std::chrono::steady_clock::time_point deadline, TranspositionTable& tt,
                                 size_t threadIndex) {
    RootSearchResult result;
    result.move = rootMoves.front();

    SearchContext ctx{tt, {}, {}, 0, deadline};
    std::mt19937 jitterRng(0x9E3779B9u + static_cast<unsigned>(threadIndex));
    std::uniform_int_distribution<int> jitterDist(0, 24);

    for (int depth = 1; depth <= maxDepth; depth++) {
        Move ttMoveAtRoot{};
        TTEntry rootEntry;
        if (tt.probe(board.hashKey(), rootEntry)) {
            if (rootEntry.hasMove) ttMoveAtRoot = rootEntry.best;
        }

        std::vector<std::pair<int, Move>> scored;
        scored.reserve(rootMoves.size());
        for (const Move& m : rootMoves) {
            int s = moveScore(board, m, ttMoveAtRoot, ctx, 0, aiColor);
            if (threadIndex != 0 && !sameMove(m, ttMoveAtRoot)) s += jitterDist(jitterRng);
            scored.push_back({s, m});
        }
        std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<Move> moves;
        moves.reserve(scored.size());
        for (const auto& sm : scored) moves.push_back(sm.second);

        int alpha = INT_MIN + 1, beta = INT_MAX - 1;
        Move depthBestMove = moves.front();
        int depthBest = INT_MIN;
        bool completed = true;
        try {
            for (const Move& m : moves) {
                Board::UndoState undo;
                board.makeMove(m, undo);
                int score =
                    -negamax(board, depth - 1, 1, -beta, -alpha, opponent(aiColor), ctx, kMaxCheckExtensions);
                board.unmakeMove(m, undo);
                if (score > depthBest) {
                    depthBest = score;
                    depthBestMove = m;
                }
                if (depthBest > alpha) alpha = depthBest;
            }
        } catch (const TimeUp&) {
            completed = false;
        }

        if (!completed) break;

        result.move = depthBestMove;
        result.score = depthBest;
        result.depthReached = depth;
        tt.store(board.hashKey(), depth, depthBest, TTFlag::Exact, depthBestMove, true);

        if (depthBest > kMateThreshold) break; // forced mate found, no need to search deeper
    }

    result.nodes = ctx.nodes;
    return result;
}

} // namespace

BenchResult benchSearch(const Board& board, Color color, int depth) {
    // searchRootFull() (below) is outside Step 3's scope and still takes a
    // std::vector<Move>&, so convert once here at the root (not a hot path).
    MoveList rootMoveList = board.legalMoves(color);
    std::vector<Move> rootMoves(rootMoveList.begin(), rootMoveList.end());
    BenchResult result;
    if (rootMoves.empty()) return result;

    Board local = board;
    TranspositionTable tt(kTTSize);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(24);
    RootSearchResult r = searchRootFull(local, color, rootMoves, depth, deadline, tt, 0);
    result.move = r.move;
    result.score = r.score;
    result.nodes = r.nodes;
    return result;
}

Move findBestMove(const Board& board, Color aiColor, int maxDepth, int timeBudgetMs, TranspositionTable& tt) {
    std::vector<Move> book = lookupBook(board);
    static std::mt19937 rng(std::random_device{}());
    if (!book.empty()) {
        // Cheap, unconditional safety net: even though hashKey() is a
        // complete encoding of position state (verified: pieces, side to
        // move, all four castling rights, en passant target), a book move
        // is only ever trustworthy if it's actually legal right now. Falls
        // through to normal search instead of returning a stale/bogus move
        // on any hash collision or book/board mismatch.
        MoveList legal = board.legalMoves(aiColor);
        std::vector<Move> legalBookMoves;
        for (const Move& bm : book) {
            for (const Move& lm : legal) {
                if (bm.fromX == lm.fromX && bm.fromY == lm.fromY && bm.toX == lm.toX && bm.toY == lm.toY &&
                    bm.promotion == lm.promotion) {
                    legalBookMoves.push_back(lm);
                    break;
                }
            }
        }
        if (!legalBookMoves.empty()) {
            std::uniform_int_distribution<size_t> dist(0, legalBookMoves.size() - 1);
            return legalBookMoves[dist(rng)];
        }
    }

    // searchRootFull() is outside Step 3's scope and still takes a
    // std::vector<Move>&, so convert once here at the root (not a hot path).
    MoveList rootMoveList = board.legalMoves(aiColor);
    std::vector<Move> rootMoves(rootMoveList.begin(), rootMoveList.end());
    if (rootMoves.size() == 1) return rootMoves.front();

    if (g_aiDebugLog) {
        std::fprintf(stderr, "[ai] findBestMove: color=%s maxDepth=%d timeBudgetMs=%d rootMoves=%zu\n",
                     aiColor == Color::White ? "White" : "Black", maxDepth, timeBudgetMs, rootMoves.size());
    }

    // Endgame positions have far fewer pieces and thus a much lower
    // branching factor, so the same time budget can afford looking several
    // plies further — exactly where accurate defense/king activity matters
    // most. Difficulty presets otherwise apply the same depth cap regardless
    // of material left on the board.
    double phase = board.gamePhase();
    int effectiveMaxDepth = maxDepth;
    if (phase < 0.5) effectiveMaxDepth += 2;
    if (phase < 0.2) effectiveMaxDepth += 2;

    if (g_aiDebugLog) {
        std::fprintf(stderr, "[ai] gamePhase=%.3f effectiveMaxDepth=%d\n", phase, effectiveMaxDepth);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeBudgetMs);

    // Use about half the machine's hardware threads (e.g. 12 of 24), leaving
    // headroom for the OS/UI thread. Every thread searches the FULL root
    // move list (Lazy-SMP), sharing one transposition table, so there's no
    // reason to cap thread count by the number of root moves anymore — that
    // cap only made sense when each thread owned a disjoint, non-empty
    // bucket to search.
    size_t numThreads = std::max(1u, std::thread::hardware_concurrency() / 2);

    if (g_aiDebugLog) {
        std::fprintf(stderr, "[ai] numThreads=%zu rootMoves: %s\n", numThreads, movesStr(rootMoves).c_str());
    }

    std::vector<std::future<RootSearchResult>> futures;
    futures.reserve(numThreads);
    for (size_t t = 0; t < numThreads; t++) {
        // Each thread needs its own mutable board to make/unmake moves into,
        // so capture a fresh non-const copy per thread rather than the
        // outer `const Board&` (capturing that by value would still carry
        // the const through into the closure, `mutable` notwithstanding).
        Board threadBoard = board;
        futures.push_back(std::async(
            std::launch::async,
            [threadBoard, aiColor, rootMoves, effectiveMaxDepth, deadline, &tt, t]() mutable {
                return searchRootFull(threadBoard, aiColor, rootMoves, effectiveMaxDepth, deadline, tt, t);
            }));
    }

    std::vector<RootSearchResult> results;
    results.reserve(futures.size());
    for (size_t t = 0; t < futures.size(); t++) {
        RootSearchResult r = futures[t].get();
        if (g_aiDebugLog) {
            std::fprintf(stderr, "[ai]   thread[%zu] depthReached=%d nodes=%lld score=%d move=%s\n", t,
                         r.depthReached, r.nodes, r.score, moveStr(r.move).c_str());
        }
        results.push_back(r);
    }

    // Aggregation only ever compares results that reached the same depth:
    // find the deepest depth any thread actually completed, then pick the
    // best score among just the threads that reached that depth. Since
    // every thread searched the full root move list, every candidate in
    // that comparison had access to every move — the comparison is finally
    // apples-to-apples, unlike the old disjoint-bucket scheme.
    int maxDepthReached = 0;
    for (const RootSearchResult& r : results) maxDepthReached = std::max(maxDepthReached, r.depthReached);

    RootSearchResult best;
    int bestThread = -1;
    int bestScoreAmongMaxDepth = INT_MIN;
    size_t threadsAtMaxDepth = 0;
    for (size_t t = 0; t < results.size(); t++) {
        const RootSearchResult& r = results[t];
        if (r.depthReached != maxDepthReached) continue;
        threadsAtMaxDepth++;
        bestScoreAmongMaxDepth = std::max(bestScoreAmongMaxDepth, r.score);
        if (bestThread == -1 || isBetterAtSameDepth(r, best)) {
            best = r;
            bestThread = static_cast<int>(t);
        }
    }

    if (g_aiDebugLog) {
        std::fprintf(stderr,
                     "[ai] maxDepthReached=%d (%zu/%zu threads) | winner=thread[%d] score=%d move=%s | best score "
                     "at that depth=%d%s\n",
                     maxDepthReached, threadsAtMaxDepth, results.size(), bestThread, best.score,
                     moveStr(best.move).c_str(), bestScoreAmongMaxDepth,
                     (bestThread != -1 && best.score != bestScoreAmongMaxDepth)
                         ? "  <-- WINNER IS NOT HIGHEST SCORE (BUG)"
                         : "");
        if (maxDepthReached == 0) {
            std::fprintf(stderr, "[ai] FALLBACK: no thread completed depth 1; returning rootMoves.front()=%s\n",
                         moveStr(rootMoves.front()).c_str());
        }
    }

    return maxDepthReached > 0 ? best.move : rootMoves.front();
}
