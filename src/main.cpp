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
#include "datagen.hpp"
#include "gui.hpp"
#include "match.hpp"
#include "nnue.hpp"
#include "opening_book.hpp"
#include "pgn.hpp"
#include "uci.hpp"
#include "version.hpp"

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
         {48, 2039, 97862, 4085603, 193690690}},
        {"endgame", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", {14, 191, 2812, 43238, 674624}},
        // Chess Programming Wiki positions 4 and 5 (promotions, castling through check, en passant), position 4
        // mirrored, position 6, and two more edge cases: all four castlings, and promotions with captures.
        {"cpw4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
         {6, 264, 9467, 422333, 15833292}},
        {"cpw4mirror", "r2q1rk1/pP1p2pp/Q4n2/bbp1p3/Np6/1B3NBn/pPPP1PPP/R3K2R b KQ - 0 1",
         {6, 264, 9467, 422333, 15833292}},
        {"cpw5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", {44, 1486, 62379, 2103487, 89941194}},
        {"cpw6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
         {46, 2079, 89890, 3894594, 164075551}},
        {"castling", "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", {26, 568, 13744, 314346, 7594526}},
        {"promotions", "n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1", {24, 496, 9483, 182838, 3605103}},
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
// asserts the incrementally-maintained eval, hash and pawn hash match a
// from-scratch recompute. This is the equivalence check for Step 2: it must never fire
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
    if (board.hashKey() != board.hashKeyFromScratch() || board.pawnKey() != board.pawnKeyFromScratch()) {
        mismatches++;
        std::fprintf(stderr, "HASH MISMATCH at depth=%d\n", depth);
    }
    if (!board.bitboardsConsistent()) {
        mismatches++;
        std::fprintf(stderr, "BITBOARD MISMATCH at depth=%d\n", depth);
    }
    {
        // toFEN must read back (tryFromFEN may drop an en passant square no pawn can use, so the
        // comparison stops after the castling field).
        const std::string fen = board.toFEN();
        Board again;
        const auto head = [](const std::string& f) { return f.substr(0, f.find(' ', f.find(' ', f.find(' ') + 1) + 1)); };
        if (!Board::tryFromFEN(fen, again) || head(again.toFEN()) != head(fen)) {
            mismatches++;
            std::fprintf(stderr, "FEN ROUND-TRIP MISMATCH at depth=%d: %s\n", depth, fen.c_str());
        }
    }
    if (!board.nnueMatchesScratch()) {
        mismatches++;
        std::fprintf(stderr, "NETWORK ACCUMULATOR MISMATCH at depth=%d\n", depth);
    }
    if (depth == 0) return 1;
    long long nodes = 0;
    for (const Move& m : board.legalMoves(color)) {
        const bool predictedCheck = board.givesCheck(m);
        const uint64_t predictedHash = board.hashAfter(m);
        Board::UndoState undo;
        board.makeMove(m, undo);
        if (predictedHash != board.hashKey()) {
            mismatches++;
            std::fprintf(stderr, "HASH-AFTER MISMATCH at depth=%d: %s%s\n", depth, squareStr(m.fromX, m.fromY).c_str(),
                         squareStr(m.toX, m.toY).c_str());
        }
        if (predictedCheck != board.isInCheck(opponent(color))) {
            mismatches++;
            std::fprintf(stderr, "GIVES-CHECK MISMATCH at depth=%d: %s%s predicted %d\n", depth,
                         squareStr(m.fromX, m.fromY).c_str(), squareStr(m.toX, m.toY).c_str(), predictedCheck);
        }
        nodes += verifyEvalNode(board, depth - 1, opponent(color), mismatches);
        board.unmakeMove(m, undo);
    }
    return nodes;
}

// Both sides' views of a piece must be each other's mirror images, or the network could not be colour-symmetric.
bool networkFeaturesSymmetric() {
    for (int color = 0; color < 2; color++)
        for (int type = 1; type <= 6; type++)
            for (int x = 0; x < 8; x++)
                for (int y = 0; y < 8; y++)
                    if (nnue::featureIndex(0, color, type, x, y) != nnue::featureIndex(1, 1 - color, type, x, 7 - y))
                        return false;
    return true;
}

