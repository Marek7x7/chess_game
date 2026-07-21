#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "board.hpp"
#include "gui.hpp"

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

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--perft") == 0) {
        int depth = argc >= 3 ? std::atoi(argv[2]) : 4;
        return runPerft(depth);
    }
    return runGui();
}
