#include "board.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ostream>
#include <random>

#include "bitboard.hpp"
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
    {132, 108, 180, 119,  90,  52, -55, -60},
    { -6,   2,  -8, -15,  10,  38,  32, -23},
    {-51, -66, -64, -52, -20, -13, -53, -71},
    {-72, -82, -70, -62, -56, -44, -60, -92},
    {-89, -99, -96, -94, -85, -64, -53, -87},
    {-78, -88, -85, -95, -80, -22, -18, -82},
    {  0,   0,   0,   0,   0,   0,   0,   0},
});
TUNE_ARR2(kKnightTable, 8, 8, {
    {-184, -64, -23, -14,  10, -65,  16, -124},
    {-102, -30,  30,  43,  22,  57, -34, -54},
    {-32, -32,  62,  62, 108,  84,  52,   4},
    { -1,   3,  22,  76,  24,  67,  27,  52},
    {-14,   0,  30,  24,  51,  46,  61,   4},
    {-36, -12,   0,  22,  54,   6,  16, -27},
    {-80, -35,  -8,  13,  23,  10,  -5,  -2},
    {-167, -38, -26, -16,  -6,  -8, -39, -64},
});
TUNE_ARR2(kBishopTable, 8, 8, {
    {-34, -20, -16, -51, -24, -72,  51,  35},
    {-49, -23, -16, -59, -14, -26, -72, -68},
    {-18,  -2, -37,  -7,  44, 106,  58,  61},
    {-22, -19,  -7,  34,  29,  20, -13, -26},
    {-25,  -5,   8,  35,  37,   5,  10,  26},
    {  2,  36,  27,  27,  30,  34,  45,  22},
    { 33,  29,  32,   8,  23,  42,  58,  28},
    {  0,  57,   1, -20,  18,  -8,  63,  -6},
});
TUNE_ARR2(kRookTable, 8, 8, {
    { 21,  21,  63,  40,  28, 104,  72,  96},
    {-48, -41,  -2,  52,  14, 120,  54,  94},
    {-16, -16,   9,  24,  92,  98,  73,  48},
    {-50, -34, -26,  18, -10,   1,  22,  20},
    {-66, -78, -58, -40, -30, -45,  14, -61},
    {-61, -63, -54, -47, -46, -26,  11, -19},
    {-66, -82, -34, -23, -32, -21, -37, -80},
    {-48, -38, -22, -15,   3,  -6, -20, -20},
});
TUNE_ARR2(kQueenTable, 8, 8, {
    {-35, -26,  20,  42,  48, 143, 115, 115},
    {-84, -100, -10, -25, -10,  48,  14,  34},
    {-56, -40, -44,   1,   7, 117,  72,  34},
    {-54, -42, -62, -30,  -6, -22,  -9,   5},
    {-56, -40, -24, -32, -16, -23,  -6, -11},
    {-46, -42, -12, -30, -10,  -9,   0, -25},
    {-19, -18, -10,   0,   4,  12,  -8,   4},
    {-10, -25, -19,  -9,  10, -78, -99, -61},
});
TUNE_ARR2(kKingMidTable, 8, 8, {
    {-43, -22, -50, -26, -21,  -3,  18, -101},
    {  2,  -5,  42, -16, -52,  38,   4, -82},
    {-38,   0, -25, -105, -69,   4,   3, -13},
    {-12,  25, -91, -81, -83, -16,  19, -58},
    {-54,  61,   7, -87, -41,  18, -43, -127},
    {-20,  24,  23, -10, -32, -31,  -8, -50},
    { 45, -25,  30, -43, -27,  -7,  40,  11},
    {-41,  28,  12, -127,  -2, -81,  53, -20},
});
TUNE_ARR2(kKingEndTable, 8, 8, {
    {-95,  -6, -21, -97, -12,  34,  63, -206},
    {-26,  34,  37,  14,  34,  72,  85, -30},
    { 17,  56,  54,  46,  49,  64,  71,  10},
    {-44,   6,  38,  32,  50,  44,  44,  -8},
    {-44, -34,  -5,  26,  28,   8,   1, -10},
    {-83, -35,  -9,  -9,   5,  13,   1, -26},
    {-75, -15, -23,  17,  -5, -16, -33, -55},
    {-59, -58, -34,  -9, -54, -27, -80, -86},
});

