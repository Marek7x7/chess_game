#include "match.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "ai.hpp"
#include "pgn.hpp"

namespace {

// The current local time, formatted with strftime.
std::string localTime(const char* format) {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), format, &tm);
    return buf;
}

// Only kings plus at most one knight or bishop in total: nobody can ever
// deliver mate. (Rarer dead positions, like same-colored bishops only, are
// left for the 50-move rule to end.)
bool insufficientMaterial(const Board& board) {
    const uint64_t heavy = board.pieces(Color::White, PieceType::Pawn) | board.pieces(Color::Black, PieceType::Pawn) |
                           board.pieces(Color::White, PieceType::Rook) | board.pieces(Color::Black, PieceType::Rook) |
                           board.pieces(Color::White, PieceType::Queen) | board.pieces(Color::Black, PieceType::Queen);
    if (heavy) return false;
    const uint64_t minors = board.pieces(Color::White, PieceType::Knight) | board.pieces(Color::White, PieceType::Bishop) |
                            board.pieces(Color::Black, PieceType::Knight) | board.pieces(Color::Black, PieceType::Bishop);
    return __builtin_popcountll(minors) <= 1;
}

} // namespace

bool isInsufficientMaterial(const Board& board) { return insufficientMaterial(board); }

int presetIndexByName(const std::string& name) {
    std::string upper;
    for (char c : name) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (int i = 0; i < 4; i++) {
        if (upper == kDifficultyPresets[i].label) return i;
    }
    return -1;
}

GameRecord::GameRecord() { positionHashes_.push_back(board_.hashKey()); }

void GameRecord::play(const Move& m) {
    std::string san = moveToSan(board_, m);
    // Cheap self-check of the SAN writer against the existing SAN reader: a
    // PGN whose moves don't read back is useless for a review.
    Move readBack;
    if (!resolveSanMove(board_, san, readBack) || !sameMove(readBack, m)) {
        std::fprintf(stderr, "warning: SAN '%s' at ply %zu does not read back as the move played\n", san.c_str(),
                     san_.size() + 1);
    }
    bool irreversible = board_.at(m.fromX, m.fromY).type == PieceType::Pawn || m.captured != PieceType::None;
    halfmoveClock_ = irreversible ? 0 : halfmoveClock_ + 1;
    board_.makeMove(m);
    san_.push_back(san);
    positionHashes_.push_back(board_.hashKey());
}

GameOutcome GameRecord::outcome() const {
    GameStatus status = board_.status();
    if (status == GameStatus::Checkmate) {
        return {true, board_.sideToMove() == Color::White ? "0-1" : "1-0", "checkmate"};
    }
    // hashKey() covers piece placement, side to move, castling rights and
    // the en passant file, so equal hashes are the same position for
    // repetition purposes.
    const char* drawReason = nullptr;
    if (status == GameStatus::Stalemate) {
        drawReason = "stalemate";
    } else if (std::count(positionHashes_.begin(), positionHashes_.end(), positionHashes_.back()) >= 3) {
        drawReason = "threefold repetition";
    } else if (halfmoveClock_ >= 100) {
        drawReason = "50-move rule";
    } else if (insufficientMaterial(board_)) {
        drawReason = "insufficient material";
    }
    if (drawReason) return {true, "1/2-1/2", drawReason};
    return {};
}

BotMatch::BotMatch(int levelA, int levelB, int games)
    : level{levelA, levelB}, totalGames(games), stamp(localTime("%Y%m%d_%H%M%S")) {}

std::string BotMatch::botName(int bot) const {
    return std::string(bot == 0 ? "Bot A" : "Bot B") + " (" + kDifficultyPresets[level[bot]].label + ")";
}

std::string BotMatch::recordFinishedGame(const GameRecord& game) {
    int round = ++gamesPlayed;
    int white = whiteBot(round);
    GameOutcome outcome = game.outcome();
    std::string termination;
    if (outcome.result == "1-0" || outcome.result == "0-1") {
        int winner = outcome.result == "1-0" ? white : 1 - white;
        wins[winner]++;
        termination = botName(winner) + " won by " + outcome.reason;
    } else {
        draws++;
        termination = "Game drawn by " + outcome.reason;
    }

    std::string pgn = formatPgn({{"Event", "Bot Match"},
                                 {"Site", "Local"},
                                 {"Date", localTime("%Y.%m.%d")},
                                 {"Round", std::to_string(round)},
                                 {"White", botName(white)},
                                 {"Black", botName(1 - white)},
                                 {"Result", outcome.result},
                                 {"Termination", termination}},
                                game.sanMoves(), outcome.result);

    std::filesystem::path path = std::filesystem::path("games") / ("botmatch_" + stamp + "_game" +
                                                                    (round < 10 ? "0" : "") + std::to_string(round) +
                                                                    ".pgn");
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    out << pgn;
    if (!out) {
        std::fprintf(stderr, "error: could not write %s\n", path.string().c_str());
        return "";
    }
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return (ec ? path : absolute).string();
}

int runSelfplay(int games, int levelA, int levelB) {
    BotMatch match(levelA, levelB, games);
    std::printf("Bot match: %s vs %s, %d game(s), colors swap every game\n", match.botName(0).c_str(),
                match.botName(1).c_str(), games);
    while (!match.finished()) {
        int round = match.gamesPlayed + 1;
        int white = match.whiteBot(round);
        std::printf("game %d/%d: %s (White) vs %s (Black)...\n", round, games, match.botName(white).c_str(),
                    match.botName(1 - white).c_str());
        std::fflush(stdout);

        // One table per bot, fresh every game: a shared table would hand the
        // weaker bot the stronger one's deeper search results, and one kept
        // across games would leak one game into the next.
        TranspositionTable tt[2] = {TranspositionTable(kTTSize), TranspositionTable(kTTSize)};
        GameRecord game;
        while (!game.outcome().over) {
            Color side = game.board().sideToMove();
            int bot = match.botToMove(round, side);
            const DifficultyPreset& preset = kDifficultyPresets[match.level[bot]];
            SearchOptions options;
            options.gameHistory = game.positionHashes();
            game.play(findBestMove(game.board(), side, preset.depthCap, preset.timeBudgetMs, tt[bot], options));
        }

        GameOutcome outcome = game.outcome();
        std::string path = match.recordFinishedGame(game);
        std::printf("game %d/%d: %s, %s after %zu moves -> %s\n", round, games, outcome.result.c_str(),
                    outcome.reason.c_str(), (game.sanMoves().size() + 1) / 2,
                    path.empty() ? "(PGN not saved)" : path.c_str());
        std::fflush(stdout);
    }
    std::printf("final score: %s %.1f - %.1f %s (wins %d-%d, %d draw(s))\n", match.botName(0).c_str(),
                match.wins[0] + match.draws / 2.0, match.wins[1] + match.draws / 2.0, match.botName(1).c_str(),
                match.wins[0], match.wins[1], match.draws);
    return 0;
}
