#include "board.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ostream>
#include <random>

#include "tune.hpp"

namespace {

const int kBishopDirs[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
const int kRookDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
const int kQueenDirs[8][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1},
                               {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
const int kKnightOffsets[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2},
                                    {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};

int pieceTypeIndex(PieceType t) { return static_cast<int>(t); }

// Phase weight of remaining non-pawn material (knight/bishop=1, rook=2,
// queen=4), used to interpolate the king's piece-square bonus between the
// opening/middlegame and endgame tables as material comes off the board.
int phaseValue(PieceType t) {
    switch (t) {
        case PieceType::Knight:
        case PieceType::Bishop: return 1;
        case PieceType::Rook: return 2;
        case PieceType::Queen: return 4;
        default: return 0;
    }
}
constexpr int kMaxPhase = 24; // 4 knights+4 bishops (1 each) + 4 rooks (2 each) + 2 queens (4 each)

// Standard "simplified evaluation function" piece-square tables (centipawns),
// written in published order: row 0 = rank 8, row 7 = rank 1, column 0 = file a.
TUNE_ARR2(kPawnTable, 8, 8, {
    {  0,   0,   0,   0,   0,   0,   0,   0},
    { 90,  56,  94,  35,  30,  52,  35,  30},
    { 24,  32,  22,  15,  40,  64,  24,  37},
    { -9,  20,   9,  35,  62,  27,  17,  17},
    {-12, -10,  14,   6,  11,  24, -14, -18},
    {-33, -31, -30, -32, -19,  12,  15,   1},
    {-10, -34, -34, -60, -30,  26,  28, -38},
    {  0,   0,   0,   0,   0,   0,   0,   0},
});
TUNE_ARR2(kKnightTable, 8, 8, {
    {-94, -34,   7, -14, -36, -35, -17, -94},
    {-84, -22,  14,  14,   6,  -7, -34, -54},
    {-44,  -2,  48,  46,  58,  24,  24, -15},
    {-15,  -9,  22,  46,  22,  30,  -8,   0},
    {-38, -14,  30,   6,  32,  24,   0, -16},
    {-74, -28, -10,  40,  28,   0,   0, -66},
    {-50, -38, -38, -11,  -7, -16, -34, -32},
    {-94, -66, -46, -46, -44, -50, -64, -64},
});
TUNE_ARR2(kBishopTable, 8, 8, {
    {-34,   4, -23, -24, -24,   2,   0, -25},
    {-19,   3,  14,  20,  14,   8, -12,   9},
    {  4,  14,   7,   7,  -4,  18,  14,  34},
    { -8,  -4,  20,  23,   3, -10,   1,  -8},
    { -4,   0,  -2,  21,  17,  -4,   0,   4},
    {  0,  20,  11,  13,  16,  12,  23,  -2},
    {-26,   9,  -4, -12,   5, -10,  44,  34},
    {-34, -18, -17,   4,   5, -18,  -3, -19},
});
TUNE_ARR2(kRookTable, 8, 8, {
    { 21,  29,  14,  44,  14,  14,  14,  38},
    { 20,  26,  20,  30,  24,   6,  18,  34},
    { 40,  44,  14,  44,  32,  44,  14,  30},
    { 10,  14,  14,  18,  24,  14,   6,  10},
    {  4,  10,   6, -10,   4,   6,  14,  -9},
    {-19, -14, -14,  -6, -21,  -4,   4,  -3},
    {-36, -30,  -6, -11, -14, -13, -29, -50},
    { -8,  -4,  10,   9,   9,  14, -14, -28},
});
TUNE_ARR2(kQueenTable, 8, 8, {
    { -5,   4,  20,  40,  10,  34,  34,  25},
    {-54, -44,  10,   5, -14,  44,  14,   4},
    {-22, -12,   2,   1,  50,  50,  44,  34},
    {-22, -44, -10,  -6,  43,  36,  13,  29},
    {-30, -30, -23, -10,   1,   7, -14,  -6},
    {-50, -26,   3, -10,  18,   7,  14, -26},
    {-48, -16,   6,  -4,  12, -14, -14,   4},
    {-12, -24,  -3,  -3,  -8, -54, -54, -34},
});
TUNE_ARR2(kKingMidTable, 8, 8, {
    {-43, -44, -50, -26, -19, -33, -28, -34},
    {  2,   3,  -4, -38, -52,  -2,   4, -19},
    { 12,   0, -25, -68, -37,   4,   5,   9},
    { -7,  -5, -60, -45, -59, -26,  -3, -18},
    { -5, -18,  -3, -48, -42, -19, -18, -59},
    {-19,  -5,  22, -10,  24,  -6,  -8, -20},
    { 64,  34,   0, -20, -15,   6,  20,   5},
    { 49,  74,  15, -43,  14, -34,  45,   4},
});
TUNE_ARR2(kKingEndTable, 8, 8, {
    {-65, -54,   8, -62,  18,   4, -26, -92},
    {-16,  18,  32,  18,   6,  34,  25, -16},
    { 12,  34,  34,  44,  49,  34,  35,  10},
    {-24,   4,  44,  28,  26,  48,  34, -16},
    {-14, -21,  15,  26,  28,  33,  22, -26},
    {-75, -25,   9,  21,  13,  35,   5, -34},
    {-45, -16,   4,  25,   1,  14, -17, -45},
    {-64, -45, -22, -18, -43, -37, -65, -90},
});

// Looks up `table` (published rank-8-first order) for a piece of `color` on
// (x, y), where y=0 is rank 1 in Board's own coordinate system. Mirrors for
// Black so both sides use the same "toward the center / toward the back
// rank" pattern.
int pst(const int table[8][8], Color color, int x, int y) {
    int effectiveY = (color == Color::White) ? y : 7 - y;
    int row = 7 - effectiveY;
    return table[row][x];
}

int pstFor(PieceType type, Color color, int x, int y) {
    switch (type) {
        case PieceType::Pawn: return pst(kPawnTable, color, x, y);
        case PieceType::Knight: return pst(kKnightTable, color, x, y);
        case PieceType::Bishop: return pst(kBishopTable, color, x, y);
        case PieceType::Rook: return pst(kRookTable, color, x, y);
        case PieceType::Queen: return pst(kQueenTable, color, x, y);
        default: return 0;
    }
}

int kingPst(Color color, int x, int y, double phase) {
    int mid = pst(kKingMidTable, color, x, y);
    int end = pst(kKingEndTable, color, x, y);
    return static_cast<int>(mid * phase + end * (1.0 - phase) + 0.5);
}

// Two bishops cover both square colors between them, which no other pair of
// minor pieces can, so the pair is worth more than two single bishops --
// most of all in open endgames. Middlegame / endgame values, interpolated by
// phase like the king's table.
TUNE_INT(kBishopPairMg, 31);
TUNE_INT(kBishopPairEg, 46);

// Positive = good for White.
int bishopPairTerm(const int (&bishops)[2], double phase) {
    int bonus = static_cast<int>(kBishopPairMg * phase + kBishopPairEg * (1.0 - phase) + 0.5);
    return (bishops[0] >= 2 ? bonus : 0) - (bishops[1] >= 2 ? bonus : 0);
}

// Endgame pawn values by rank counted from the pawn's own side. With few
// pieces left, what a pawn is worth is how close it is to promoting, not
// whether it controls the center, which is what kPawnTable (used, halved,
// in the middlegame) rewards.
TUNE_ARR(kPawnEgByRank, 8, {0, 0, 1, 1, 1, 21, 31, 0});

// How much a pawn's endgame square value differs from its halved
// middlegame one, positive = good for White.
int pawnEgShiftFor(Color color, int x, int y) {
    int rank = color == Color::White ? y : 7 - y;
    int shift = kPawnEgByRank[rank] - pst(kPawnTable, color, x, y) / 2;
    return color == Color::White ? shift : -shift;
}

// Positive = good for White. Positional bonuses are scaled down relative to
// material so a few plies of accumulated piece-square swings can't outweigh
// a clean material gain.
int pieceContribution(PieceType type, Color color, int x, int y) {
    int total = pieceValue(type) + pstFor(type, color, x, y) / 2;
    return (color == Color::White) ? total : -total;
}

// Random keys for incremental Zobrist hashing. Seeded with a fixed constant
// so hashes (and therefore search/opening-book behavior) are reproducible
// across runs for debugging.
struct ZobristKeys {
    uint64_t piece[2][7][8][8];
    uint64_t sideToMove;
    uint64_t castle[2][2]; // [color][0=kingside, 1=queenside]
    uint64_t enPassantFile[8];

    ZobristKeys() {
        std::mt19937_64 rng(0x9E3779B97F4A7C15ULL);
        for (auto& byColor : piece)
            for (auto& byType : byColor)
                for (auto& byFile : byType)
                    for (auto& key : byFile) key = rng();
        sideToMove = rng();
        for (auto& byColor : castle)
            for (auto& key : byColor) key = rng();
        for (auto& key : enPassantFile) key = rng();
    }
};

const ZobristKeys& zobrist() {
    static const ZobristKeys keys;
    return keys;
}

} // namespace

Board::Board() {
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++)
            squares[x][y] = Piece{};

    const PieceType backRank[8] = {
        PieceType::Rook, PieceType::Knight, PieceType::Bishop, PieceType::Queen,
        PieceType::King, PieceType::Bishop, PieceType::Knight, PieceType::Rook};

    for (int x = 0; x < 8; x++) {
        squares[x][0] = Piece{backRank[x], Color::White};
        squares[x][1] = Piece{PieceType::Pawn, Color::White};
        squares[x][6] = Piece{PieceType::Pawn, Color::Black};
        squares[x][7] = Piece{backRank[x], Color::Black};
    }

    turn = Color::White;
    castleKingside[0] = castleKingside[1] = true;
    castleQueenside[0] = castleQueenside[1] = true;
    enPassantX = -1;
    enPassantY = -1;

    recomputeHash();
    recomputeEval();
}

void Board::recomputeEval() {
    nonKingScore = 0;
    phaseUnits = 0;
    bishopCount[0] = bishopCount[1] = 0;
    pawnEgShift = 0;
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            const Piece& p = squares[x][y];
            if (p.isEmpty()) continue;
            if (p.type == PieceType::King) {
                kingX[colorIndex(p.color)] = x;
                kingY[colorIndex(p.color)] = y;
                continue;
            }
            nonKingScore += pieceContribution(p.type, p.color, x, y);
            phaseUnits += phaseValue(p.type);
            if (p.type == PieceType::Bishop) bishopCount[colorIndex(p.color)]++;
            if (p.type == PieceType::Pawn) pawnEgShift += pawnEgShiftFor(p.color, x, y);
        }
    }
}