// Endgame versions of the knight, bishop, rook and queen tables above, which are the
// middlegame ones (the king, with its own middlegame and endgame tables, and the pawns, with
// kPawnEgByRank, already taper); the evaluation blends the two by game phase.
TUNE_ARR2(kKnightEgTable, 8, 8, {
    {-94,  27,   7,  16, -10, -35, -68, -124},
    { 16,   4,  26,  14,  22,  -3, -12, -70},
    { 24,  13,  26,  22,  28,  16,  -2, -23},
    { 14,  29,  46,  32,  40,   2,  18, -14},
    { -8,  16,  34,  50,  51,  30, -17, -16},
    {-56,   6,  12,  22,  16,   4,  -2, -66},
    {-80,   1, -18, -15, -11,  12,  -6, -32},
    {-124, -96, -52, -48, -14, -32, -100, -94},
});
TUNE_ARR2(kBishopEgTable, 8, 8, {
    { 22,  34, -11,  42, -10,   0,   0, -14},
    { 11,  17,  44,  29,  44,  54,  48,  -9},
    { 34,  50,  40,  30,  14,  23,   9,  22},
    { 26,  47,  28,  25,  22,   4,  15,  20},
    { 35,  20,  34,  33,  -1,  27,   0, -24},
    { -2,  19,  21,  13,  42,   0,  -5,  14},
    {-36,  -8, -20,  24,   7, -18, -20, -60},
    {-29, -41,  -1,   0,   0,   0, -63, -19},
});
TUNE_ARR2(kRookEgTable, 8, 8, {
    { 23,  69,  30,  30,  56,  56,  48,  38},
    { 52,  64,  38,  14,  36,  12,  26,  -2},
    { 72,  84,  44,  30,   2,  24,  35,  30},
    { 50,  74,  69,  34,  40,  44,  38,  10},
    { 54,  70,  46,  37,  28,  46,  34,  21},
    { 42,  38,  27,  -7,   4,  12,  -8,   9},
    { 22,  20,  -4, -19,  -7, -35, -15, -42},
    { 12,  12,  12,  -3, -25,  -6,   5, -36},
});
TUNE_ARR2(kQueenEgTable, 8, 8, {
    { 51,  34,  20,  40,  68,  64,  66,  83},
    {  0,  10,  20,  19,  32,  58,  14,  34},
    { 21,  48,  61,  53,  96,  32,  15,  88},
    {-12,  28,  75,  48,  43,  82,  91,  79},
    {-44, -10,  16,  28,  -3,  47,  38,  14},
    {-80, -38, -14, -20, -14,  18, -18, -26},
    {-78, -58, -74, -76, -64, -96, -102, -26},
    {-102, -104, -79, -47, -92, -84, -62, -64},
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

int pstEgFor(PieceType type, Color color, int x, int y) {
    switch (type) {
        case PieceType::Knight: return pst(kKnightEgTable, color, x, y);
        case PieceType::Bishop: return pst(kBishopEgTable, color, x, y);
        case PieceType::Rook: return pst(kRookEgTable, color, x, y);
        case PieceType::Queen: return pst(kQueenEgTable, color, x, y);
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
TUNE_INT(kBishopPairMg, 14);
TUNE_INT(kBishopPairEg, 56);

// Positive = good for White.
int bishopPairTerm(const int (&bishops)[2], double phase) {
    int bonus = static_cast<int>(kBishopPairMg * phase + kBishopPairEg * (1.0 - phase) + 0.5);
    return (bishops[0] >= 2 ? bonus : 0) - (bishops[1] >= 2 ? bonus : 0);
}

// Endgame pawn values by rank counted from the pawn's own side. With few
// pieces left, what a pawn is worth is how close it is to promoting, not
// whether it controls the center, which is what kPawnTable (used, halved,
// in the middlegame) rewards.
TUNE_ARR(kPawnEgByRank, 8, {0, 0, 1, 1, 1, 5, 67, 0});

// How much a piece's endgame square value differs from its halved
// middlegame one, positive = good for White (0 for the king, which has its own
// phase-interpolated term).
int egShiftFor(PieceType type, Color color, int x, int y) {
    int shift;
    if (type == PieceType::Pawn) {
        int rank = color == Color::White ? y : 7 - y;
        shift = kPawnEgByRank[rank] - pst(kPawnTable, color, x, y) / 2;
    } else if (type == PieceType::Knight || type == PieceType::Bishop || type == PieceType::Rook ||
               type == PieceType::Queen) {
        shift = pstEgFor(type, color, x, y) / 2 - pstFor(type, color, x, y) / 2;
    } else {
        return 0;
    }
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

void Board::setSquare(int x, int y, Piece piece) {
    const uint64_t b = bb::bit(x, y);
    const Piece old = squares[x][y];
    if (!old.isEmpty()) {
        pieceBB[colorIndex(old.color)][static_cast<int>(old.type)] &= ~b;
        occBB[colorIndex(old.color)] &= ~b;
    }
    if (!piece.isEmpty()) {
        pieceBB[colorIndex(piece.color)][static_cast<int>(piece.type)] |= b;
        occBB[colorIndex(piece.color)] |= b;
    }
    squares[x][y] = piece;
}

void Board::recomputeBitboards() {
    for (auto& byColor : pieceBB)
        for (uint64_t& b : byColor) b = 0;
    occBB[0] = occBB[1] = 0;
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            const Piece& p = squares[x][y];
            if (p.isEmpty()) continue;
            pieceBB[colorIndex(p.color)][static_cast<int>(p.type)] |= bb::bit(x, y);
            occBB[colorIndex(p.color)] |= bb::bit(x, y);
        }
    }
}

bool Board::bitboardsConsistent() const {
    Board copy = *this;
    copy.recomputeBitboards();
    for (int c = 0; c < 2; c++) {
        if (copy.occBB[c] != occBB[c]) return false;
        for (int t = 0; t < 7; t++)
            if (copy.pieceBB[c][t] != pieceBB[c][t]) return false;
    }
    return true;
}

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

    recomputeBitboards();
    recomputeHash();
    recomputeEval();
}

void Board::recomputeEval() {
    nonKingScore = 0;
    phaseUnits = 0;
    bishopCount[0] = bishopCount[1] = 0;
    egShift = 0;
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
            egShift += egShiftFor(p.type, p.color, x, y);
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
           static_cast<int>(egShift * (1.0 - phase));
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
    int egShiftSum = 0;
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
            egShiftSum += egShiftFor(p.type, p.color, x, y);
        }
    }
    return score + bishopPairTerm(bishops, phaseFrac) + static_cast<int>(egShiftSum * (1.0 - phaseFrac));
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

    board.recomputeBitboards();
    board.recomputeHash();
    board.recomputeEval();
    return board;
}

