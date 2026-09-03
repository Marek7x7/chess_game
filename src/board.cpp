#include "board.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ostream>
#include <random>

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
constexpr int kPawnTable[8][8] = {
    { 0,  0,   0,   0,   0,   0,  0,  0},
    {50, 50,  50,  50,  50,  50, 50, 50},
    {10, 10,  20,  30,  30,  20, 10, 10},
    { 5,  5,  10,  25,  25,  10,  5,  5},
    { 0,  0,   0,  20,  20,   0,  0,  0},
    { 5, -5, -10,   0,   0, -10, -5,  5},
    { 5, 10,  10, -20, -20,  10, 10,  5},
    { 0,  0,   0,   0,   0,   0,  0,  0},
};
constexpr int kKnightTable[8][8] = {
    {-50, -40, -30, -30, -30, -30, -40, -50},
    {-40, -20,   0,   0,   0,   0, -20, -40},
    {-30,   0,  10,  15,  15,  10,   0, -30},
    {-30,   5,  15,  20,  20,  15,   5, -30},
    {-30,   0,  15,  20,  20,  15,   0, -30},
    {-30,   5,  10,  15,  15,  10,   5, -30},
    {-40, -20,   0,   5,   5,   0, -20, -40},
    {-50, -40, -30, -30, -30, -30, -40, -50},
};
constexpr int kBishopTable[8][8] = {
    {-20, -10, -10, -10, -10, -10, -10, -20},
    {-10,   0,   0,   0,   0,   0,   0, -10},
    {-10,   0,   5,  10,  10,   5,   0, -10},
    {-10,   5,   5,  10,  10,   5,   5, -10},
    {-10,   0,  10,  10,  10,  10,   0, -10},
    {-10,  10,  10,  10,  10,  10,  10, -10},
    {-10,   5,   0,   0,   0,   0,   5, -10},
    {-20, -10, -10, -10, -10, -10, -10, -20},
};
constexpr int kRookTable[8][8] = {
    { 0,  0,  0,  0,  0,  0,  0,  0},
    { 5, 10, 10, 10, 10, 10, 10,  5},
    {-5,  0,  0,  0,  0,  0,  0, -5},
    {-5,  0,  0,  0,  0,  0,  0, -5},
    {-5,  0,  0,  0,  0,  0,  0, -5},
    {-5,  0,  0,  0,  0,  0,  0, -5},
    {-5,  0,  0,  0,  0,  0,  0, -5},
    { 0,  0,  0,  5,  5,  0,  0,  0},
};
constexpr int kQueenTable[8][8] = {
    {-20, -10, -10, -5, -5, -10, -10, -20},
    {-10,   0,   0,  0,  0,   0,   0, -10},
    {-10,   0,   5,  5,  5,   5,   0, -10},
    { -5,   0,   5,  5,  5,   5,   0,  -5},
    {  0,   0,   5,  5,  5,   5,   0,  -5},
    {-10,   5,   5,  5,  5,   5,   0, -10},
    {-10,   0,   5,  0,  0,   0,   0, -10},
    {-20, -10, -10, -5, -5, -10, -10, -20},
};
constexpr int kKingMidTable[8][8] = {
    {-30, -40, -40, -50, -50, -40, -40, -30},
    {-30, -40, -40, -50, -50, -40, -40, -30},
    {-30, -40, -40, -50, -50, -40, -40, -30},
    {-30, -40, -40, -50, -50, -40, -40, -30},
    {-20, -30, -30, -40, -40, -30, -30, -20},
    {-10, -20, -20, -20, -20, -20, -20, -10},
    {20,   20,   0,   0,   0,   0,  20,  20},
    {20,   30,  10,   0,   0,  10,  30,  20},
};
constexpr int kKingEndTable[8][8] = {
    {-50, -40, -30, -20, -20, -30, -40, -50},
    {-30, -20, -10,   0,   0, -10, -20, -30},
    {-30, -10,  20,  30,  30,  20, -10, -30},
    {-30, -10,  30,  40,  40,  30, -10, -30},
    {-30, -10,  30,  40,  40,  30, -10, -30},
    {-30, -10,  20,  30,  30,  20, -10, -30},
    {-30, -30,   0,   0,   0,   0, -30, -30},
    {-50, -30, -30, -30, -30, -30, -30, -50},
};

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
        }
    }
}

int Board::kingTerm() const {
    double phase = gamePhase();
    int white = kingPst(Color::White, kingX[0], kingY[0], phase);
    int black = kingPst(Color::Black, kingX[1], kingY[1], phase);
    return white / 2 - black / 2;
}

int Board::rawEval() const { return nonKingScore + kingTerm(); }

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
        }
    }
    return score;
}