int Board::kingTerm() const {
    double phase = gamePhase();
    int white = kingPst(Color::White, kingX[0], kingY[0], phase);
    int black = kingPst(Color::Black, kingX[1], kingY[1], phase);
    return white / 2 - black / 2;
}

int Board::rawEval() const {
#ifdef TUNING
    return rawEvalFromScratch(); // the tuner changes the tables the incremental sums were built with
#endif
    double phase = gamePhase();
    return nonKingScore + kingTerm() + bishopPairTerm(bishopCount, phase) +
           static_cast<int>(pawnEgShift * (1.0 - phase));
}

double Board::gamePhase() const { return std::min(1.0, phaseUnits / static_cast<double>(kMaxPhase)); }

int Board::rawEvalFromScratch() const {
    int score = 0;
    int phase = 0;
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            const Piece& p = squares[x][y];
            if (p.isEmpty()) continue;
            phase += phaseValue(p.type);
        }
    }
    double phaseFrac = std::min(1.0, phase / static_cast<double>(kMaxPhase));
    int bishops[2] = {0, 0};
    int pawnShift = 0;
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            const Piece& p = squares[x][y];
            if (p.isEmpty()) continue;
            if (p.type == PieceType::King) {
                int bonus = kingPst(p.color, x, y, phaseFrac);
                score += (p.color == Color::White) ? bonus / 2 : -(bonus / 2);
            } else {
                score += pieceContribution(p.type, p.color, x, y);
            }
            if (p.type == PieceType::Bishop) bishops[colorIndex(p.color)]++;
            if (p.type == PieceType::Pawn) pawnShift += pawnEgShiftFor(p.color, x, y);
        }
    }
    return score + bishopPairTerm(bishops, phaseFrac) + static_cast<int>(pawnShift * (1.0 - phaseFrac));
}

