#include "opening_book.hpp"

#include <unordered_map>

namespace {

struct Coord {
    int x, y;
};

constexpr Coord sq(char file, char rank) { return {file - 'a', rank - '1'}; }

using Line = std::vector<Coord>;

// A handful of well-known opening lines (a few half-moves each) so the AI
// develops recognizable openings instead of relying purely on search, which
// would still be deterministic move-to-move without this. Coordinates come
// in (from, to) pairs.
const std::vector<Line>& openingLines() {
    static const std::vector<Line> lines = {
        // Italian Game
        {sq('e', '2'), sq('e', '4'), sq('e', '7'), sq('e', '5'), sq('g', '1'), sq('f', '3'),
         sq('b', '8'), sq('c', '6'), sq('f', '1'), sq('c', '4'), sq('g', '8'), sq('f', '6')},
        // Ruy Lopez
        {sq('e', '2'), sq('e', '4'), sq('e', '7'), sq('e', '5'), sq('g', '1'), sq('f', '3'),
         sq('b', '8'), sq('c', '6'), sq('f', '1'), sq('b', '5'), sq('a', '7'), sq('a', '6')},
        // Queen's Gambit Declined
        {sq('d', '2'), sq('d', '4'), sq('d', '7'), sq('d', '5'), sq('c', '2'), sq('c', '4'),
         sq('e', '7'), sq('e', '6'), sq('b', '1'), sq('c', '3'), sq('g', '8'), sq('f', '6')},
        // Sicilian Defense (Open)
        {sq('e', '2'), sq('e', '4'), sq('c', '7'), sq('c', '5'), sq('g', '1'), sq('f', '3'),
         sq('d', '7'), sq('d', '6'), sq('d', '2'), sq('d', '4'), sq('c', '5'), sq('d', '4')},
        // French Defense
        {sq('e', '2'), sq('e', '4'), sq('e', '7'), sq('e', '6'), sq('d', '2'), sq('d', '4'),
         sq('d', '7'), sq('d', '5'), sq('b', '1'), sq('c', '3'), sq('g', '8'), sq('f', '6')},
        // Caro-Kann Defense
        {sq('e', '2'), sq('e', '4'), sq('c', '7'), sq('c', '6'), sq('d', '2'), sq('d', '4'),
         sq('d', '7'), sq('d', '5'), sq('b', '1'), sq('c', '3'), sq('d', '5'), sq('e', '4')},
        // London System
        {sq('d', '2'), sq('d', '4'), sq('d', '7'), sq('d', '5'), sq('g', '1'), sq('f', '3'),
         sq('g', '8'), sq('f', '6'), sq('c', '1'), sq('f', '4'), sq('e', '7'), sq('e', '6')},
        // King's Indian setup
        {sq('d', '2'), sq('d', '4'), sq('g', '8'), sq('f', '6'), sq('c', '2'), sq('c', '4'),
         sq('g', '7'), sq('g', '6'), sq('b', '1'), sq('c', '3'), sq('f', '8'), sq('g', '7')},
    };
    return lines;
}

bool containsMove(const std::vector<Move>& moves, const Move& m) {
    for (const Move& existing : moves) {
        if (existing.fromX == m.fromX && existing.fromY == m.fromY &&
            existing.toX == m.toX && existing.toY == m.toY)
            return true;
    }
    return false;
}

// Maps position hash -> the book move(s) known to follow it. Replaying each
// line through Board::findLegalMove reuses the engine's own move-legality
// code instead of re-implementing move parsing/validation here.
const std::unordered_map<uint64_t, std::vector<Move>>& bookIndex() {
    static const std::unordered_map<uint64_t, std::vector<Move>> index = [] {
        std::unordered_map<uint64_t, std::vector<Move>> map;
        for (const Line& line : openingLines()) {
            Board board;
            for (size_t i = 0; i + 1 < line.size(); i += 2) {
                Coord from = line[i];
                Coord to = line[i + 1];
                Move m;
                if (!board.findLegalMove(from.x, from.y, to.x, to.y, PieceType::None, m)) break;
                std::vector<Move>& moves = map[board.hashKey()];
                if (!containsMove(moves, m)) moves.push_back(m);
                board.makeMove(m);
            }
        }
        return map;
    }();
    return index;
}

} // namespace

std::vector<Move> lookupBook(const Board& board) {
    const auto& index = bookIndex();
    auto it = index.find(board.hashKey());
    if (it == index.end()) return {};
    return it->second;
}