int runVerifyEval(int maxDepth) {
    bool allPass = true;
    // Once with the hand-crafted evaluation (the incremental terms against a recompute), once with a network of
    // random weights (the incrementally updated accumulators against a recompute).
    const bool hadNetwork = nnue::active();
    for (int round = 0; round < 2; round++) {
        if (round == 1) nnue::loadRandom(12345);
        for (const PerftCase& tc : perftSuite()) {
            Board board = Board::fromFEN(tc.fen);
            long long mismatches = 0;
            long long nodes = verifyEvalNode(board, maxDepth, board.sideToMove(), mismatches);
            const bool pass = mismatches == 0;
            allPass &= pass;
            std::printf("%-10s depth=%d nodesChecked=%-12lld mismatches=%lld %s%s\n", tc.name, maxDepth, nodes,
                        mismatches, pass ? "OK" : "FAIL", round == 1 ? "  (random network)" : "");
        }
    }
    if (!hadNetwork) nnue::unload();
    const bool symmetric = networkFeaturesSymmetric();
    allPass &= symmetric;
    std::printf("network features colour-symmetric: %s\n", symmetric ? "OK" : "FAIL");
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
    long long totalNodes = 0;
    double totalSecs = 0;
    for (const BenchPosition& bp : benchPositions()) {
        Board board = bp.fen ? Board::fromFEN(bp.fen) : Board();
        BenchResult r = benchSearch(board, board.sideToMove(), depth);
        double secs = r.seconds;
        double nps = secs > 0 ? r.nodes / secs : 0;
        std::string mv = squareStr(r.move.fromX, r.move.fromY) + squareStr(r.move.toX, r.move.toY);
        std::printf("%-22s depth=%d nodes=%-10lld time=%.3fs nps=%.0f score=%d move=%s\n", bp.name, depth, r.nodes,
                     secs, nps, r.score, mv.c_str());
        totalNodes += r.nodes;
        totalSecs += secs;
    }
    // Speed over all positions together, search time only (the 64 MiB table's allocation is outside the
    // clock). Depth 6 is far too short to time (a tenth of a second): use `--bench 12` or more.
    std::printf("BENCH SPEED: nodes=%lld time=%.3fs nps=%.0f\n", totalNodes, totalSecs,
                totalSecs > 0 ? totalNodes / totalSecs : 0.0);
    // Stable, single-line summary for scripted regression checks: a pure
    // speedup commit must reproduce this line byte-for-byte across builds
    // (same tree, same node count); anything else means search behavior
    // changed, not just speed.
    std::printf("BENCH SIGNATURE: nodes=%lld depth=%d positions=%zu\n", totalNodes, depth, benchPositions().size());
    return 0;
}

// Forces the opening book's lazily-built index to construct (it logs a
// warning to stderr for any line with an illegal move) and reports what the
// starting position's book looks like, as a lightweight sanity check when
// editing opening_book.cpp.
int runVerifyBook() {
    Board board;
    std::vector<BookEntry> replies = lookupBook(board);
    std::printf("Generated book: %zu position(s) (0: none compiled in).\n", generatedBookPositions());
    std::printf("Book has %zu known first move(s) for White.\n", replies.size());
    for (const BookEntry& be : replies) {
        std::string mv = squareStr(be.move.fromX, be.move.fromY) + squareStr(be.move.toX, be.move.toY);
        std::printf("  %s (weight=%d)\n", mv.c_str(), be.weight);
    }
    std::printf("BOOK VERIFICATION: see warnings above (none printed = every line is fully legal)\n");
    return 0;
}

// Replays a PGN's mainline move-by-move. At every position matching `side`
// ("white", "black", or "both") it calls findBestMove with debug logging
// enabled and reports whether the engine's choice, score, and PV match what
// was actually played, then applies the *actual historical* move so later
// positions stay in sync with the real game regardless of what the engine
// suggests.
int runReplay(const std::string& path, const std::string& side) {
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
    int analyzed = 0;
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

        bool shouldAnalyze = (toMove == Color::White) ? (side != "black") : (side != "white");
        if (shouldAnalyze) {
            analyzed++;
            g_aiDebugLog = true;
            std::fprintf(stderr, "=== move %d, %s to play (actual: %s) ===\n", moveNumber, sideLabel, san.c_str());
            SearchProgress lastProgress{};
            bool searched = false; // onDepthComplete only fires if findBestMove actually searched
                                    // (an opening-book hit returns instantly with no search at all)
            SearchOptions opts;
            opts.onDepthComplete = [&lastProgress, &searched](const SearchProgress& p) {
                lastProgress = p;
                searched = true;
            };
            Move suggested = findBestMove(board, toMove, 20, 6000, tt, opts);
            g_aiDebugLog = false;
            bool match = suggested.fromX == actual.fromX && suggested.fromY == actual.fromY &&
                         suggested.toX == actual.toX && suggested.toY == actual.toY &&
                         suggested.promotion == actual.promotion;
            std::string scoreStr = searched ? formatUciScore(lastProgress.score) : "book";
            std::string pvStr = "-";
            if (searched) {
                // `suggested` is the authoritative move for the search that
                // just ran; extractPV()'s own TT root-probe for `board` can
                // disagree with it for the same reason documented in
                // uci.cpp's onDepthComplete (persistent-across-the-game TT,
                // depth-gated replacement) -- so ply 0 always comes from
                // `suggested`, extractPV only supplies the continuation.
                Board afterSuggested = board;
                afterSuggested.makeMove(suggested);
                std::vector<Move> pv = {suggested};
                std::vector<Move> rest = extractPV(afterSuggested, tt, 11);
                pv.insert(pv.end(), rest.begin(), rest.end());
                pvStr = formatUciMoveList(pv);
            }
            std::fprintf(stderr, "=== move %d: engine suggests %s (score %s, pv %s), actual played %s -> %s\n",
                         moveNumber, formatUciMove(suggested).c_str(), scoreStr.c_str(), pvStr.c_str(), san.c_str(),
                         match ? "MATCH" : "MISMATCH");
        }

        board.makeMove(actual);
        ply++;
    }

    std::printf("replay: finished %s, %d plies, %d analyzed (side=%s)\n", path.c_str(), ply, analyzed, side.c_str());
    return 0;
}