void Board::addSlidingMoves(MoveList& moves, int x, int y, Color color, int firstDir, int numDirs,
                             bool noisyOnly) const {
    const int sq = x * 8 + y;
    const uint64_t occupied = occBB[0] | occBB[1];
    const uint64_t enemy = occBB[1 - colorIndex(color)];
    auto addCapture = [&](int t) {
        moves.push_back({x, y, t / 8, t % 8, PieceType::None, false, false, squares[t / 8][t % 8].type});
    };
    for (int d = firstDir; d < firstDir + numDirs; d++) {
        const uint64_t ray = bb::rays[d][sq];
        const uint64_t blockers = ray & occupied;
        const bool up = bb::rayDelta[d] > 0; // walking outward raises the square number
        int blocker = -1;
        uint64_t quiet = ray;
        if (blockers) {
            blocker = up ? bb::lsb(blockers) : bb::msb(blockers);
            quiet = ray & ~bb::rays[d][blocker] & ~(uint64_t(1) << blocker); // the empty squares before it
        }
        if (!noisyOnly) {
            while (quiet) { // nearest first, as the walk visits them
                const int t = up ? bb::lsb(quiet) : bb::msb(quiet);
                quiet &= ~(uint64_t(1) << t);
                moves.push_back({x, y, t / 8, t % 8});
            }
        }
        if (blocker >= 0 && ((enemy >> blocker) & 1)) addCapture(blocker);
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
    const uint64_t own = occBB[colorIndex(color)], enemy = occBB[1 - colorIndex(color)];
    // The pieces in square-number order (file by file, rank by rank), as the old square scan visited them.
    for (uint64_t pieces = own; pieces; pieces = bb::clearLsb(pieces)) {
        const int sq = bb::lsb(pieces);
        const int x = sq / 8, y = sq % 8;
        switch (squares[x][y].type) {
            case PieceType::Pawn:
                addPawnMoves(moves, x, y, color, noisyOnly);
                break;
            case PieceType::Knight:
                for (int i = 0; i < 8; i++) {
                    const int t = bb::knightOrdered[sq][i];
                    if (t < 0) continue;
                    if (noisyOnly ? ((enemy >> t) & 1) : !((own >> t) & 1))
                        moves.push_back({x, y, t / 8, t % 8, PieceType::None, false, false, squares[t / 8][t % 8].type});
                }
                break;
            case PieceType::Bishop:
                addSlidingMoves(moves, x, y, color, 0, 4, noisyOnly);
                break;
            case PieceType::Rook:
                addSlidingMoves(moves, x, y, color, 4, 4, noisyOnly);
                break;
            case PieceType::Queen:
                addSlidingMoves(moves, x, y, color, 0, 8, noisyOnly);
                break;
            case PieceType::King:
                addKingMoves(moves, x, y, color, noisyOnly);
                break;
            default:
                break;
        }
    }
    return moves;
}

bool Board::isSquareAttacked(int x, int y, Color byColor) const {
    const int sq = x * 8 + y;
    const int c = colorIndex(byColor);
    const uint64_t occupied = occBB[0] | occBB[1];
    // A pawn of byColor attacks (x, y) from where a pawn of the other color
    // standing on (x, y) would attack.
    if (bb::pawnAttacks[1 - c][sq] & pieceBB[c][static_cast<int>(PieceType::Pawn)]) return true;
    if (bb::knightAttacks[sq] & pieceBB[c][static_cast<int>(PieceType::Knight)]) return true;
    if (bb::kingAttacks[sq] & pieceBB[c][static_cast<int>(PieceType::King)]) return true;
    const uint64_t queens = pieceBB[c][static_cast<int>(PieceType::Queen)];
    if (bb::bishopAttacks(sq, occupied) & (pieceBB[c][static_cast<int>(PieceType::Bishop)] | queens)) return true;
    if (bb::rookAttacks(sq, occupied) & (pieceBB[c][static_cast<int>(PieceType::Rook)] | queens)) return true;
    return false;
}

uint64_t Board::pinnedPieces(Color color) const {
    const int c = colorIndex(color), e = 1 - c;
    const int ksq = kingX[c] * 8 + kingY[c];
    const uint64_t occupied = occBB[0] | occBB[1];
    const uint64_t queens = pieceBB[e][static_cast<int>(PieceType::Queen)];
    // Enemy sliders on one of the king's lines (blockers ignored); a pin is exactly one own piece
    // on the squares between king and slider.
    uint64_t pinners = ((pieceBB[e][static_cast<int>(PieceType::Rook)] | queens) & bb::rookRays[ksq]) |
                       ((pieceBB[e][static_cast<int>(PieceType::Bishop)] | queens) & bb::bishopRays[ksq]);
    uint64_t pinned = 0;
    while (pinners) {
        const int sq = bb::lsb(pinners);
        pinners = bb::clearLsb(pinners);
        const uint64_t blockers = bb::between[ksq][sq] & occupied;
        if (blockers && !bb::clearLsb(blockers) && (blockers & occBB[c])) pinned |= blockers;
    }
    return pinned;
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

    // Most pseudo-legal moves cannot be illegal. With the king not in check,
    // a move by a piece other than the king can only expose it by unblocking
    // a line -- the piece is pinned -- or, for en passant, by removing two
    // pieces from a rank at once. So those moves, the king's own moves, and
    // every move while in check get the full make-and-test; the rest pass.
    const int ci = colorIndex(color);
    const int kx = kingX[ci], ky = kingY[ci];
    const bool inCheckNow = isSquareAttacked(kx, ky, opponent(color));
    const uint64_t pinned = inCheckNow ? 0 : pinnedPieces(color); // bit x * 8 + y: an own piece pinned against the king

    MoveList result;
    for (const Move& m : pseudo) {
        const bool needsTest = inCheckNow || m.isEnPassant || (m.fromX == kx && m.fromY == ky) ||
                               ((pinned >> (m.fromX * 8 + m.fromY)) & 1);
        if (!needsTest) {
            result.push_back(m);
            continue;
        }
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
        egShift += egShiftFor(t, c, x, y);
    };
    auto removeEval = [&](Color c, PieceType t, int x, int y) {
        if (t != PieceType::King) nonKingScore -= pieceContribution(t, c, x, y);
        egShift -= egShiftFor(t, c, x, y);
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
        setSquare(m.toX, m.fromY, Piece{});
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
            setSquare(5, homeY, squares[7][homeY]);
            setSquare(7, homeY, Piece{});
        } else {
            togglePiece(color, PieceType::Rook, 0, homeY);
            togglePiece(color, PieceType::Rook, 3, homeY);
            removeEval(color, PieceType::Rook, 0, homeY);
            addEval(color, PieceType::Rook, 3, homeY);
            setSquare(3, homeY, squares[0][homeY]);
            setSquare(0, homeY, Piece{});
        }
    }

    setSquare(m.toX, m.toY, moving);
    setSquare(m.fromX, m.fromY, Piece{});
    if (m.promotion != PieceType::None) setSquare(m.toX, m.toY, Piece{m.promotion, color});
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
    undo.prevEgShift = egShift;
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
        setSquare(m.toX, m.toY, Piece{});
        setSquare(m.toX, m.fromY, Piece{PieceType::Pawn, enemy});
    } else if (m.captured != PieceType::None) {
        setSquare(m.toX, m.toY, Piece{m.captured, enemy});
    } else {
        setSquare(m.toX, m.toY, Piece{});
    }

    setSquare(m.fromX, m.fromY, Piece{undo.movedType, color});

    if (m.isCastle) {
        int homeY = m.fromY;
        if (m.toX == 6) {
            setSquare(7, homeY, Piece{PieceType::Rook, color});
            setSquare(5, homeY, Piece{});
        } else {
            setSquare(0, homeY, Piece{PieceType::Rook, color});
            setSquare(3, homeY, Piece{});
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
    egShift = undo.prevEgShift;
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
        setSquare(m.toX, m.fromY, Piece{});
    } else if (m.isCastle) {
        int homeY = m.fromY;
        if (m.toX == 6) {
            setSquare(5, homeY, squares[7][homeY]);
            setSquare(7, homeY, Piece{});
        } else {
            setSquare(3, homeY, squares[0][homeY]);
            setSquare(0, homeY, Piece{});
        }
    }
    // Ordinary captures need no extra step: squares[m.toX][m.toY] below
    // simply overwrites whatever was there.

    setSquare(m.toX, m.toY, moving);
    setSquare(m.fromX, m.fromY, Piece{});
    if (m.promotion != PieceType::None) setSquare(m.toX, m.toY, Piece{m.promotion, color});

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
        setSquare(m.toX, m.toY, Piece{});
        setSquare(m.toX, m.fromY, Piece{PieceType::Pawn, enemy});
    } else if (m.captured != PieceType::None) {
        setSquare(m.toX, m.toY, Piece{m.captured, enemy});
    } else {
        setSquare(m.toX, m.toY, Piece{});
    }

    setSquare(m.fromX, m.fromY, Piece{undo.movedType, color});

    if (m.isCastle) {
        int homeY = m.fromY;
        if (m.toX == 6) {
            setSquare(7, homeY, Piece{PieceType::Rook, color});
            setSquare(5, homeY, Piece{});
        } else {
            setSquare(0, homeY, Piece{PieceType::Rook, color});
            setSquare(3, homeY, Piece{});
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