void Board::recomputeHash() { computeHashes(hash, pawnHash); }

uint64_t Board::hashKeyFromScratch() const {
    uint64_t full, pawns;
    computeHashes(full, pawns);
    return full;
}

uint64_t Board::pawnKeyFromScratch() const {
    uint64_t full, pawns;
    computeHashes(full, pawns);
    return pawns;
}

void Board::computeHashes(uint64_t& full, uint64_t& pawns) const {
    const ZobristKeys& z = zobrist();
    full = pawns = 0;
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            const Piece& p = squares[x][y];
            if (p.isEmpty()) continue;
            uint64_t key = z.piece[colorIndex(p.color)][pieceTypeIndex(p.type)][x][y];
            full ^= key;
            if (p.type == PieceType::Pawn) pawns ^= key;
        }
    }
    if (castleKingside[0]) full ^= z.castle[0][0];
    if (castleQueenside[0]) full ^= z.castle[0][1];
    if (castleKingside[1]) full ^= z.castle[1][0];
    if (castleQueenside[1]) full ^= z.castle[1][1];
    if (enPassantX != -1) full ^= z.enPassantFile[enPassantX];
    if (turn == Color::Black) full ^= z.sideToMove;
}

Board Board::fromFEN(const std::string& fen) {
    Board board;
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++)
            board.squares[x][y] = Piece{};
    board.castleKingside[0] = board.castleKingside[1] = false;
    board.castleQueenside[0] = board.castleQueenside[1] = false;
    board.enPassantX = board.enPassantY = -1;

    size_t pos = 0;
    auto nextField = [&]() {
        size_t start = fen.find_first_not_of(' ', pos);
        size_t end = fen.find(' ', start);
        pos = end;
        return fen.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };

    std::string placement = nextField();
    int x = 0, y = 7;
    for (char c : placement) {
        if (c == '/') {
            x = 0;
            y--;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            x += c - '0';
        } else {
            Color color = std::isupper(static_cast<unsigned char>(c)) ? Color::White : Color::Black;
            PieceType type = PieceType::None;
            switch (std::tolower(static_cast<unsigned char>(c))) {
                case 'p': type = PieceType::Pawn; break;
                case 'n': type = PieceType::Knight; break;
                case 'b': type = PieceType::Bishop; break;
                case 'r': type = PieceType::Rook; break;
                case 'q': type = PieceType::Queen; break;
                case 'k': type = PieceType::King; break;
                default: break;
            }
            board.squares[x][y] = Piece{type, color};
            x++;
        }
    }

    std::string activeColor = nextField();
    board.turn = (activeColor == "b") ? Color::Black : Color::White;

    std::string castling = nextField();
    for (char c : castling) {
        switch (c) {
            case 'K': board.castleKingside[0] = true; break;
            case 'Q': board.castleQueenside[0] = true; break;
            case 'k': board.castleKingside[1] = true; break;
            case 'q': board.castleQueenside[1] = true; break;
            default: break;
        }
    }

    std::string epField = nextField();
    if (epField != "-" && epField.size() == 2) {
        board.enPassantX = epField[0] - 'a';
        board.enPassantY = epField[1] - '1';
    }

    // The halfmove clock is optional (the GUI's custom setup leaves it out).
    if (pos != std::string::npos && fen.find_first_not_of(' ', pos) != std::string::npos) {
        board.halfmoveCount = std::atoi(nextField().c_str());
    }

    board.recomputeHash();
    board.recomputeEval();
    return board;
}

