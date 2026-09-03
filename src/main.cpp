#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "ai.hpp"
#include "board.hpp"
#include "gui.hpp"
#include "pgn.hpp"

namespace {

long long perft(const Board& board, int depth, Color color) {
    if (depth == 0) return 1;
    long long nodes = 0;
    for (const Move& m : board.legalMoves(color)) {
        Board copy = board;
        copy.makeMove(m);
        nodes += perft(copy, depth - 1, opponent(color));
    }
    return nodes;
}

// Same tree as perft(), but walked via make/unmake in place instead of a
// per-node board copy — exercises exactly the Board::makeMove(m, undo) /
// unmakeMove(m, undo) pair that negamax/quiescence/searchRootFull use, which
// plain perft() (still copy-based) never touches. Any node-count mismatch
// against perft() pinpoints an unmake bug (wrong castling-rights/en-passant/
// captured-piece restoration) rather than a move-generation bug.
long long perftUnmake(Board& board, int depth, Color color) {
    if (depth == 0) return 1;
    long long nodes = 0;
    for (const Move& m : board.legalMoves(color)) {
        Board::UndoState undo;
        board.makeMove(m, undo);
        nodes += perftUnmake(board, depth - 1, opponent(color));
        board.unmakeMove(m, undo);
    }
    return nodes;
}

int runPerft(int maxDepth) {
    Board board;
    for (int depth = 1; depth <= maxDepth; depth++) {
        long long nodes = perft(board, depth, board.sideToMove());
        std::printf("perft(%d) = %lld\n", depth, nodes);
    }
    return 0;
}

// Standard perft test suite (Chess Programming Wiki "Perft Results"), used
// as the move-generation safety net: known-correct node counts at each
// depth catch make/unmake and legality bugs that the start position alone
// (too symmetric, too few castling/en-passant edge cases at low depth)
// would miss.
struct PerftCase {
    const char* name;
    const char* fen;
    std::vector<long long> expected; // expected[i] = perft(i+1)
};

const std::vector<PerftCase>& perftSuite() {
    static const std::vector<PerftCase> suite = {
        {"startpos", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
         {20, 400, 8902, 197281, 4865609}},
        {"kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
         {48, 2039, 97862, 4085603}},
        {"endgame", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", {14, 191, 2812, 43238, 674624}},
    };
    return suite;
}

int runPerftSuite(int maxDepth) {
    bool allPass = true;
    for (const PerftCase& tc : perftSuite()) {
        Board board = Board::fromFEN(tc.fen);
        int depthLimit = std::min<int>(maxDepth, static_cast<int>(tc.expected.size()));
        for (int depth = 1; depth <= depthLimit; depth++) {
            auto start = std::chrono::steady_clock::now();
            long long nodes = perft(board, depth, board.sideToMove());
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

            Board unmakeBoard = board;
            long long nodesUnmake = perftUnmake(unmakeBoard, depth, unmakeBoard.sideToMove());

            long long want = tc.expected[depth - 1];
            bool pass = nodes == want && nodesUnmake == want;
            allPass &= pass;
            std::printf("%-10s perft(%d) = %-12lld unmake=%-12lld expected %-12lld %s  (%.2fs)\n", tc.name, depth,
                        nodes, nodesUnmake, want, pass ? "OK" : "MISMATCH", secs);
        }
    }
    std::printf(allPass ? "PERFT SUITE: ALL PASS\n" : "PERFT SUITE: FAILURES\n");
    return allPass ? 0 : 1;
}

std::string squareStr(int x, int y) {
    std::string s;
    s += static_cast<char>('a' + x);
    s += static_cast<char>('1' + y);
    return s;
}

// Walks the same tree as perftUnmake(), but at every node (not just leaves)
// asserts the incrementally-maintained eval/hash match a from-scratch
// recompute. This is the equivalence check for Step 2: it must never fire
// across thousands of positions spanning captures, castling, promotions,
// and en passant, since the incremental fields are the ones the real search
// evaluates at every node.
long long verifyEvalNode(Board& board, int depth, Color color, long long& mismatches) {
    int incremental = board.rawEval();
    int fromScratch = board.rawEvalFromScratch();
    if (incremental != fromScratch) {
        mismatches++;
        std::fprintf(stderr, "EVAL MISMATCH at depth=%d: incremental=%d fromScratch=%d\n", depth, incremental,
                     fromScratch);
    }
    if (depth == 0) return 1;
    long long nodes = 0;
    for (const Move& m : board.legalMoves(color)) {
        Board::UndoState undo;
        board.makeMove(m, undo);
        nodes += verifyEvalNode(board, depth - 1, opponent(color), mismatches);
        board.unmakeMove(m, undo);
    }
    return nodes;
}

int runVerifyEval(int maxDepth) {
    bool allPass = true;
    for (const PerftCase& tc : perftSuite()) {
        Board board = Board::fromFEN(tc.fen);
        long long mismatches = 0;
        long long nodes = verifyEvalNode(board, maxDepth, board.sideToMove(), mismatches);
        bool pass = mismatches == 0;
        allPass &= pass;
        std::printf("%-10s depth=%d nodesChecked=%-12lld mismatches=%lld %s\n", tc.name, maxDepth, nodes, mismatches,
                    pass ? "OK" : "FAIL");
    }
    std::printf(allPass ? "EVAL VERIFICATION: ALL PASS\n" : "EVAL VERIFICATION: FAILURES\n");
    return allPass ? 0 : 1;
}

struct BenchPosition {
    const char* name;
    const char* fen; // nullptr means the default starting position
};

const std::vector<BenchPosition>& benchPositions() {
    static const std::vector<BenchPosition> positions = {
        {"startpos", nullptr},
        {"middlegame(kiwipete)", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"},
        {"endgame", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1"},
        {"tactical(mate-in-1)", "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1"},
    };
    return positions;
}

int runBench(int depth) {
    for (const BenchPosition& bp : benchPositions()) {
        Board board = bp.fen ? Board::fromFEN(bp.fen) : Board();
        auto start = std::chrono::steady_clock::now();
        BenchResult r = benchSearch(board, board.sideToMove(), depth);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        double nps = secs > 0 ? r.nodes / secs : 0;
        std::string mv = squareStr(r.move.fromX, r.move.fromY) + squareStr(r.move.toX, r.move.toY);
        std::printf("%-22s depth=%d nodes=%-10lld time=%.3fs nps=%.0f score=%d move=%s\n", bp.name, depth, r.nodes,
                     secs, nps, r.score, mv.c_str());
    }
    return 0;
}

// Replays a PGN's mainline move-by-move. At every White-to-move position
// (White is this engine's suggestions, relayed by hand in the source games)
// it calls findBestMove with debug logging enabled and reports whether the
// engine's choice matches what was actually played, then applies the
// *actual historical* move so later positions stay in sync with the real
// game regardless of what the engine suggests.
int runReplay(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "replay: could not open %s\n", path.c_str());
        return 1;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::vector<std::string> sanMoves = parsePgnMainline(buffer.str());

    Board board;
    TranspositionTable tt(kTTSize); // persists across the whole replayed game, not rebuilt per move
    int ply = 0;
    for (const std::string& san : sanMoves) {
        int moveNumber = ply / 2 + 1;
        Color toMove = board.sideToMove();
        const char* sideLabel = toMove == Color::White ? "White" : "Black";

        Move actual;
        if (!resolveSanMove(board, san, actual)) {
            std::fprintf(stderr, "replay: failed to resolve SAN '%s' at move %d (%s)\n", san.c_str(), moveNumber,
                         sideLabel);
            return 1;
        }

        if (toMove == Color::White) {
            g_aiDebugLog = true;
            std::fprintf(stderr, "=== move %d, White to play (actual: %s) ===\n", moveNumber, san.c_str());
            Move suggested = findBestMove(board, Color::White, 20, 6000, tt);
            g_aiDebugLog = false;
            bool match = suggested.fromX == actual.fromX && suggested.fromY == actual.fromY &&
                         suggested.toX == actual.toX && suggested.toY == actual.toY &&
                         suggested.promotion == actual.promotion;
            std::string suggestedStr =
                squareStr(suggested.fromX, suggested.fromY) + squareStr(suggested.toX, suggested.toY);
            std::fprintf(stderr, "=== move %d: engine suggests %s, actual played %s -> %s\n", moveNumber,
                         suggestedStr.c_str(), san.c_str(), match ? "MATCH" : "MISMATCH");
        }

        board.makeMove(actual);
        ply++;
    }

    std::printf("replay: finished %s, %d plies\n", path.c_str(), ply);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--perft") == 0) {
        int depth = argc >= 3 ? std::atoi(argv[2]) : 4;
        return runPerft(depth);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--perft-suite") == 0) {
        int depth = argc >= 3 ? std::atoi(argv[2]) : 4;
        return runPerftSuite(depth);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--verify-eval") == 0) {
        int depth = argc >= 3 ? std::atoi(argv[2]) : 4;
        return runVerifyEval(depth);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--bench") == 0) {
        int depth = argc >= 3 ? std::atoi(argv[2]) : 6;
        return runBench(depth);
    }
    if (argc >= 3 && std::strcmp(argv[1], "--replay") == 0) {
        return runReplay(argv[2]);
    }
    return runGui();
}