void printUsage(std::FILE* out, const char* program) {
    std::fprintf(out,
                 "ChessGame %s\n"
                 "usage: %s [mode]\n"
                 "  (no mode)                       open the GUI\n"
                 "  --uci                           UCI engine on stdin/stdout\n"
                 "  --bench [depth]                 fixed-depth search benchmark (default 6)\n"
                 "  --perft [depth]                 node count from the start position\n"
                 "  --perft-suite [depth]           perft against published counts\n"
                 "  --verify-eval [depth]           incremental eval/hash/bitboards vs from-scratch\n"
                 "  --verify-book                   list the opening book's first moves\n"
                 "  --build-book <lines> <out> [min]  make the generated book (tools/book/extract_lines.py)\n"
                 "  --replay <pgn> [white|black|both]   re-search the positions of a game\n"
                 "  --selfplay <games> [levelA] [levelB]   bot matches, levels easy|medium|hard|expert\n"
                 "  --datagen --out <file> [--games N] [--nodes N] [--seed N] [--net <file>]   training data\n"
                 "  --version, --help\n",
                 CHESS_VERSION, program);
}

} // namespace

#ifdef CHESS_NO_GUI
int runGui() {
    std::fprintf(stderr, "this build has no GUI (CHESS_GUI=OFF); run it with --uci\n");
    return 1;
}
#endif

int main(int argc, char** argv) {
    nnue::loadBuiltin(); // the network compiled into this binary, if there is one
    if (argc >= 2 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0)) {
        std::printf("ChessGame %s\nnetwork kernel: %s\n", CHESS_VERSION, nnue::kernelName());
        return 0;
    }
    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        printUsage(stdout, argv[0]);
        return 0;
    }
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
    if (argc >= 2 && std::strcmp(argv[1], "--verify-book") == 0) {
        return runVerifyBook();
    }
    if (argc >= 4 && std::strcmp(argv[1], "--build-book") == 0) {
        const int minCount = argc >= 5 ? std::atoi(argv[4]) : 20;
        std::string message;
        const bool ok = buildBookFile(argv[2], argv[3], minCount, message);
        std::fprintf(ok ? stdout : stderr, "build-book: %s\n", message.c_str());
        return ok ? 0 : 1;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--uci") == 0) {
        return runUci();
    }
    if (argc >= 2 && std::strcmp(argv[1], "--datagen") == 0) {
        return runDatagen(argc, argv);
    }
    if (argc >= 3 && std::strcmp(argv[1], "--replay") == 0) {
        std::string side = argc >= 4 ? argv[3] : "both";
        if (side != "white" && side != "black" && side != "both") {
            std::fprintf(stderr, "replay: unknown side '%s' (expected white|black|both)\n", side.c_str());
            return 1;
        }
        return runReplay(argv[2], side);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--selfplay") == 0) {
        int games = argc >= 3 ? std::atoi(argv[2]) : 0;
        int levelA = presetIndexByName(argc >= 4 ? argv[3] : "medium");
        int levelB = presetIndexByName(argc >= 5 ? argv[4] : "medium");
        if (games < 1 || levelA < 0 || levelB < 0) {
            std::fprintf(stderr,
                         "usage: %s --selfplay <games> [botA level] [botB level]\n"
                         "  levels: easy|medium|hard|expert (default medium); Bot A is White in game 1\n",
                         argv[0]);
            return 1;
        }
        return runSelfplay(games, levelA, levelB);
    }
    if (argc >= 2) { // an unknown option must not silently open a window
        std::fprintf(stderr, "unknown option '%s'\n", argv[1]);
        printUsage(stderr, argv[0]);
        return 2;
    }
    return runGui();
}