void Board::addSlidingMoves(MoveList& moves, int x, int y, Color color,
                             const int dirs[][2], int numDirs, bool noisyOnly) const {
    for (int d = 0; d < numDirs; d++) {
        int nx = x + dirs[d][0];
        int ny = y + dirs[d][1];
        while (inBounds(nx, ny)) {
            const Piece& target = squares[nx][ny];
            if (target.isEmpty()) {
                if (!noisyOnly) moves.push_back({x, y, nx, ny});
            } else {
                if (target.color != color) moves.push_back({x, y, nx, ny, PieceType::None, false, false, target.type});
                break;
            }
            nx += dirs[d][0];
            ny += dirs[d][1];
        }
    }
}

void Board::addPawnMoves(MoveList& moves, int x, int y, Color color, bool noisyOnly) const {
    int dir = (color == Color::White) ? 1 : -1;
    int startRank = (color == Color::White) ? 1 : 6;
    int promotionRank = (color == Color::White) ? 7 : 0;
    const PieceType promotions[4] = {PieceType::Queen, PieceType::Rook,
                                       PieceType::Bishop, PieceType::Knight};

    auto addForwardOrPromotion = [&](int nx, int ny, PieceType capturedType) {
        if (ny == promotionRank) {
            for (PieceType pt : promotions) moves.push_back({x, y, nx, ny, pt, false, false, capturedType});
        } else {
            moves.push_back({x, y, nx, ny, PieceType::None, false, false, capturedType});
        }
    };

    int ny = y + dir;
    if (inBounds(x, ny) && squares[x][ny].isEmpty()) {
        if (!noisyOnly) {
            addForwardOrPromotion(x, ny, PieceType::None);
            int ny2 = y + 2 * dir;
            if (y == startRank && squares[x][ny2].isEmpty()) {
                moves.push_back({x, y, x, ny2});
            }
        } else if (ny == promotionRank) {
            moves.push_back({x, y, x, ny, PieceType::Queen});
        }
    }

    for (int dx : {-1, 1}) {
        int nx = x + dx;
        if (!inBounds(nx, ny)) continue;
        const Piece& target = squares[nx][ny];
        if (!target.isEmpty() && target.color != color) {
            addForwardOrPromotion(nx, ny, target.type);
        } else if (nx == enPassantX && ny == enPassantY) {
            Move m{x, y, nx, ny};
            m.isEnPassant = true;
            m.captured = PieceType::Pawn;
            moves.push_back(m);
        }
    }
}

void Board::addKingMoves(MoveList& moves, int x, int y, Color color, bool noisyOnly) const {
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            if (dx == 0 && dy == 0) continue;
            int nx = x + dx, ny = y + dy;
            if (!inBounds(nx, ny)) continue;
            const Piece& target = squares[nx][ny];
            if (noisyOnly ? !target.isEmpty() && target.color != color : target.isEmpty() || target.color != color)
                moves.push_back({x, y, nx, ny, PieceType::None, false, false, target.type});
        }
    }

    if (noisyOnly || isInCheck(color)) return;
    int homeY = (color == Color::White) ? 0 : 7;
    if (x != 4 || y != homeY) return;
    Color enemy = opponent(color);
    int ci = colorIndex(color);

    if (castleKingside[ci] && squares[5][homeY].isEmpty() && squares[6][homeY].isEmpty() &&
        squares[7][homeY].type == PieceType::Rook && squares[7][homeY].color == color &&
        !isSquareAttacked(5, homeY, enemy) && !isSquareAttacked(6, homeY, enemy)) {
        Move m{x, y, 6, homeY};
        m.isCastle = true;
        moves.push_back(m);
    }
    if (castleQueenside[ci] && squares[1][homeY].isEmpty() && squares[2][homeY].isEmpty() &&
        squares[3][homeY].isEmpty() && squares[0][homeY].type == PieceType::Rook &&
        squares[0][homeY].color == color &&
        !isSquareAttacked(3, homeY, enemy) && !isSquareAttacked(2, homeY, enemy)) {
        Move m{x, y, 2, homeY};
        m.isCastle = true;
        moves.push_back(m);
    }
}

