#pragma once

#include <string>
#include <utility>
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

// The inverse of resolveSanMove: the SAN for legal move `m` in position
// `before` (the position m is played from), including the minimal
// disambiguation and a "+"/"#" suffix.
std::string moveToSan(const Board& before, const Move& m);

// A complete PGN game: tag pairs in the given order, a blank line, then
// numbered movetext wrapped at 80 columns and ending with `result`.
// sanMoves must start with a White move (i.e. a game from the standard
// starting position).
std::string formatPgn(const std::vector<std::pair<std::string, std::string>>& tags,
                      const std::vector<std::string>& sanMoves, const std::string& result);
