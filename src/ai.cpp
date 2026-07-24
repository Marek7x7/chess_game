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
// Material + piece-square-table evaluation.
// ---------------------------------------------------------------------

int pieceValue(PieceType t) {
    switch (t) {
        case PieceType::Pawn: return 100;
        case PieceType::Knight: return 320;
        case PieceType::Bishop: return 330;
        case PieceType::Rook: return 500;
        case PieceType::Queen: return 900;
        default: return 0;
    }
}

// Standard "simplified evaluation function" piece-square tables (centipawns),
// written in published order: row 0 = rank 8, row 7 = rank 1, column 0 = file a.
constexpr int kPawnTable[8][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0},
    {50, 50, 50, 50, 50, 50, 50, 50},
    {10, 10, 20, 30, 30, 20, 10, 10},
    {5, 5, 10, 25, 25, 10, 5, 5},
    {0, 0, 0, 20, 20, 0, 0, 0},
    {5, -5, -10, 0, 0, -10, -5, 5},
    {5, 10, 10, -20, -20, 10, 10, 5},
    {0, 0, 0, 0, 0, 0, 0, 0},
};
constexpr int kKnightTable[8][8] = {
    {-50, -40, -30, -30, -30, -30, -40, -50},
    {-40, -20, 0, 0, 0, 0, -20, -40},
    {-30, 0, 10, 15, 15, 10, 0, -30},
    {-30, 5, 15, 20, 20, 15, 5, -30},
    {-30, 0, 15, 20, 20, 15, 0, -30},
    {-30, 5, 10, 15, 15, 10, 5, -30},
    {-40, -20, 0, 5, 5, 0, -20, -40},
    {-50, -40, -30, -30, -30, -30, -40, -50},
};
constexpr int kBishopTable[8][8] = {
    {-20, -10, -10, -10, -10, -10, -10, -20},
    {-10, 0, 0, 0, 0, 0, 0, -10},
    {-10, 0, 5, 10, 10, 5, 0, -10},
    {-10, 5, 5, 10, 10, 5, 5, -10},
    {-10, 0, 10, 10, 10, 10, 0, -10},
    {-10, 10, 10, 10, 10, 10, 10, -10},
    {-10, 5, 0, 0, 0, 0, 5, -10},
    {-20, -10, -10, -10, -10, -10, -10, -20},
};
constexpr int kRookTable[8][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0},
    {5, 10, 10, 10, 10, 10, 10, 5},
    {-5, 0, 0, 0, 0, 0, 0, -5},
    {-5, 0, 0, 0, 0, 0, 0, -5},
    {-5, 0, 0, 0, 0, 0, 0, -5},
    {-5, 0, 0, 0, 0, 0, 0, -5},
    {-5, 0, 0, 0, 0, 0, 0, -5},
    {0, 0, 0, 5, 5, 0, 0, 0},
};
constexpr int kQueenTable[8][8] = {
    {-20, -10, -10, -5, -5, -10, -10, -20},
    {-10, 0, 0, 0, 0, 0, 0, -10},
    {-10, 0, 5, 5, 5, 5, 0, -10},
    {-5, 0, 5, 5, 5, 5, 0, -5},
    {0, 0, 5, 5, 5, 5, 0, -5},
    {-10, 5, 5, 5, 5, 5, 0, -10},
    {-10, 0, 5, 0, 0, 0, 0, -10},
    {-20, -10, -10, -5, -5, -10, -10, -20},
};
constexpr int kKingMidTable[8][8] = {
    {-30, -40, -40, -50, -50, -40, -40, -30},
    {-30, -40, -40, -50, -50, -40, -40, -30},
    {-30, -40, -40, -50, -50, -40, -40, -30},
    {-30, -40, -40, -50, -50, -40, -40, -30},
    {-20, -30, -30, -40, -40, -30, -30, -20},
    {-10, -20, -20, -20, -20, -20, -20, -10},
    {20, 20, 0, 0, 0, 0, 20, 20},
    {20, 30, 10, 0, 0, 10, 30, 20},
};
constexpr int kKingEndTable[8][8] = {
    {-50, -40, -30, -20, -20, -30, -40, -50},
    {-30, -20, -10, 0, 0, -10, -20, -30},
    {-30, -10, 20, 30, 30, 20, -10, -30},
    {-30, -10, 30, 40, 40, 30, -10, -30},
    {-30, -10, 30, 40, 40, 30, -10, -30},
    {-30, -10, 20, 30, 30, 20, -10, -30},
    {-30, -30, 0, 0, 0, 0, -30, -30},
    {-50, -30, -30, -30, -30, -30, -30, -50},
};