void Board::recomputeHash() {
    const ZobristKeys& z = zobrist();
    hash = 0;
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            const Piece& p = squares[x][y];
            if (p.isEmpty()) continue;
            hash ^= z.piece[colorIndex(p.color)][pieceTypeIndex(p.type)][x][y];
        }
    }
    if (castleKingside[0]) hash ^= z.castle[0][0];
    if (castleQueenside[0]) hash ^= z.castle[0][1];
    if (castleKingside[1]) hash ^= z.castle[1][0];
    if (castleQueenside[1]) hash ^= z.castle[1][1];
    if (enPassantX != -1) hash ^= z.enPassantFile[enPassantX];
    if (turn == Color::Black) hash ^= z.sideToMove;
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

    board.recomputeHash();
    board.recomputeEval();
    return board;
}

void Board::addSlidingMoves(MoveList& moves, int x, int y, Color color,
                             const int dirs[][2], int numDirs) const {
    for (int d = 0; d < numDirs; d++) {
        int nx = x + dirs[d][0];
        int ny = y + dirs[d][1];
        while (inBounds(nx, ny)) {
            const Piece& target = squares[nx][ny];
            if (target.isEmpty()) {
                moves.push_back({x, y, nx, ny});
            } else {
                if (target.color != color) moves.push_back({x, y, nx, ny, PieceType::None, false, false, target.type});
                break;
            }
            nx += dirs[d][0];
            ny += dirs[d][1];
        }
    }
}

void Board::addPawnMoves(MoveList& moves, int x, int y, Color color) const {
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
        addForwardOrPromotion(x, ny, PieceType::None);
        int ny2 = y + 2 * dir;
        if (y == startRank && squares[x][ny2].isEmpty()) {
            moves.push_back({x, y, x, ny2});
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

void Board::addKingMoves(MoveList& moves, int x, int y, Color color) const {
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            if (dx == 0 && dy == 0) continue;
            int nx = x + dx, ny = y + dy;
            if (!inBounds(nx, ny)) continue;
            const Piece& target = squares[nx][ny];
            if (target.isEmpty() || target.color != color)
                moves.push_back({x, y, nx, ny, PieceType::None, false, false, target.type});
        }
    }

    if (isInCheck(color)) return;
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

MoveList Board::pseudoLegalMoves(Color color) const {
    MoveList moves;
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            const Piece& p = squares[x][y];
            if (p.color != color) continue;
            switch (p.type) {
                case PieceType::Pawn:
                    addPawnMoves(moves, x, y, color);
                    break;
                case PieceType::Knight:
                    for (auto& off : kKnightOffsets) {
                        int nx = x + off[0], ny = y + off[1];
                        if (!inBounds(nx, ny)) continue;
                        const Piece& target = squares[nx][ny];
                        if (target.isEmpty() || target.color != color)
                            moves.push_back({x, y, nx, ny, PieceType::None, false, false, target.type});
                    }
                    break;
                case PieceType::Bishop:
                    addSlidingMoves(moves, x, y, color, kBishopDirs, 4);
                    break;
                case PieceType::Rook:
                    addSlidingMoves(moves, x, y, color, kRookDirs, 4);
                    break;
                case PieceType::Queen:
                    addSlidingMoves(moves, x, y, color, kQueenDirs, 8);
                    break;
                case PieceType::King:
                    addKingMoves(moves, x, y, color);
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
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            if (squares[x][y].type == PieceType::King && squares[x][y].color == color)
                return isSquareAttacked(x, y, opponent(color));
        }
    }
    return false; // no king on board (shouldn't happen)
}

MoveList Board::legalMoves(Color color) const {
    MoveList result;
    for (const Move& m : pseudoLegalMoves(color)) {
        Board copy = *this;
        copy.makeMove(m);
        if (!copy.isInCheck(color)) result.push_back(m);
    }
    return result;
}

void Board::makeMove(const Move& m) {
    const ZobristKeys& z = zobrist();
    auto togglePiece = [&](Color c, PieceType t, int x, int y) {
        hash ^= z.piece[colorIndex(c)][pieceTypeIndex(t)][x][y];
    };
    // Kings are excluded from nonKingScore (see kingTerm()); non-king pieces
    // are summed incrementally at exactly the points their placement changes.
    auto addEval = [&](Color c, PieceType t, int x, int y) {
        if (t != PieceType::King) nonKingScore += pieceContribution(t, c, x, y);
    };
    auto removeEval = [&](Color c, PieceType t, int x, int y) {
        if (t != PieceType::King) nonKingScore -= pieceContribution(t, c, x, y);
    };

    Piece moving = squares[m.fromX][m.fromY];
    Color color = moving.color;
    Piece captured = squares[m.toX][m.toY];

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
    nonKingScore = undo.prevNonKingScore;
    phaseUnits = undo.prevPhaseUnits;
    kingX[0] = undo.prevKingX[0];
    kingX[1] = undo.prevKingX[1];
    kingY[0] = undo.prevKingY[0];
    kingY[1] = undo.prevKingY[1];
    turn = color;
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
