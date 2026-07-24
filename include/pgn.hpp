#pragma once

#include <string>
#include <vector>

#include "board.hpp"
#include "move.hpp"

// Parses a PGN's movetext into an ordered list of mainline SAN move tokens,
// discarding tags, NAGs ($n), result markers, and parenthesized (RAV)
// variations — only the actual game continuation is returned.
std::vector<std::string> parsePgnMainline(const std::string& pgnText);

// Resolves a single SAN token (e.g. "Nbd7", "exd5", "O-O", "e8=Q+") against
// the current position by matching it against board.legalMoves(). Returns
// false if zero or more than one legal move matches (ambiguous/malformed).
bool resolveSanMove(const Board& board, const std::string& san, Move& out);
