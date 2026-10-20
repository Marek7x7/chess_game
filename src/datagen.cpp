#include "datagen.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "ai.hpp"
#include "board.hpp"
#include "nnue.hpp"
#include "nnue_record.hpp"
#include "transposition_table.hpp"

// Self-play for training data: games between two copies of the engine, each move searched to a fixed number of
// nodes, started from random openings. Every position where the search score is meaningful is written as a
// record (nnue_record.hpp) once the game's result is known. One process plays on one thread (findBestMove keeps
// per-process state); tools/nnue/datagen.sh runs many.
namespace {

struct Config {
    std::string out;
    std::string net; // a network file for both sides ("" = whatever the engine starts with)
    long long games = 1000;
    int nodes = 5000;
    int randomPlies = 8;
    uint64_t seed = 1;
};

struct Stats {
    long long games = 0, positions = 0, whiteWins = 0, blackWins = 0, draws = 0, abandoned = 0;
};

bool insufficientMaterial(const Board& b) {
    const uint64_t heavy = b.pieces(Color::White, PieceType::Pawn) | b.pieces(Color::Black, PieceType::Pawn) |
                           b.pieces(Color::White, PieceType::Rook) | b.pieces(Color::Black, PieceType::Rook) |
                           b.pieces(Color::White, PieceType::Queen) | b.pieces(Color::Black, PieceType::Queen);
    if (heavy) return false;
    const int minors = __builtin_popcountll(b.pieces(Color::White, PieceType::Knight) | b.pieces(Color::White, PieceType::Bishop) |
                                            b.pieces(Color::Black, PieceType::Knight) | b.pieces(Color::Black, PieceType::Bishop));
    return minors <= 1;
}

// The score of the deepest completed iteration (the side that was to move's point of view) and its depth.
struct LastIteration {
    int score = 0, depth = 0;
};

Move search(const Board& board, const Config& cfg, TranspositionTable& tt, const std::vector<uint64_t>& history,
            LastIteration& last) {
    SearchOptions opts;
    opts.useOpeningBook = false;
    opts.threadOverride = 1;
    opts.nodeLimit = cfg.nodes;
    opts.gameHistory = history;
    opts.endgameDepthBonus = false;
    opts.onDepthComplete = [&last](const SearchProgress& p) {
        last.score = p.score;
        last.depth = p.depth;
    };
    last = LastIteration{};
    return findBestMove(board, board.sideToMove(), 64, 60 * 1000, tt, opts);
}

// Plays one game. Returns false if the opening could not be set up. `result` is from White's point of view.
bool playGame(const Config& cfg, std::mt19937_64& rng, std::vector<nnue::data::Record>& records, int& result) {
    Board board;
    std::vector<uint64_t> history{board.hashKey()};
    for (int i = 0; i < cfg.randomPlies; i++) {
        MoveList moves = board.legalMoves(board.sideToMove());
        if (moves.empty()) return false;
        board.makeMove(moves[rng() % moves.size()]);
        history.push_back(board.hashKey());
    }
    if (board.legalMoves(board.sideToMove()).empty()) return false;

    TranspositionTable tt(size_t(1) << 20); // 16 MB
    // An opening one side has already lost teaches little: look at it briefly and start again if it is lopsided.
    {
        LastIteration probe;
        Config quick = cfg;
        quick.nodes = std::max(cfg.nodes, 20000);
        search(board, quick, tt, history, probe);
        if (std::abs(probe.score) > 300) return false;
    }

    std::vector<nnue::data::Record> game;
    int decisive = 0, quiet = 0; // consecutive plies of a lopsided / of a dead-level score
    int adjudicated = 0;         // +1 White wins, -1 Black wins by the lopsided score
    for (int ply = cfg.randomPlies; ply < 600; ply++) {
        const Color stm = board.sideToMove();
        MoveList legal = board.legalMoves(stm);
        if (legal.empty()) {
            result = board.isInCheck(stm) ? (stm == Color::White ? -1 : 1) : 0;
            break;
        }
        if (board.halfmoveClock() >= 100 || insufficientMaterial(board) ||
            std::count(history.begin(), history.end(), board.hashKey()) >= 3) {
            result = 0;
            break;
        }
        LastIteration last;
        const Move best = search(board, cfg, tt, history, last);
        const int whiteScore = stm == Color::White ? last.score : -last.score;
        if (std::abs(last.score) < 20000 && last.depth > 0) {
            const bool noisy = best.captured != PieceType::None || best.isEnPassant || best.promotion != PieceType::None;
            if (!noisy && !board.isInCheck(stm) && std::abs(last.score) <= 2500)
                game.push_back(nnue::data::makeRecord(board, last.score, 0, last.depth, ply));
        }
        if (last.depth > 0) { // (a forced move is not searched, so says nothing)
            if (std::abs(last.score) >= kMateThreshold) { // the search sees a forced mate
                result = whiteScore > 0 ? 1 : -1;
                break;
            }
            if (std::abs(whiteScore) >= 1500) {
                const int side = whiteScore > 0 ? 1 : -1;
                decisive = side == adjudicated ? decisive + 1 : 1;
                adjudicated = side;
            } else {
                decisive = 0;
                adjudicated = 0;
            }
            quiet = std::abs(whiteScore) <= 8 && ply >= 80 ? quiet + 1 : 0;
            if (decisive >= 6) { result = adjudicated; break; }
            if (quiet >= 14) { result = 0; break; }
        }
        board.makeMove(best);
        history.push_back(board.hashKey());
    }
    for (nnue::data::Record& r : game) r.result = static_cast<int8_t>(r.stm == 0 ? result : -result);
    records.insert(records.end(), game.begin(), game.end());
    return true;
}

} // namespace

