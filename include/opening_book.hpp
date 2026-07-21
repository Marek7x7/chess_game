#pragma once

#include <vector>

#include "board.hpp"
#include "move.hpp"

// Returns the known book moves for the given position, or an empty vector
// if the position isn't part of any known opening line.
std::vector<Move> lookupBook(const Board& board);