// Looks up `table` (published rank-8-first order) for a piece of `color` on
// (x, y), where y=0 is rank 1 in Board's own coordinate system. Mirrors for
// Black so both sides use the same "toward the center / toward the back
// rank" pattern.
int pst(const int table[8][8], Color color, int x, int y) {
    int effectiveY = (color == Color::White) ? y : 7 - y;
    int row = 7 - effectiveY;
    return table[row][x];
}

int kingPst(Color color, int x, int y, double phase) {
    int mid = pst(kKingMidTable, color, x, y);
    int end = pst(kKingEndTable, color, x, y);
    return static_cast<int>(mid * phase + end * (1.0 - phase) + 0.5);
}

// 0 (endgame) .. 1 (opening/middlegame), based on remaining non-pawn material.
double gamePhase(const Board& board) {
    constexpr int kMaxPhase = 24; // 4 knights+4 bishops (1 each) + 4 rooks (2 each) + 2 queens (4 each)
    int phaseUnits = 0;
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            switch (board.at(x, y).type) {
                case PieceType::Knight:
                case PieceType::Bishop: phaseUnits += 1; break;
                case PieceType::Rook: phaseUnits += 2; break;
                case PieceType::Queen: phaseUnits += 4; break;
                default: break;
            }
        }
    }
    return std::min(1.0, phaseUnits / static_cast<double>(kMaxPhase));
}

// Positive = good for `color`.
int evaluate(const Board& board, Color color) {
    double phase = gamePhase(board);
    int score = 0; // positive = good for White
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            Piece p = board.at(x, y);
            if (p.isEmpty()) continue;
            int posBonus = 0;
            switch (p.type) {
                case PieceType::Pawn: posBonus = pst(kPawnTable, p.color, x, y); break;
                case PieceType::Knight: posBonus = pst(kKnightTable, p.color, x, y); break;
                case PieceType::Bishop: posBonus = pst(kBishopTable, p.color, x, y); break;
                case PieceType::Rook: posBonus = pst(kRookTable, p.color, x, y); break;
                case PieceType::Queen: posBonus = pst(kQueenTable, p.color, x, y); break;
                case PieceType::King: posBonus = kingPst(p.color, x, y, phase); break;
                default: break;
            }
            // Positional bonuses are scaled down relative to material so a
            // few plies of accumulated piece-square swings can't outweigh a
            // clean material gain (deep search will otherwise construct
            // exactly the line that exploits an oversized positional term).
            int total = pieceValue(p.type) + posBonus / 2;
            score += (p.color == Color::White) ? total : -total;
        }
    }
    return (color == Color::White) ? score : -score;
}

// ---------------------------------------------------------------------
// Search.
// ---------------------------------------------------------------------

constexpr int kMaxPly = 64;
constexpr int kMateScore = 1'000'000;
constexpr int kMateThreshold = kMateScore - 1000;
constexpr int kMaxCheckExtensions = 16;

// sizeof(TTEntry) is 56 bytes (measured). This table is shared by every
// search thread (see findBestMove) instead of being privately owned per
// thread, so it must absorb the *combined* node volume of every thread
// searching the full root position, not one thread's slice of it.
// Diagnosis against real games showed ~90-100M total nodes visited per
// move at EXPERT settings (12 threads x ~7-9M nodes each, under the old
// per-thread-subset design); 1<<22 (4,194,304) entries x 56 bytes ~= 224
// MiB comfortably covers that per-move working set, and is still a cheap,
// disposable allocation (rebuilt fresh every move, never persisted across
// the game). It's also a real upgrade over the old *aggregate* capacity
// (12 private tables x 1<<18 entries x 56 bytes ~= 168 MiB) despite being
// a single table: that old capacity was fragmented across threads that
// could never see each other's entries, so effectively none of it was
// shared; this consolidates it into one table every thread benefits from.
constexpr size_t kTTSize = size_t(1) << 22;

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

