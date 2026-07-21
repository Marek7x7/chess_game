#pragma once

#include "board.hpp"
#include "move.hpp"

// Picks a move for `aiColor`: an opening-book move if the position is a
// known line, otherwise an iterative-deepening alpha-beta search (with
// quiescence search, a transposition table, and move ordering) bounded by
// both `maxDepth` and `timeBudgetMs`, whichever is hit first. Caller must
// ensure aiColor has at least one legal move.
Move findBestMove(const Board& board, Color aiColor, int maxDepth, int timeBudgetMs);
