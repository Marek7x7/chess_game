#include "ai.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <future>
#include <random>
#include <thread>
#include <vector>

#include "opening_book.hpp"
#include "transposition_table.hpp"

namespace {

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

int negamax(const Board& board, int depth, int ply, int alpha, int beta, Color color, SearchContext& ctx);

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

int negamax(const Board& board, int depth, int ply, int alpha, int beta, Color color, SearchContext& ctx) {
    ctx.nodes++;
    checkTime(ctx);

    if (depth <= 0) return quiescence(board, alpha, beta, color, ply, ctx);

    int origAlpha = alpha;
    uint64_t key = board.hashKey();
    Move ttMove{};
    if (const TTEntry* e = ctx.tt.probe(key)) {
        if (e->hasMove) ttMove = e->best;
        if (e->depth >= depth) {
            int score = fromTT(e->score, ply);
            if (e->flag == TTFlag::Exact) return score;
            if (e->flag == TTFlag::LowerBound) alpha = std::max(alpha, score);
            else if (e->flag == TTFlag::UpperBound) beta = std::min(beta, score);
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
        int score = -negamax(copy, depth - 1, ply + 1, -beta, -alpha, opponent(color), ctx);
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
};

// A thread that proves a forced mate stops iterating early (it has nothing
// left to prove), so it will often report a *lower* depthReached than a
// thread still grinding through a subset with no decisive line. A mate
// always wins regardless of depthReached; among two mates, the faster one
// (higher score, since mate score is kMateScore-ply) wins; otherwise prefer
// the deeper, more trustworthy search.
bool isBetterRootResult(const RootSearchResult& a, const RootSearchResult& b) {
    if (a.depthReached == 0) return false;
    if (b.depthReached == 0) return true;
    bool aMate = a.score > kMateThreshold;
    bool bMate = b.score > kMateThreshold;
    if (aMate != bMate) return aMate;
    if (aMate && bMate) return a.score > b.score;
    if (a.depthReached != b.depthReached) return a.depthReached > b.depthReached;
    return a.score > b.score;
}

// Runs its own iterative-deepening search (own TT, own move ordering state)
// over just `subset` of the root moves. Splitting the root move list across
// threads this way needs no synchronization at all: each thread only ever
// reads the shared starting position and writes to its own local state.
RootSearchResult searchRootSubset(const Board& board, Color aiColor, const std::vector<Move>& subset, int maxDepth,
                                   std::chrono::steady_clock::time_point deadline) {
    RootSearchResult result;
    result.move = subset.front();

    TranspositionTable tt(1 << 18);
    SearchContext ctx{tt, {}, {}, 0, deadline};

    for (int depth = 1; depth <= maxDepth; depth++) {
        Move ttMoveAtRoot{};
        if (const TTEntry* e = tt.probe(board.hashKey())) {
            if (e->hasMove) ttMoveAtRoot = e->best;
        }
        std::vector<Move> moves = subset;
        orderMoves(board, moves, ttMoveAtRoot, ctx, 0, aiColor);

        int alpha = INT_MIN + 1, beta = INT_MAX - 1;
        Move depthBestMove = moves.front();
        int depthBest = INT_MIN;
        bool completed = true;
        try {
            for (const Move& m : moves) {
                Board copy = board;
                copy.makeMove(m);
                int score = -negamax(copy, depth - 1, 1, -beta, -alpha, opponent(aiColor), ctx);
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

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeBudgetMs);

    // Use about half the machine's hardware threads (e.g. 12 of 24), leaving
    // headroom for the OS/UI thread, and never more threads than there are
    // root moves to hand out.
    size_t numThreads = std::max(1u, std::thread::hardware_concurrency() / 2);
    numThreads = std::min(numThreads, rootMoves.size());

    std::vector<std::vector<Move>> buckets(numThreads);
    for (size_t i = 0; i < rootMoves.size(); i++) buckets[i % numThreads].push_back(rootMoves[i]);

    std::vector<std::future<RootSearchResult>> futures;
    futures.reserve(numThreads);
    for (size_t t = 0; t < numThreads; t++) {
        std::vector<Move> subset = buckets[t];
        futures.push_back(std::async(std::launch::async, [board, aiColor, subset, maxDepth, deadline] {
            return searchRootSubset(board, aiColor, subset, maxDepth, deadline);
        }));
    }

    RootSearchResult best;
    for (std::future<RootSearchResult>& f : futures) {
        RootSearchResult r = f.get();
        if (isBetterRootResult(r, best)) best = r;
    }

    return best.depthReached > 0 ? best.move : rootMoves.front();
}