int runDatagen(int argc, char** argv) {
    Config cfg;
    for (int a = 2; a < argc; a++) {
        const std::string arg = argv[a];
        auto value = [&]() -> const char* { return a + 1 < argc ? argv[++a] : "0"; };
        if (arg == "--out") cfg.out = value();
        else if (arg == "--net") cfg.net = value();
        else if (arg == "--games") cfg.games = std::atoll(value());
        else if (arg == "--nodes") cfg.nodes = std::atoi(value());
        else if (arg == "--random-plies") cfg.randomPlies = std::atoi(value());
        else if (arg == "--seed") cfg.seed = static_cast<uint64_t>(std::atoll(value()));
        else {
            std::fprintf(stderr, "unknown datagen option %s\n", arg.c_str());
            return 2;
        }
    }
    if (cfg.out.empty()) {
        std::fprintf(stderr, "usage: chess --datagen --out <file> [--games N] [--nodes N] [--random-plies N] [--seed N] [--net <file>]\n");
        return 2;
    }
    if (!cfg.net.empty()) {
        std::string error;
        if (!nnue::loadFile(cfg.net, error)) {
            std::fprintf(stderr, "cannot load %s: %s\n", cfg.net.c_str(), error.c_str());
            return 1;
        }
    }
    std::FILE* out = std::fopen(cfg.out.c_str(), "ab");
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", cfg.out.c_str());
        return 1;
    }
    std::mt19937_64 rng(cfg.seed * 0x9E3779B97F4A7C15ULL + 12345);
    Stats st;
    std::vector<nnue::data::Record> records;
    const auto start = std::chrono::steady_clock::now();
    while (st.games + st.abandoned < cfg.games) {
        records.clear();
        int result = 0;
        if (!playGame(cfg, rng, records, result)) {
            st.abandoned++;
            continue;
        }
        st.games++;
        st.positions += static_cast<long long>(records.size());
        (result > 0 ? st.whiteWins : result < 0 ? st.blackWins : st.draws)++;
        std::fwrite(records.data(), sizeof(nnue::data::Record), records.size(), out);
        if (st.games % 100 == 0) {
            std::fflush(out);
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::fprintf(stderr, "games %lld, positions %lld (%.0f/s), W %lld D %lld L %lld, unusable openings %lld\n", st.games,
                         st.positions, st.positions / secs, st.whiteWins, st.draws, st.blackWins, st.abandoned);
        }
    }
    std::fclose(out);
    std::printf("games %lld, positions %lld, W %lld D %lld L %lld\n", st.games, st.positions, st.whiteWins, st.draws, st.blackWins);
    return 0;
}
