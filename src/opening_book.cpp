#include "opening_book.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <tuple>
#include <sstream>
#include <unordered_map>

#ifdef CHESS_BOOK_BUILTIN
// The generated book compiled into the executable (made by tools/embed_file.cmake).
extern "C" {
extern const unsigned char chess_book_builtin[];
extern const unsigned long chess_book_builtin_size;
}
#endif

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

// ---- The generated book ---------------------------------------------------
// File: "CGBK", u32 version (1), u32 record count, then records sorted by key, little-endian:
// u64 position key (Board::hashKey), u8 from square, u8 to square (file * 8 + rank), u8 promotion (the
// PieceType value), u8 weight.
constexpr char kMagic[4] = {'C', 'G', 'B', 'K'};
constexpr uint32_t kVersion = 1;
constexpr size_t kRecordBytes = 12;

struct Record {
    uint64_t key;
    uint8_t from, to, promotion, weight;
};

#ifdef CHESS_BOOK_BUILTIN
bool parseBook(const unsigned char* data, size_t size, std::vector<Record>& out) {
    if (size < 12 || std::memcmp(data, kMagic, 4) != 0) return false;
    uint32_t version, count;
    std::memcpy(&version, data + 4, 4);
    std::memcpy(&count, data + 8, 4);
    if (version != kVersion || size != 12 + static_cast<size_t>(count) * kRecordBytes) return false;
    out.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char* r = data + 12 + static_cast<size_t>(i) * kRecordBytes;
        std::memcpy(&out[i].key, r, 8);
        out[i].from = r[8];
        out[i].to = r[9];
        out[i].promotion = r[10];
        out[i].weight = r[11];
    }
    return true;
}
#endif

const std::vector<Record>& generatedBook() {
    static const std::vector<Record> book = [] {
        std::vector<Record> records;
#ifdef CHESS_BOOK_BUILTIN
        if (!parseBook(chess_book_builtin, chess_book_builtin_size, records)) {
            std::fprintf(stderr, "[opening_book] the built-in book is malformed and is ignored\n");
            records.clear();
        }
#endif
        return records;
    }();
    return book;
}

} // namespace

size_t generatedBookPositions() {
    const auto& book = generatedBook();
    size_t positions = 0;
    for (size_t i = 0; i < book.size(); i++)
        if (i == 0 || book[i].key != book[i - 1].key) positions++;
    return positions;
}

std::vector<BookEntry> lookupBook(const Board& board) {
    const auto& index = bookIndex(); // (built first, so that a bad hand-written line is always reported)
    const auto& book = generatedBook();
    const uint64_t key = board.hashKey();
    auto it = std::lower_bound(book.begin(), book.end(), key, [](const Record& r, uint64_t k) { return r.key < k; });
    if (it != book.end() && it->key == key) {
        std::vector<BookEntry> entries;
        for (; it != book.end() && it->key == key; ++it) {
            Move m;
            m.fromX = static_cast<int8_t>(it->from / 8);
            m.fromY = static_cast<int8_t>(it->from % 8);
            m.toX = static_cast<int8_t>(it->to / 8);
            m.toY = static_cast<int8_t>(it->to % 8);
            m.promotion = static_cast<PieceType>(it->promotion);
            entries.push_back({m, it->weight}); // the caller checks the move against the legal moves
        }
        return entries;
    }
    auto hand = index.find(key);
    if (hand == index.end()) return {};
    return hand->second;
}

bool buildBookFile(const std::string& linesPath, const std::string& outPath, int minCount, std::string& message) {
    std::ifstream in(linesPath);
    if (!in) {
        message = "cannot read " + linesPath;
        return false;
    }
    // position key -> (from, to, promotion) -> games
    struct MoveKey {
        uint8_t from, to, promotion;
        bool operator<(const MoveKey& o) const {
            return std::tie(from, to, promotion) < std::tie(o.from, o.to, o.promotion);
        }
    };
    std::map<uint64_t, std::map<MoveKey, long long>> positions;
    std::string line;
    size_t lineNo = 0;
    while (std::getline(in, line)) {
        lineNo++;
        std::istringstream words(line);
        std::vector<std::string> tok;
        for (std::string w; words >> w;) tok.push_back(w);
        if (tok.size() < 2) continue;
        const long long count = std::atoll(tok.back().c_str());
        Board board;
        for (size_t i = 0; i + 1 < tok.size(); i++) {
            const std::string& u = tok[i];
            if (u.size() < 4) {
                message = "line " + std::to_string(lineNo) + ": bad move '" + u + "'";
                return false;
            }
            PieceType promotion = PieceType::None;
            if (u.size() == 5) {
                switch (u[4]) {
                    case 'q': promotion = PieceType::Queen; break;
                    case 'r': promotion = PieceType::Rook; break;
                    case 'b': promotion = PieceType::Bishop; break;
                    case 'n': promotion = PieceType::Knight; break;
                    default: break;
                }
            }
            Move m;
            if (!board.findLegalMove(u[0] - 'a', u[1] - '1', u[2] - 'a', u[3] - '1', promotion, m)) {
                message = "line " + std::to_string(lineNo) + ": illegal move " + u;
                return false;
            }
            if (i + 2 == tok.size()) { // the last move is the book move for this position
                const MoveKey mk{static_cast<uint8_t>(m.fromX * 8 + m.fromY), static_cast<uint8_t>(m.toX * 8 + m.toY),
                                 static_cast<uint8_t>(m.promotion)};
                positions[board.hashKey()][mk] += count;
            }
            board.makeMove(m);
        }
    }
    std::vector<Record> records;
    for (const auto& [key, moves] : positions) {
        long long top = 0;
        for (const auto& [mk, n] : moves) top = std::max(top, n);
        for (const auto& [mk, n] : moves) {
            if (n < minCount || n * 5 < top) continue;
            records.push_back({key, mk.from, mk.to, mk.promotion,
                               static_cast<uint8_t>(60 + (40 * n + top / 2) / top)});
        }
    }
    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        message = "cannot write " + outPath;
        return false;
    }
    const uint32_t version = kVersion, count = static_cast<uint32_t>(records.size());
    out.write(kMagic, 4);
    out.write(reinterpret_cast<const char*>(&version), 4);
    out.write(reinterpret_cast<const char*>(&count), 4);
    for (const Record& r : records) {
        out.write(reinterpret_cast<const char*>(&r.key), 8);
        out.put(static_cast<char>(r.from));
        out.put(static_cast<char>(r.to));
        out.put(static_cast<char>(r.promotion));
        out.put(static_cast<char>(r.weight));
    }
    if (!out) {
        message = "write failed: " + outPath;
        return false;
    }
    message = std::to_string(positions.size()) + " positions read, " + std::to_string(records.size()) +
              " book moves written";
    return true;
}
