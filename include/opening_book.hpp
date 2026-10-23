#pragma once

#include <cstddef>
#include <string>
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

// The generated book: how often strong players chose each move in each position (from games by
// 2200+ rated players), built by `chess --build-book` from tools/book/extract_lines.py's output and
// compiled in from book/default.book. lookupBook() prefers it and falls back to the hand-written lines
// for positions it does not cover. Its weights are 60 (a move a fifth as popular as the top one) to 100.
//
// Reads the sequences in `linesPath` ("e2e4 e7e5 ... <count>", the last move being the book move), adds
// up positions that different move orders reach, drops moves played less than `minCount` times or less than
// a fifth as often as the most popular one, and writes the binary book to `outPath`. False (with a
// message) if a sequence is not a legal game or a file cannot be written.
bool buildBookFile(const std::string& linesPath, const std::string& outPath, int minCount, std::string& message);

// Number of positions the compiled-in generated book covers (0: none).
size_t generatedBookPositions();