MoveList Board::pseudoLegalMoves(Color color, bool noisyOnly) const {
    MoveList moves;
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            const Piece& p = squares[x][y];
            if (p.color != color) continue;
            switch (p.type) {
                case PieceType::Pawn:
                    addPawnMoves(moves, x, y, color, noisyOnly);
                    break;
                case PieceType::Knight:
                    for (auto& off : kKnightOffsets) {
                        int nx = x + off[0], ny = y + off[1];
                        if (!inBounds(nx, ny)) continue;
                        const Piece& target = squares[nx][ny];
                        if (noisyOnly ? !target.isEmpty() && target.color != color
                                      : target.isEmpty() || target.color != color)
                            moves.push_back({x, y, nx, ny, PieceType::None, false, false, target.type});
                    }
                    break;
                case PieceType::Bishop:
                    addSlidingMoves(moves, x, y, color, kBishopDirs, 4, noisyOnly);
                    break;
                case PieceType::Rook:
                    addSlidingMoves(moves, x, y, color, kRookDirs, 4, noisyOnly);
                    break;
                case PieceType::Queen:
                    addSlidingMoves(moves, x, y, color, kQueenDirs, 8, noisyOnly);
                    break;
                case PieceType::King:
                    addKingMoves(moves, x, y, color, noisyOnly);
                    break;
                default:
                    break;
            }
        }
    }
    return moves;
}

bool Board::isSquareAttacked(int x, int y, Color byColor) const {
    int pawnDir = (byColor == Color::White) ? 1 : -1;
    int attackerY = y - pawnDir;
    for (int dx : {-1, 1}) {
        int ax = x + dx;
        if (inBounds(ax, attackerY) && squares[ax][attackerY].type == PieceType::Pawn &&
            squares[ax][attackerY].color == byColor)
            return true;
    }

    for (auto& off : kKnightOffsets) {
        int nx = x + off[0], ny = y + off[1];
        if (inBounds(nx, ny) && squares[nx][ny].type == PieceType::Knight &&
            squares[nx][ny].color == byColor)
            return true;
    }

    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            if (dx == 0 && dy == 0) continue;
            int nx = x + dx, ny = y + dy;
            if (inBounds(nx, ny) && squares[nx][ny].type == PieceType::King &&
                squares[nx][ny].color == byColor)
                return true;
        }
    }

    for (auto& dir : kBishopDirs) {
        int nx = x + dir[0], ny = y + dir[1];
        while (inBounds(nx, ny)) {
            const Piece& p = squares[nx][ny];
            if (!p.isEmpty()) {
                if (p.color == byColor && (p.type == PieceType::Bishop || p.type == PieceType::Queen))
                    return true;
                break;
            }
            nx += dir[0];
            ny += dir[1];
        }
    }

    for (auto& dir : kRookDirs) {
        int nx = x + dir[0], ny = y + dir[1];
        while (inBounds(nx, ny)) {
            const Piece& p = squares[nx][ny];
            if (!p.isEmpty()) {
                if (p.color == byColor && (p.type == PieceType::Rook || p.type == PieceType::Queen))
                    return true;
                break;
            }
            nx += dir[0];
            ny += dir[1];
        }
    }

    return false;
}

bool Board::isInCheck(Color color) const {
    // kingX/kingY are already tracked incrementally by makeMove() (for the
    // king's PST term -- see kingTerm()), so the king's square is known for
    // free; no need to rescan all 64 squares to find it.
    int ci = colorIndex(color);
    return isSquareAttacked(kingX[ci], kingY[ci], opponent(color));
}

MoveList Board::legalMoves(Color color) const { return filterLegal(pseudoLegalMoves(color, false), color); }

MoveList Board::legalNoisyMoves(Color color) const { return filterLegal(pseudoLegalMoves(color, true), color); }

