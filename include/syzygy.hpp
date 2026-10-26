#pragma once

#include <string>

#include "board.hpp"
#include "move.hpp"

// Syzygy endgame tablebases through Fathom (third_party/fathom). With no tables loaded -- or in a build
// without CHESS_SYZYGY -- largest() is 0 and nothing here changes the search.
namespace syzygy {

enum class Wdl { Loss, BlessedLoss, Draw, CursedWin, Win }; // for the side to move

// Loads the tables in `path` (directories separated by ':', ';' on Windows); an empty path unloads them.
// `message` says what was found. False only if the library failed to start.
bool init(const std::string& path, std::string& message);

// The most pieces any loaded table covers (0: none, also while the search is switched off from them).
int largest();
// For training-data generation: hide the tables from the search (largest() reports 0) and use them only through
// tableSize() and probeLabel().
void setSearchUse(bool on);
int tableSize();
// The theoretical result of the position for the side to move, ignoring the fifty-move clock; cursed wins and
// blessed losses count as draws. False if there is no table for it (or castling rights remain).
bool probeLabel(const Board& board, Wdl& wdl);
// Probes only below this many remaining plies of depth (UCI "SyzygyProbeDepth").
int probeDepth();
void setProbeDepth(int depth);

// Result for the search: needs the fifty-move clock at 0 and no castling rights, and at most largest() pieces.
bool probeWdl(const Board& board, Wdl& wdl);

// The best move of the position by DTZ (it keeps the result and respects the fifty-move rule).
bool probeRoot(const Board& board, Move& best, Wdl& wdl);

// Successful probes since resetHits() (UCI "tbhits").
long long hits();
void resetHits();

} // namespace syzygy
