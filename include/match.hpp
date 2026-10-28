#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "board.hpp"
#include "move.hpp"

// Strength presets: a search depth cap plus a per-move time budget, passed
// straight to findBestMove. Shared by the GUI's vs-computer difficulty menu
// and by bot-vs-bot matches (GUI and --selfplay).
struct DifficultyPreset {
    const char* label;
    int depthCap;
    int timeBudgetMs;
    int softTimeMs; // 0 = none; else stop deepening past this (scaled by search stability)
};

constexpr DifficultyPreset kDifficultyPresets[4] = {
    {"EASY", 3, 300, 0},
    {"MEDIUM", 6, 1200, 0},
    {"HARD", 10, 3000, 0},
    {"EXPERT", 60, 6400, 3500},
};

// Index into kDifficultyPresets for a label, case-insensitively ("hard",
// "EXPERT"), or -1 if there's no such preset.
int presetIndexByName(const std::string& name);

// Only kings plus at most one knight or bishop in total: nobody can ever deliver mate.
bool isInsufficientMaterial(const Board& board);

struct GameOutcome {
    bool over = false;
    std::string result = "*"; // "1-0", "0-1", "1/2-1/2"; "*" while still in progress
    std::string reason;       // e.g. "checkmate", "threefold repetition"; empty while in progress
};

// One game from the standard starting position, recorded move by move.
// Board itself only knows checkmate and stalemate -- the search never
// needed repetition or the 50-move rule -- so this is where a bot-vs-bot
// game gets those draw rules. Without them, two bots in a drawn endgame
// would shuffle pieces forever.
class GameRecord {
public:
    GameRecord();

    const Board& board() const { return board_; }
    const std::vector<std::string>& sanMoves() const { return san_; }
    const std::vector<uint64_t>& positionHashes() const { return positionHashes_; }

    // `m` must be legal in board().
    void play(const Move& m);
    GameOutcome outcome() const;

private:
    Board board_;
    std::vector<std::string> san_;
    std::vector<uint64_t> positionHashes_; // every position so far, the current one last
    int halfmoveClock_ = 0;                // plies since the last pawn move or capture
};

// Match-level bookkeeping shared by --selfplay and the GUI's bot-vs-bot
// mode: which bot has which color in each game, the running score, and
// where each finished game's PGN goes. Bots are 0 ("Bot A") and 1
// ("Bot B"); Bot A is White in odd-numbered games, Black in even ones.
struct BotMatch {
    int level[2];     // kDifficultyPresets index for Bot A, Bot B
    int totalGames;
    int gamesPlayed = 0;
    int wins[2] = {0, 0};
    int draws = 0;
    std::string stamp; // local start time, YYYYMMDD_HHMMSS; names every file of this match

    BotMatch(int levelA, int levelB, int games);

    bool finished() const { return gamesPlayed >= totalGames; }
    int whiteBot(int round) const { return round % 2 == 1 ? 0 : 1; } // round is 1-based
    int botToMove(int round, Color side) const { return side == Color::White ? whiteBot(round) : 1 - whiteBot(round); }
    std::string botName(int bot) const; // "Bot A (HARD)"

    // Scores the finished `game` as round gamesPlayed + 1 and saves it as
    // games/botmatch_<stamp>_game<NN>.pgn. Returns that path, or an empty
    // string if the file couldn't be written (the reason goes to stderr;
    // the game still counts toward the score).
    std::string recordFinishedGame(const GameRecord& game);
};

// Headless bot-vs-bot match for the --selfplay command line mode: plays
// `games` games between presets levelA and levelB, printing one line per
// game plus a final score. Returns a process exit code.
int runSelfplay(int games, int levelA, int levelB);