MoveList Board::filterLegal(const MoveList& pseudo, Color color) const {
    // Logically const (this queries, never mutates, the position from the
    // caller's perspective), but avoiding a per-candidate Board copy means
    // using make/unmake in place instead: each candidate is applied,
    // checked, and reversed before the next one, so *this is bit-for-bit
    // unchanged by the time this returns. const_cast is sound here only
    // because no Board in this codebase is ever declared with genuine const
    // storage (every "const Board" in the codebase is a const *reference*
    // to a non-const object) -- transiently mutating an object that was
    // actually declared const would be undefined behavior even though the
    // value is restored before anyone observes it.
    //
    // Uses the LIGHTWEIGHT make/unmake (piece placement + king tracking
    // only), not the full makeMove/unmakeMove: this loop never reads
    // hashKey()/rawEval()/castling rights/en-passant target for any
    // candidate, only isInCheck() (which itself only reads squares[][] and
    // kingX/kingY), so there's nothing to gain from updating the rest --
    // only cost.
    Board& self = const_cast<Board&>(*this);
    MoveList result;
    for (const Move& m : pseudo) {
        LightUndo undo;
        self.makeMoveLight(m, undo);
        // isInCheck(color): check the side that just moved (the candidate
        // mover), not the side to move next. Getting this backwards would
        // silently pass most positions (moving into check is rare) but
        // wrongly accept illegal moves in pinned-piece/discovered-check
        // positions specifically -- see the perft("pin") case added to
        // verify this explicitly rather than assuming it.
        if (!self.isInCheck(color)) result.push_back(m);
        self.unmakeMoveLight(m, undo);
    }
    return result;
}

void Board::makeMove(const Move& m) {
    const ZobristKeys& z = zobrist();
    auto togglePiece = [&](Color c, PieceType t, int x, int y) {
        uint64_t key = z.piece[colorIndex(c)][pieceTypeIndex(t)][x][y];
        hash ^= key;
        if (t == PieceType::Pawn) pawnHash ^= key;
    };
    // Kings are excluded from nonKingScore (see kingTerm()); non-king pieces
    // are summed incrementally at exactly the points their placement changes.
    auto addEval = [&](Color c, PieceType t, int x, int y) {
        if (t != PieceType::King) nonKingScore += pieceContribution(t, c, x, y);
        if (t == PieceType::Pawn) pawnEgShift += pawnEgShiftFor(c, x, y);
    };
    auto removeEval = [&](Color c, PieceType t, int x, int y) {
        if (t != PieceType::King) nonKingScore -= pieceContribution(t, c, x, y);
        if (t == PieceType::Pawn) pawnEgShift -= pawnEgShiftFor(c, x, y);
    };

    Piece moving = squares[m.fromX][m.fromY];
    Color color = moving.color;
    Piece captured = squares[m.toX][m.toY];
    halfmoveCount = (moving.type == PieceType::Pawn || !captured.isEmpty()) ? 0 : halfmoveCount + 1;

    togglePiece(moving.color, moving.type, m.fromX, m.fromY);
    removeEval(moving.color, moving.type, m.fromX, m.fromY);

    if (m.isEnPassant) {
        Piece capturedPawn = squares[m.toX][m.fromY];
        togglePiece(capturedPawn.color, capturedPawn.type, m.toX, m.fromY);
        removeEval(capturedPawn.color, capturedPawn.type, m.toX, m.fromY);
        phaseUnits -= phaseValue(capturedPawn.type);
        squares[m.toX][m.fromY] = Piece{};
    } else if (!captured.isEmpty()) {
        togglePiece(captured.color, captured.type, m.toX, m.toY);
        removeEval(captured.color, captured.type, m.toX, m.toY);
        phaseUnits -= phaseValue(captured.type);
        if (captured.type == PieceType::Bishop) bishopCount[colorIndex(captured.color)]--;
    }
    if (m.isCastle) {
        int homeY = m.fromY;
        if (m.toX == 6) {
            togglePiece(color, PieceType::Rook, 7, homeY);
            togglePiece(color, PieceType::Rook, 5, homeY);
            removeEval(color, PieceType::Rook, 7, homeY);
            addEval(color, PieceType::Rook, 5, homeY);
            squares[5][homeY] = squares[7][homeY];
            squares[7][homeY] = Piece{};
        } else {
            togglePiece(color, PieceType::Rook, 0, homeY);
            togglePiece(color, PieceType::Rook, 3, homeY);
            removeEval(color, PieceType::Rook, 0, homeY);
            addEval(color, PieceType::Rook, 3, homeY);
            squares[3][homeY] = squares[0][homeY];
            squares[0][homeY] = Piece{};
        }
    }

    squares[m.toX][m.toY] = moving;
    squares[m.fromX][m.fromY] = Piece{};
    if (m.promotion != PieceType::None) squares[m.toX][m.toY].type = m.promotion;
    togglePiece(color, squares[m.toX][m.toY].type, m.toX, m.toY);
    addEval(color, squares[m.toX][m.toY].type, m.toX, m.toY);
    if (m.promotion != PieceType::None) phaseUnits += phaseValue(m.promotion);
    if (m.promotion == PieceType::Bishop) bishopCount[colorIndex(color)]++;

    if (moving.type == PieceType::King) {
        kingX[colorIndex(color)] = m.toX;
        kingY[colorIndex(color)] = m.toY;
        int ci = colorIndex(color);
        if (castleKingside[ci]) { hash ^= z.castle[ci][0]; castleKingside[ci] = false; }
        if (castleQueenside[ci]) { hash ^= z.castle[ci][1]; castleQueenside[ci] = false; }
    }
    if (moving.type == PieceType::Rook) {
        int homeY = (color == Color::White) ? 0 : 7;
        int ci = colorIndex(color);
        if (m.fromY == homeY && m.fromX == 0 && castleQueenside[ci]) { hash ^= z.castle[ci][1]; castleQueenside[ci] = false; }
        if (m.fromY == homeY && m.fromX == 7 && castleKingside[ci]) { hash ^= z.castle[ci][0]; castleKingside[ci] = false; }
    }
    if (captured.type == PieceType::Rook) {
        if ((m.toX == 0 || m.toX == 7) && (m.toY == 0 || m.toY == 7)) {
            Color rookColor = (m.toY == 0) ? Color::White : Color::Black;
            int ci = colorIndex(rookColor);
            if (m.toX == 0 && castleQueenside[ci]) { hash ^= z.castle[ci][1]; castleQueenside[ci] = false; }
            else if (m.toX == 7 && castleKingside[ci]) { hash ^= z.castle[ci][0]; castleKingside[ci] = false; }
        }
    }

    if (enPassantX != -1) hash ^= z.enPassantFile[enPassantX];
    enPassantX = enPassantY = -1;
    if (moving.type == PieceType::Pawn && std::abs(m.toY - m.fromY) == 2) {
        enPassantX = m.fromX;
        enPassantY = (m.fromY + m.toY) / 2;
        hash ^= z.enPassantFile[enPassantX];
    }

    hash ^= z.sideToMove;
    turn = opponent(color);
}