void orderMoves(const Board& board, std::vector<Move>& moves, const Move& ttMove, const SearchContext& ctx,
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

int negamax(const Board& board, int depth, int ply, int alpha, int beta, Color color, SearchContext& ctx,
            int extensionsLeft);

// Searches captures/promotions (all moves if in check) until the position is
// "quiet," to avoid evaluating mid-capture-sequence (the horizon effect).
int quiescence(const Board& board, int alpha, int beta, Color color, int ply, SearchContext& ctx) {
    ctx.nodes++;
    checkTime(ctx);
    if (ply >= kMaxPly - 1) return evaluate(board, color);

    bool inCheck = board.isInCheck(color);
    int standPat = 0;
    if (!inCheck) {
        standPat = evaluate(board, color);
        if (standPat >= beta) return standPat;
        if (standPat > alpha) alpha = standPat;
    }

    std::vector<Move> candidates;
    if (inCheck) {
        candidates = board.legalMoves(color);
        if (candidates.empty()) return -(kMateScore - ply);
    } else {
        for (const Move& m : board.legalMoves(color)) {
            if (m.captured != PieceType::None || m.promotion == PieceType::Queen) candidates.push_back(m);
        }
    }
    orderMoves(board, candidates, Move{}, ctx, ply, color);

    int best = inCheck ? INT_MIN : standPat;
    for (const Move& m : candidates) {
        Board copy = board;
        copy.makeMove(m);
        int score = -quiescence(copy, -beta, -alpha, opponent(color), ply + 1, ctx);
        if (score > best) best = score;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
    }
    return best;
}

// A checking move often only looks safe because the fixed-depth search
// stops right after it, one ply before the opponent's forced reply exposes
// that it actually lost material for nothing. Extending search depth by one
// ply whenever the side to move is in check lets the search see past that
// forcing sequence. `extensionsLeft` bounds the total extensions per line so
// a long forced-check sequence can't blow up recursion depth or search time.
int negamax(const Board& board, int depth, int ply, int alpha, int beta, Color color, SearchContext& ctx,
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

    std::vector<Move> moves = board.legalMoves(color);
    if (moves.empty()) {
        if (board.isInCheck(color)) return -(kMateScore - ply);
        return 0; // stalemate
    }
    orderMoves(board, moves, ttMove, ctx, ply, color);

    Move bestMove = moves.front();
    int best = INT_MIN;
    for (const Move& m : moves) {
        Board copy = board;
        copy.makeMove(m);
        int score = -negamax(copy, depth - 1, ply + 1, -beta, -alpha, opponent(color), ctx, extensionsLeft);
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
RootSearchResult searchRootFull(const Board& board, Color aiColor, const std::vector<Move>& rootMoves, int maxDepth,
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
                Board copy = board;
                copy.makeMove(m);
                int score =
                    -negamax(copy, depth - 1, 1, -beta, -alpha, opponent(aiColor), ctx, kMaxCheckExtensions);
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

Move findBestMove(const Board& board, Color aiColor, int maxDepth, int timeBudgetMs) {
    std::vector<Move> book = lookupBook(board);
    static std::mt19937 rng(std::random_device{}());
    if (!book.empty()) {
        std::uniform_int_distribution<size_t> dist(0, book.size() - 1);
        return book[dist(rng)];
    }

    std::vector<Move> rootMoves = board.legalMoves(aiColor);
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
    double phase = gamePhase(board);
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

    TranspositionTable sharedTT(kTTSize);

    std::vector<std::future<RootSearchResult>> futures;
    futures.reserve(numThreads);
    for (size_t t = 0; t < numThreads; t++) {
        futures.push_back(
            std::async(std::launch::async, [board, aiColor, rootMoves, effectiveMaxDepth, deadline, &sharedTT, t] {
                return searchRootFull(board, aiColor, rootMoves, effectiveMaxDepth, deadline, sharedTT, t);
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
