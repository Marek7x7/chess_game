#include "opening_book.hpp"

#include <cstdio>
#include <unordered_map>

namespace {

struct Coord {
    int x, y;
};

constexpr Coord sq(char file, char rank) { return {file - 'a', rank - '1'}; }

using Line = std::vector<Coord>;

struct NamedLine {
    const char* name;
    // Rough opening-theory soundness/main-line-ness, ~1-100. Used only to
    // pick the strongest known reply when the side to move is Black and a
    // position has more than one known continuation -- see bookIndex().
    int weight;
    Line moves;
};

// A broad set of well-known opening lines (both White's own repertoire and
// Black's replies to it) so the AI develops recognizable openings instead of
// relying purely on search, which would still be deterministic move-to-move
// without this. Coordinates come in (from, to) pairs, alternating White and
// Black starting with White's first move.
const std::vector<NamedLine>& openingLines() {
    static const std::vector<NamedLine> lines = {
        // --- 1.e4 e5 open games ---
        {"Italian Game (Two Knights Defense)", 85,
         {sq('e', '2'), sq('e', '4'), sq('e', '7'), sq('e', '5'), sq('g', '1'), sq('f', '3'),
          sq('b', '8'), sq('c', '6'), sq('f', '1'), sq('c', '4'), sq('g', '8'), sq('f', '6')}},
        {"Italian Game (Giuoco Piano)", 88,
         {sq('e', '2'), sq('e', '4'), sq('e', '7'), sq('e', '5'), sq('g', '1'), sq('f', '3'),
          sq('b', '8'), sq('c', '6'), sq('f', '1'), sq('c', '4'), sq('f', '8'), sq('c', '5'),
          sq('c', '2'), sq('c', '3'), sq('g', '8'), sq('f', '6')}},
        {"Ruy Lopez (Morphy Defense)", 95,
         {sq('e', '2'), sq('e', '4'), sq('e', '7'), sq('e', '5'), sq('g', '1'), sq('f', '3'),
          sq('b', '8'), sq('c', '6'), sq('f', '1'), sq('b', '5'), sq('a', '7'), sq('a', '6'),
          sq('b', '5'), sq('a', '4'), sq('g', '8'), sq('f', '6')}},
        {"Ruy Lopez (Berlin Defense)", 93,
         {sq('e', '2'), sq('e', '4'), sq('e', '7'), sq('e', '5'), sq('g', '1'), sq('f', '3'),
          sq('b', '8'), sq('c', '6'), sq('f', '1'), sq('b', '5'), sq('g', '8'), sq('f', '6')}},
        {"Scotch Game", 80,
         {sq('e', '2'), sq('e', '4'), sq('e', '7'), sq('e', '5'), sq('g', '1'), sq('f', '3'),
          sq('b', '8'), sq('c', '6'), sq('d', '2'), sq('d', '4'), sq('e', '5'), sq('d', '4'),
          sq('f', '3'), sq('d', '4')}},
        {"Petrov's Defense", 85,
         {sq('e', '2'), sq('e', '4'), sq('e', '7'), sq('e', '5'), sq('g', '1'), sq('f', '3'),
          sq('g', '8'), sq('f', '6'), sq('f', '3'), sq('e', '5'), sq('d', '7'), sq('d', '6'),
          sq('e', '5'), sq('f', '3'), sq('f', '6'), sq('e', '4')}},
        {"Vienna Game", 72,
         {sq('e', '2'), sq('e', '4'), sq('e', '7'), sq('e', '5'), sq('b', '1'), sq('c', '3'),
          sq('g', '8'), sq('f', '6')}},
        {"King's Gambit Accepted", 62,
         {sq('e', '2'), sq('e', '4'), sq('e', '7'), sq('e', '5'), sq('f', '2'), sq('f', '4'),
          sq('e', '5'), sq('f', '4'), sq('g', '1'), sq('f', '3')}},
        {"Philidor Defense", 55,
         {sq('e', '2'), sq('e', '4'), sq('e', '7'), sq('e', '5'), sq('g', '1'), sq('f', '3'),
          sq('d', '7'), sq('d', '6')}},

        // --- 1.e4 c5 Sicilian ---
        {"Sicilian Defense (Najdorf)", 98,
         {sq('e', '2'), sq('e', '4'), sq('c', '7'), sq('c', '5'), sq('g', '1'), sq('f', '3'),
          sq('d', '7'), sq('d', '6'), sq('d', '2'), sq('d', '4'), sq('c', '5'), sq('d', '4'),
          sq('f', '3'), sq('d', '4'), sq('g', '8'), sq('f', '6'), sq('b', '1'), sq('c', '3'),
          sq('a', '7'), sq('a', '6')}},
        {"Sicilian Defense (Dragon)", 90,
         {sq('e', '2'), sq('e', '4'), sq('c', '7'), sq('c', '5'), sq('g', '1'), sq('f', '3'),
          sq('d', '7'), sq('d', '6'), sq('d', '2'), sq('d', '4'), sq('c', '5'), sq('d', '4'),
          sq('f', '3'), sq('d', '4'), sq('g', '8'), sq('f', '6'), sq('b', '1'), sq('c', '3'),
          sq('g', '7'), sq('g', '6')}},
        {"Sicilian Defense (Classical)", 86,
         {sq('e', '2'), sq('e', '4'), sq('c', '7'), sq('c', '5'), sq('g', '1'), sq('f', '3'),
          sq('b', '8'), sq('c', '6'), sq('d', '2'), sq('d', '4'), sq('c', '5'), sq('d', '4'),
          sq('f', '3'), sq('d', '4'), sq('g', '8'), sq('f', '6'), sq('b', '1'), sq('c', '3'),
          sq('d', '7'), sq('d', '6')}},
        {"Sicilian Defense (Closed)", 78,
         {sq('e', '2'), sq('e', '4'), sq('c', '7'), sq('c', '5'), sq('b', '1'), sq('c', '3'),
          sq('b', '8'), sq('c', '6'), sq('g', '2'), sq('g', '3'), sq('g', '7'), sq('g', '6')}},

        // --- 1.e4 e6 French ---
        {"French Defense (Classical)", 84,
         {sq('e', '2'), sq('e', '4'), sq('e', '7'), sq('e', '6'), sq('d', '2'), sq('d', '4'),
          sq('d', '7'), sq('d', '5'), sq('b', '1'), sq('c', '3'), sq('g', '8'), sq('f', '6')}},
        {"French Defense (Winawer)", 88,
         {sq('e', '2'), sq('e', '4'), sq('e', '7'), sq('e', '6'), sq('d', '2'), sq('d', '4'),
          sq('d', '7'), sq('d', '5'), sq('b', '1'), sq('c', '3'), sq('f', '8'), sq('b', '4')}},
        {"French Defense (Advance)", 80,
         {sq('e', '2'), sq('e', '4'), sq('e', '7'), sq('e', '6'), sq('d', '2'), sq('d', '4'),
          sq('d', '7'), sq('d', '5'), sq('e', '4'), sq('e', '5'), sq('c', '7'), sq('c', '5')}},

        // --- 1.e4 c6 Caro-Kann ---
        {"Caro-Kann Defense (Classical)", 88,
         {sq('e', '2'), sq('e', '4'), sq('c', '7'), sq('c', '6'), sq('d', '2'), sq('d', '4'),
          sq('d', '7'), sq('d', '5'), sq('b', '1'), sq('c', '3'), sq('d', '5'), sq('e', '4'),
          sq('c', '3'), sq('e', '4'), sq('c', '8'), sq('f', '5')}},
        {"Caro-Kann Defense (Advance)", 84,
         {sq('e', '2'), sq('e', '4'), sq('c', '7'), sq('c', '6'), sq('d', '2'), sq('d', '4'),
          sq('d', '7'), sq('d', '5'), sq('e', '4'), sq('e', '5'), sq('c', '8'), sq('f', '5')}},

        // --- other 1.e4 replies ---
        {"Pirc Defense", 76,
         {sq('e', '2'), sq('e', '4'), sq('d', '7'), sq('d', '6'), sq('d', '2'), sq('d', '4'),
          sq('g', '8'), sq('f', '6'), sq('b', '1'), sq('c', '3'), sq('g', '7'), sq('g', '6')}},
        {"Modern Defense", 70,
         {sq('e', '2'), sq('e', '4'), sq('g', '7'), sq('g', '6'), sq('d', '2'), sq('d', '4'),
          sq('f', '8'), sq('g', '7')}},
        {"Scandinavian Defense", 68,
         {sq('e', '2'), sq('e', '4'), sq('d', '7'), sq('d', '5'), sq('e', '4'), sq('d', '5'),
          sq('d', '8'), sq('d', '5'), sq('b', '1'), sq('c', '3'), sq('d', '5'), sq('a', '5')}},
        {"Alekhine's Defense", 66,
         {sq('e', '2'), sq('e', '4'), sq('g', '8'), sq('f', '6'), sq('e', '4'), sq('e', '5'),
          sq('f', '6'), sq('d', '5'), sq('d', '2'), sq('d', '4'), sq('d', '7'), sq('d', '6')}},

        // --- 1.d4 d5 queen's pawn ---
        {"Queen's Gambit Declined", 92,
         {sq('d', '2'), sq('d', '4'), sq('d', '7'), sq('d', '5'), sq('c', '2'), sq('c', '4'),
          sq('e', '7'), sq('e', '6'), sq('b', '1'), sq('c', '3'), sq('g', '8'), sq('f', '6'),
          sq('c', '1'), sq('g', '5'), sq('f', '8'), sq('e', '7')}},
        {"Queen's Gambit Accepted", 85,
         {sq('d', '2'), sq('d', '4'), sq('d', '7'), sq('d', '5'), sq('c', '2'), sq('c', '4'),
          sq('d', '5'), sq('c', '4'), sq('g', '1'), sq('f', '3'), sq('g', '8'), sq('f', '6')}},
        {"Slav Defense", 90,
         {sq('d', '2'), sq('d', '4'), sq('d', '7'), sq('d', '5'), sq('c', '2'), sq('c', '4'),
          sq('c', '7'), sq('c', '6'), sq('g', '1'), sq('f', '3'), sq('g', '8'), sq('f', '6')}},
        {"London System", 80,
         {sq('d', '2'), sq('d', '4'), sq('d', '7'), sq('d', '5'), sq('g', '1'), sq('f', '3'),
          sq('g', '8'), sq('f', '6'), sq('c', '1'), sq('f', '4'), sq('e', '7'), sq('e', '6'),
          sq('e', '2'), sq('e', '3'), sq('c', '7'), sq('c', '5')}},

        // --- 1.d4 Nf6 Indian systems ---
        {"King's Indian Defense", 92,
         {sq('d', '2'), sq('d', '4'), sq('g', '8'), sq('f', '6'), sq('c', '2'), sq('c', '4'),
          sq('g', '7'), sq('g', '6'), sq('b', '1'), sq('c', '3'), sq('f', '8'), sq('g', '7'),
          sq('e', '2'), sq('e', '4'), sq('d', '7'), sq('d', '6')}},
        {"Nimzo-Indian Defense", 95,
         {sq('d', '2'), sq('d', '4'), sq('g', '8'), sq('f', '6'), sq('c', '2'), sq('c', '4'),
          sq('e', '7'), sq('e', '6'), sq('b', '1'), sq('c', '3'), sq('f', '8'), sq('b', '4')}},
        {"Gruenfeld Defense", 90,
         {sq('d', '2'), sq('d', '4'), sq('g', '8'), sq('f', '6'), sq('c', '2'), sq('c', '4'),
          sq('g', '7'), sq('g', '6'), sq('b', '1'), sq('c', '3'), sq('d', '7'), sq('d', '5')}},
        {"Queen's Indian Defense", 87,
         {sq('d', '2'), sq('d', '4'), sq('g', '8'), sq('f', '6'), sq('c', '2'), sq('c', '4'),
          sq('e', '7'), sq('e', '6'), sq('g', '1'), sq('f', '3'), sq('b', '7'), sq('b', '6')}},
        {"Modern Benoni", 76,
         {sq('d', '2'), sq('d', '4'), sq('g', '8'), sq('f', '6'), sq('c', '2'), sq('c', '4'),
          sq('c', '7'), sq('c', '5'), sq('d', '4'), sq('d', '5'), sq('e', '7'), sq('e', '6')}},

        // --- other 1.d4 replies ---
        {"Dutch Defense (Leningrad)", 74,
         {sq('d', '2'), sq('d', '4'), sq('f', '7'), sq('f', '5'), sq('g', '2'), sq('g', '3'),
          sq('g', '8'), sq('f', '6'), sq('f', '1'), sq('g', '2'), sq('g', '7'), sq('g', '6')}},

        // --- flank openings ---
        {"English Opening (Reversed Sicilian)", 78,
         {sq('c', '2'), sq('c', '4'), sq('e', '7'), sq('e', '5'), sq('b', '1'), sq('c', '3'),
          sq('g', '8'), sq('f', '6')}},
        {"English Opening (Symmetrical)", 78,
         {sq('c', '2'), sq('c', '4'), sq('c', '7'), sq('c', '5'), sq('g', '1'), sq('f', '3'),
          sq('g', '8'), sq('f', '6')}},
        {"Reti Opening", 76,
         {sq('g', '1'), sq('f', '3'), sq('d', '7'), sq('d', '5'), sq('c', '2'), sq('c', '4'),
          sq('e', '7'), sq('e', '6'), sq('g', '2'), sq('g', '3'), sq('g', '8'), sq('f', '6')}},
    };
    return lines;
}

// If `m` is already recorded at `entries`, keeps the higher of the two
// weights (the same move can be reached via several lines with different
// soundness ratings -- e.g. 1.d4 Nf6 is both the King's Indian and the
// Nimzo-Indian's first move, and the more established line's rating should
// win) and returns true; otherwise leaves `entries` untouched and returns
// false, so the caller knows to insert a new entry.
bool mergeMove(std::vector<BookEntry>& entries, const Move& m, int weight) {
    for (BookEntry& e : entries) {
        if (e.move.fromX == m.fromX && e.move.fromY == m.fromY && e.move.toX == m.toX && e.move.toY == m.toY) {
            e.weight = std::max(e.weight, weight);
            return true;
        }
    }
    return false;
}

// Maps position hash -> the book move(s) known to follow it, each carrying
// its line's soundness weight. Replaying each line through
// Board::findLegalMove reuses the engine's own move-legality code instead of
// re-implementing move parsing/validation here. If a line's coordinates
// don't form a legal sequence, findLegalMove fails and the line is silently
// truncated there -- flagged to stderr so a typo in openingLines() doesn't
// go unnoticed.
const std::unordered_map<uint64_t, std::vector<BookEntry>>& bookIndex() {
    static const std::unordered_map<uint64_t, std::vector<BookEntry>> index = [] {
        std::unordered_map<uint64_t, std::vector<BookEntry>> map;
        for (const NamedLine& line : openingLines()) {
            Board board;
            size_t i = 0;
            for (; i + 1 < line.moves.size(); i += 2) {
                Coord from = line.moves[i];
                Coord to = line.moves[i + 1];
                Move m;
                if (!board.findLegalMove(from.x, from.y, to.x, to.y, PieceType::None, m)) break;
                std::vector<BookEntry>& entries = map[board.hashKey()];
                if (!mergeMove(entries, m, line.weight)) entries.push_back({m, line.weight});
                board.makeMove(m);
            }
            if (i + 1 < line.moves.size()) {
                std::fprintf(stderr, "[opening_book] WARNING: line \"%s\" has an illegal move at half-move %zu\n",
                             line.name, i + 1);
            }
        }
        return map;
    }();
    return index;
}

} // namespace

std::vector<BookEntry> lookupBook(const Board& board) {
    const auto& index = bookIndex();
    auto it = index.find(board.hashKey());
    if (it == index.end()) return {};
    return it->second;
}