void Board::makeMove(const Move& m, UndoState& undo) {
    undo.prevHash = hash;
    undo.prevPawnHash = pawnHash;
    undo.prevCastleKingside[0] = castleKingside[0];
    undo.prevCastleKingside[1] = castleKingside[1];
    undo.prevCastleQueenside[0] = castleQueenside[0];
    undo.prevCastleQueenside[1] = castleQueenside[1];
    undo.prevEnPassantX = enPassantX;
    undo.prevEnPassantY = enPassantY;
    undo.prevNonKingScore = nonKingScore;
    undo.prevPhaseUnits = phaseUnits;
    undo.prevKingX[0] = kingX[0];
    undo.prevKingX[1] = kingX[1];
    undo.prevKingY[0] = kingY[0];
    undo.prevKingY[1] = kingY[1];
    undo.prevBishopCount[0] = bishopCount[0];
    undo.prevBishopCount[1] = bishopCount[1];
    undo.prevPawnEgShift = pawnEgShift;
    undo.prevHalfmoveCount = halfmoveCount;
    const Piece& moving = squares[m.fromX][m.fromY];
    undo.movedType = moving.type;
    undo.movedColor = moving.color;

    makeMove(m);
}

void Board::unmakeMove(const Move& m, const UndoState& undo) {
    Color color = undo.movedColor;
    Color enemy = opponent(color);

    if (m.isEnPassant) {
        squares[m.toX][m.toY] = Piece{};
        squares[m.toX][m.fromY] = Piece{PieceType::Pawn, enemy};
    } else if (m.captured != PieceType::None) {
        squares[m.toX][m.toY] = Piece{m.captured, enemy};
    } else {
        squares[m.toX][m.toY] = Piece{};
    }

    squares[m.fromX][m.fromY] = Piece{undo.movedType, color};

    if (m.isCastle) {
        int homeY = m.fromY;
        if (m.toX == 6) {
            squares[7][homeY] = Piece{PieceType::Rook, color};
            squares[5][homeY] = Piece{};
        } else {
            squares[0][homeY] = Piece{PieceType::Rook, color};
            squares[3][homeY] = Piece{};
        }
    }

    castleKingside[0] = undo.prevCastleKingside[0];
    castleKingside[1] = undo.prevCastleKingside[1];
    castleQueenside[0] = undo.prevCastleQueenside[0];
    castleQueenside[1] = undo.prevCastleQueenside[1];
    enPassantX = undo.prevEnPassantX;
    enPassantY = undo.prevEnPassantY;
    hash = undo.prevHash;
    pawnHash = undo.prevPawnHash;
    nonKingScore = undo.prevNonKingScore;
    phaseUnits = undo.prevPhaseUnits;
    kingX[0] = undo.prevKingX[0];
    kingX[1] = undo.prevKingX[1];
    kingY[0] = undo.prevKingY[0];
    kingY[1] = undo.prevKingY[1];
    bishopCount[0] = undo.prevBishopCount[0];
    bishopCount[1] = undo.prevBishopCount[1];
    pawnEgShift = undo.prevPawnEgShift;
    halfmoveCount = undo.prevHalfmoveCount;
    turn = color;
}

