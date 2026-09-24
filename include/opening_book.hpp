#pragma once

#include <vector>

#include "board.hpp"
#include "move.hpp"

// One known-book continuation for a position, carrying a rough
// opening-theory soundness weight (roughly 1-100; higher = more
// established/main-line) alongside the move itself. The weight lets the
// engine pick its strongest known reply when a position has several -- see
// findBestMove()'s use of this for the side playing Black.
struct BookEntry {
    Move move;
    int weight;
};

// Returns the known book replies for the given position (each with its
// soundness weight), or an empty vector if the position isn't part of any
// known opening line.
std::vector<BookEntry> lookupBook(const Board& board);
