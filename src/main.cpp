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

int runPerft(int maxDepth) {
    Board board;
    for (int depth = 1; depth <= maxDepth; depth++) {
        long long nodes = perft(board, depth, board.sideToMove());
        std::printf("perft(%d) = %lld\n", depth, nodes);
    }
    return 0;
}

std::string squareStr(int x, int y) {
    std::string s;
    s += static_cast<char>('a' + x);
    s += static_cast<char>('1' + y);
    return s;
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
            Move suggested = findBestMove(board, Color::White, 20, 6000);
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
    if (argc >= 3 && std::strcmp(argv[1], "--replay") == 0) {
        return runReplay(argv[2]);
    }
    return runGui();
}