void Board::makeNullMove(NullMoveUndo& undo) {
    const ZobristKeys& z = zobrist();
    undo.prevHash = hash;
    undo.prevEnPassantX = enPassantX;
    undo.prevEnPassantY = enPassantY;
    if (enPassantX != -1) hash ^= z.enPassantFile[enPassantX];
    enPassantX = enPassantY = -1;
    hash ^= z.sideToMove;
    turn = opponent(turn);
}

void Board::unmakeNullMove(const NullMoveUndo& undo) {
    hash = undo.prevHash;
    enPassantX = undo.prevEnPassantX;
    enPassantY = undo.prevEnPassantY;
    turn = opponent(turn);
}

bool Board::hasNonPawnMaterial(Color color) const {
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            const Piece& p = squares[x][y];
            if (p.color != color) continue;
            if (p.type != PieceType::Pawn && p.type != PieceType::King) return true;
        }
    }
    return false;
}

// Piece-placement and king-tracking only -- no hash, no eval, no castling
// rights, no en-passant target. See the LightUndo comment in board.hpp for
// the invariant this depends on.
void Board::makeMoveLight(const Move& m, LightUndo& undo) {
    Piece moving = squares[m.fromX][m.fromY];
    Color color = moving.color;
    undo.movedType = moving.type;
    undo.movedColor = color;

    if (m.isEnPassant) {
        // The captured pawn is NOT on the destination square -- it's on the
        // same file as the destination but the same rank the capturing
        // pawn started on.
        squares[m.toX][m.fromY] = Piece{};
    } else if (m.isCastle) {
        int homeY = m.fromY;
        if (m.toX == 6) {
            squares[5][homeY] = squares[7][homeY];
            squares[7][homeY] = Piece{};
        } else {
            squares[3][homeY] = squares[0][homeY];
            squares[0][homeY] = Piece{};
        }
    }
    // Ordinary captures need no extra step: squares[m.toX][m.toY] below
    // simply overwrites whatever was there.

    squares[m.toX][m.toY] = moving;
    squares[m.fromX][m.fromY] = Piece{};
    if (m.promotion != PieceType::None) squares[m.toX][m.toY].type = m.promotion;

    if (moving.type == PieceType::King) {
        int ci = colorIndex(color);
        kingX[ci] = m.toX;
        kingY[ci] = m.toY;
    }
}

void Board::unmakeMoveLight(const Move& m, const LightUndo& undo) {
    Color color = undo.movedColor;
    Color enemy = opponent(color);

    if (m.isEnPassant) {
        squares[m.toX][m.toY] = Piece{};
        squares[m.toX][m.fromY] = Piece{PieceType::Pawn, enemy};
    } else if (m.captured != PieceType::None) {
        squares[m.toX][m.toY] = Piece{m.captured, enemy};
    } else {
        squares[m.toX][m.toY] = Piece{};
    }

    squares[m.fromX][m.fromY] = Piece{undo.movedType, color};

    if (m.isCastle) {
        int homeY = m.fromY;
        if (m.toX == 6) {
            squares[7][homeY] = Piece{PieceType::Rook, color};
            squares[5][homeY] = Piece{};
        } else {
            squares[0][homeY] = Piece{PieceType::Rook, color};
            squares[3][homeY] = Piece{};
        }
    }

    // Both a plain king move and a castling move set fromX/fromY to the
    // king's original square, so this unconditionally restores the right
    // square either way -- no need to have stashed it in LightUndo.
    if (undo.movedType == PieceType::King) {
        int ci = colorIndex(color);
        kingX[ci] = m.fromX;
        kingY[ci] = m.fromY;
    }
}

GameStatus Board::status() const {
    bool check = isInCheck(turn);
    bool hasMoves = !legalMoves(turn).empty();
    if (!hasMoves) return check ? GameStatus::Checkmate : GameStatus::Stalemate;
    return check ? GameStatus::Check : GameStatus::Ongoing;
}

bool Board::findLegalMove(int fromX, int fromY, int toX, int toY, PieceType promotion, Move& out) const {
    Move underpromotionMatch{};
    bool foundUnderpromotion = false;
    for (const Move& m : legalMoves(turn)) {
        if (m.fromX != fromX || m.fromY != fromY || m.toX != toX || m.toY != toY) continue;
        if (m.promotion == promotion) {
            out = m;
            return true;
        }
        if (promotion == PieceType::None && m.promotion == PieceType::Queen) {
            underpromotionMatch = m;
            foundUnderpromotion = true;
        }
    }
    if (foundUnderpromotion) {
        out = underpromotionMatch;
        return true;
    }
    return false;
}

void Board::print(std::ostream& os) const {
    for (int y = 7; y >= 0; y--) {
        os << (y + 1) << " ";
        for (int x = 0; x < 8; x++) {
            os << squares[x][y].symbol() << " ";
        }
        os << "\n";
    }
    os << "  a b c d e f g h\n";
}
