#include "datagen.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <unordered_set>
#include <string>
#include <vector>

#include "ai.hpp"
#include "bitboard.hpp"
#include "match.hpp"
#include "board.hpp"
#include "nnue.hpp"
#include "nnue_record.hpp"
#include "opening_book.hpp"
#include "syzygy.hpp"
#include "transposition_table.hpp"

// Self-play for training data: games between two copies of the engine, each move searched to a fixed number of
// nodes, started from random openings. Every position where the search score is meaningful is written as a
// record (nnue_record.hpp) once the game's result is known. One process plays on one thread (findBestMove keeps
// per-process state); tools/nnue/datagen.sh runs many.
namespace {

struct Config {
    std::string out;
    std::string syzygy; // tablebase directories: label positions of up to their size exactly (not used by the search)
    std::string net; // a network file for both sides ("" = whatever the engine starts with)
    long long games = 1000;
    int nodes = 5000;
    int randomPlies = 8;
    int bookPlies = 0; // plies taken from the opening book first (weighted at random), before the random ones
    uint64_t seed = 1;
    double endgameShare = 0;  // share of games that start from a random endgame (setupEndgame) and are played out
    int maxScore = 2500;      // positions whose search score is larger are not recorded
    int adjudicateCp = 1500;  // a score this lopsided for 6 plies in a row ends the game as won
    double randomMoveProb = 0; // after the random opening plies, each move of the next randomMovePlies plies is random
    int randomMovePlies = 0;   // with this probability (as Stockfish's gensfen does) and is not recorded
    int noAdjudicatePieces = 0; // ...but not in positions with this many pieces or fewer: play the ending out
};

struct Stats {
    long long games = 0, positions = 0, whiteWins = 0, blackWins = 0, draws = 0, abandoned = 0, endgames = 0;
};

// A random legal endgame from a weighted list of the material that matters in play: the basic mates, the
// minor-piece and pawn endings, and the rook and queen endings where a side's conversion is the hard part. Pieces
// stand on random squares, pawns on ranks 2-7, either side to move; the setup is retried until it is legal.
bool setupEndgame(std::mt19937_64& rng, Board& board) {
    struct Material { const char* white; const char* black; int weight; };
    static const Material kList[] = {
        {"R", "", 12},   {"Q", "", 4},    {"BN", "", 6},   {"BB", "", 3},   {"RR", "", 2},    {"QR", "", 1},
        {"P", "", 4},    {"RP", "", 6},   {"QP", "", 2},   {"BP", "", 2},   {"NP", "", 2},    {"PP", "P", 2},
        {"R", "P", 5},   {"R", "B", 3},   {"R", "N", 3},   {"Q", "R", 3},   {"Q", "P", 3},    {"Q", "B", 1},
        {"RP", "R", 6},  {"RPP", "R", 6}, {"RPPP", "RP", 3}, {"RRP", "RR", 1}, {"QP", "Q", 3}, {"B", "N", 1},
        {"BP", "B", 2},  {"BP", "N", 2},  {"NP", "N", 2},  {"BPP", "NP", 2}, {"RB", "R", 3}, {"RN", "R", 3},
        {"RB", "RB", 2}, {"RN", "RN", 1}, {"QB", "Q", 1},  {"QN", "Q", 1},  {"PPP", "PP", 2}, {"PPPP", "PPP", 1},
    };
    int total = 0;
    for (const Material& m : kList) total += m.weight;
    for (int attempt = 0; attempt < 200; attempt++) {
        int pick = static_cast<int>(rng() % static_cast<uint64_t>(total));
        const Material* m = &kList[0];
        for (const Material& e : kList) if ((pick -= e.weight) < 0) { m = &e; break; }
        char grid[8][8] = {};
        auto place = [&](char c, bool pawn) {
            for (int tries = 0; tries < 100; tries++) {
                const int sq = static_cast<int>(rng() % 64), rank = sq / 8, file = sq % 8;
                if (grid[rank][file] || (pawn && (rank == 0 || rank == 7))) continue;
                grid[rank][file] = c;
                return;
            }
        };
        place('K', false);
        place('k', false);
        for (const char* c = m->white; *c; c++) place(*c, *c == 'P');
        for (const char* c = m->black; *c; c++) place(static_cast<char>(*c + 32), *c == 'P');
        std::string fen;
        for (int rank = 7; rank >= 0; rank--) {
            int empty = 0;
            for (int file = 0; file < 8; file++) {
                if (!grid[rank][file]) { empty++; continue; }
                if (empty) fen += static_cast<char>('0' + empty);
                empty = 0;
                fen += grid[rank][file];
            }
            if (empty) fen += static_cast<char>('0' + empty);
            if (rank) fen += '/';
        }
        fen += (rng() & 1) ? " w - - 0 1" : " b - - 0 1";
        Board candidate;
        if (!Board::tryFromFEN(fen, candidate)) continue;
        if (candidate.legalMoves(candidate.sideToMove()).empty()) continue;
        board = candidate;
        return true;
    }
    return false;
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
bool playGame(const Config& cfg, std::mt19937_64& rng, std::vector<nnue::data::Record>& records, int& result,
              bool endgame) {
    Board board;
    if (endgame && !setupEndgame(rng, board)) return false;
    std::vector<uint64_t> history{board.hashKey()};
    for (int i = 0; !endgame && i < cfg.bookPlies; i++) {
        const std::vector<BookEntry> book = lookupBook(board);
        if (book.empty()) break;
        int total = 0;
        for (const BookEntry& e : book) total += e.weight;
        int pick = static_cast<int>(rng() % static_cast<uint64_t>(total));
        const BookEntry* chosen = &book.back();
        for (const BookEntry& e : book) if ((pick -= e.weight) < 0) { chosen = &e; break; }
        board.makeMove(chosen->move);
        history.push_back(board.hashKey());
    }
    for (int i = 0; !endgame && i < cfg.randomPlies; i++) {
        MoveList moves = board.legalMoves(board.sideToMove());
        if (moves.empty()) return false;
        board.makeMove(moves[rng() % moves.size()]);
        history.push_back(board.hashKey());
    }
    if (board.legalMoves(board.sideToMove()).empty()) return false;

    TranspositionTable tt(size_t(1) << 20); // 16 MB
    // An opening one side has already lost teaches little: look at it briefly and start again if it is lopsided.
    if (!endgame) {
        LastIteration probe;
        Config quick = cfg;
        quick.nodes = std::max(cfg.nodes, 20000);
        search(board, quick, tt, history, probe);
        if (std::abs(probe.score) > 300) return false;
    }

    std::vector<nnue::data::Record> game;
    int decisive = 0, quiet = 0; // consecutive plies of a lopsided / of a dead-level score
    int adjudicated = 0;         // +1 White wins, -1 Black wins by the lopsided score
    int tbWhite = 2;             // the tablebase verdict (White's point of view) of the first table position: 2 = none yet
    const int tbSize = syzygy::tableSize();
    for (int ply = static_cast<int>(history.size()) - 1; ply < 600; ply++) {
        const Color stm = board.sideToMove();
        MoveList legal = board.legalMoves(stm);
        if (legal.empty()) {
            result = board.isInCheck(stm) ? (stm == Color::White ? -1 : 1) : 0;
            break;
        }
        if (board.halfmoveClock() >= 100 || isInsufficientMaterial(board) ||
            std::count(history.begin(), history.end(), board.hashKey()) >= 3) {
            result = 0;
            break;
        }
        if (!endgame && ply < cfg.randomPlies + cfg.randomMovePlies && (rng() >> 11) * 0x1.0p-53 < cfg.randomMoveProb) {
            board.makeMove(legal[rng() % legal.size()]);
            history.push_back(board.hashKey());
            continue;
        }
        int tb = 2; // the tablebase result for the side to move: the exact label of this position
        if (tbSize > 0 && bb::popcount(board.occupancy()) <= tbSize) {
            syzygy::Wdl wdl;
            if (syzygy::probeLabel(board, wdl)) {
                tb = wdl == syzygy::Wdl::Win ? 1 : wdl == syzygy::Wdl::Loss ? -1 : 0;
                if (tbWhite == 2) tbWhite = stm == Color::White ? tb : -tb;
            }
        }
        LastIteration last;
        const Move best = search(board, cfg, tt, history, last);
        const int whiteScore = stm == Color::White ? last.score : -last.score;
        if (std::abs(last.score) < 20000 && last.depth > 0) {
            const bool noisy = best.captured != PieceType::None || best.isEnPassant || best.promotion != PieceType::None;
            if (!noisy && !board.isInCheck(stm) && std::abs(last.score) <= cfg.maxScore) {
                game.push_back(nnue::data::makeRecord(board, last.score, tb == 2 ? 0 : tb, last.depth, ply));
                game.back().reserved = tb != 2; // (the result is the table's, whatever the game's)
            }
        }
        if (last.depth > 0) { // (a forced move is not searched, so says nothing)
            if (std::abs(last.score) >= kMateThreshold) { // the search sees a forced mate
                result = whiteScore > 0 ? 1 : -1;
                break;
            }
            const bool play = endgame || bb::popcount(board.occupancy()) <= cfg.noAdjudicatePieces;
            if (!play && std::abs(whiteScore) >= cfg.adjudicateCp) {
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
    // The first table position settles the game as Stockfish's data does: the play before it is labelled with the
    // theoretical result even if this engine then fails to convert (or to hold) it.
    if (tbWhite != 2) result = tbWhite;
    for (nnue::data::Record& r : game) {
        if (!r.reserved) r.result = static_cast<int8_t>(r.stm == 0 ? result : -result);
        r.reserved = 0;
    }
    records.insert(records.end(), game.begin(), game.end());
    return true;
}

// Post-processing of existing data files: drop duplicate positions (same pieces and side to move; the first copy
// stays) and, with tablebases, replace the result of every position they cover by the exact one.
int processFiles(const Config& cfg, const std::vector<std::string>& inputs) {
    std::FILE* out = std::fopen(cfg.out.c_str(), "wb");
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", cfg.out.c_str());
        return 1;
    }
    std::unordered_set<uint64_t> seen;
    long long read = 0, kept = 0, relabelled = 0, changed = 0;
    std::vector<nnue::data::Record> chunk(1 << 16);
    for (const std::string& name : inputs) {
        std::FILE* in = std::fopen(name.c_str(), "rb");
        if (!in) {
            std::fprintf(stderr, "cannot read %s\n", name.c_str());
            return 1;
        }
        size_t n;
        while ((n = std::fread(chunk.data(), sizeof(nnue::data::Record), chunk.size(), in)) > 0) {
            size_t w = 0;
            for (size_t i = 0; i < n; i++) {
                nnue::data::Record r = chunk[i];
                read++;
                uint64_t h = r.occupancy * 0x9E3779B97F4A7C15ULL ^ (r.stm + 1ULL);
                for (int k = 0; k < 16; k++) h = (h ^ r.pieces[k]) * 0x100000001B3ULL;
                h ^= h >> 29;
                if (!seen.insert(h).second) continue;
                if (syzygy::tableSize() > 0 && nnue::data::pieceCount(r) <= syzygy::tableSize()) {
                    Board board;
                    if (Board::tryFromFEN(nnue::data::toFen(r), board)) {
                        syzygy::Wdl wdl;
                        if (syzygy::probeLabel(board, wdl)) {
                            const int result = wdl == syzygy::Wdl::Win ? 1 : wdl == syzygy::Wdl::Loss ? -1 : 0;
                            relabelled++;
                            changed += r.result != result;
                            r.result = static_cast<int8_t>(result);
                        }
                    }
                }
                chunk[w++] = r;
            }
            kept += static_cast<long long>(w);
            std::fwrite(chunk.data(), sizeof(nnue::data::Record), w, out);
        }
        std::fclose(in);
        std::fprintf(stderr, "%s done: read %lld, kept %lld, table-labelled %lld (%lld changed)\n", name.c_str(), read, kept,
                     relabelled, changed);
    }
    std::fclose(out);
    return 0;
}

} // namespace

int runDatagen(int argc, char** argv) {
    Config cfg;
    std::vector<std::string> inputs; // data files to deduplicate / relabel instead of playing games
    for (int a = 2; a < argc; a++) {
        const std::string arg = argv[a];
        auto value = [&]() -> const char* { return a + 1 < argc ? argv[++a] : "0"; };
        if (arg == "--out") cfg.out = value();
        else if (arg == "--syzygy") cfg.syzygy = value();
        else if (arg == "--net") cfg.net = value();
        else if (arg == "--games") cfg.games = std::atoll(value());
        else if (arg == "--nodes") cfg.nodes = std::atoi(value());
        else if (arg == "--random-plies") cfg.randomPlies = std::atoi(value());
        else if (arg == "--book-plies") cfg.bookPlies = std::atoi(value());
        else if (arg == "--endgame-share") cfg.endgameShare = std::atof(value());
        else if (arg == "--random-move-prob") cfg.randomMoveProb = std::atof(value());
        else if (arg == "--random-move-plies") cfg.randomMovePlies = std::atoi(value());
        else if (arg == "--max-score") cfg.maxScore = std::atoi(value());
        else if (arg == "--adjudicate-cp") cfg.adjudicateCp = std::atoi(value());
        else if (arg == "--no-adjudicate-pieces") cfg.noAdjudicatePieces = std::atoi(value());
        else if (arg == "--seed") cfg.seed = static_cast<uint64_t>(std::atoll(value()));
        else if (arg.rfind("--", 0) != 0) inputs.push_back(arg);
        else {
            std::fprintf(stderr, "unknown datagen option %s\n", arg.c_str());
            return 2;
        }
    }
    if (cfg.out.empty()) {
        std::fprintf(stderr, "usage: chess --datagen --out <file> [<data files to deduplicate and relabel>] [--games N] [--nodes N] [--random-plies N] [--book-plies N] [--endgame-share F] [--syzygy <dirs>] [--random-move-prob P] [--random-move-plies N] [--max-score N] [--adjudicate-cp N] [--no-adjudicate-pieces N] [--seed N] [--net <file>]\n");
        return 2;
    }
    if (!cfg.net.empty()) {
        std::string error;
        if (!nnue::loadFile(cfg.net, error)) {
            std::fprintf(stderr, "cannot load %s: %s\n", cfg.net.c_str(), error.c_str());
            return 1;
        }
    }
    if (!cfg.syzygy.empty()) {
        std::string message;
        if (!syzygy::init(cfg.syzygy, message) || syzygy::tableSize() == 0) {
            std::fprintf(stderr, "tablebases: %s\n", message.c_str());
            return 1;
        }
        syzygy::setSearchUse(false);
        std::fprintf(stderr, "%s\n", message.c_str());
    }
    if (!inputs.empty()) return processFiles(cfg, inputs);
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
        const bool endgame = cfg.endgameShare > 0 && (rng() >> 11) * 0x1.0p-53 < cfg.endgameShare;
        if (!playGame(cfg, rng, records, result, endgame)) {
            st.abandoned++;
            continue;
        }
        st.games++;
        st.endgames += endgame;
        st.positions += static_cast<long long>(records.size());
        (result > 0 ? st.whiteWins : result < 0 ? st.blackWins : st.draws)++;
        std::fwrite(records.data(), sizeof(nnue::data::Record), records.size(), out);
        if (st.games % 100 == 0) {
            std::fflush(out);
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::fprintf(stderr, "games %lld, positions %lld (%.0f/s), W %lld D %lld L %lld, unusable openings %lld, endgames %lld\n", st.games,
                         st.positions, st.positions / secs, st.whiteWins, st.draws, st.blackWins, st.abandoned, st.endgames);
        }
    }
    std::fclose(out);
    std::printf("games %lld, positions %lld, W %lld D %lld L %lld\n", st.games, st.positions, st.whiteWins, st.draws, st.blackWins);
    return 0;
}
