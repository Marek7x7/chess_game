#include "board.hpp"

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

    const ZobristKeys& z = zobrist();
    hash = 0;
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            const Piece& p = squares[x][y];
            if (p.isEmpty()) continue;
            hash ^= z.piece[colorIndex(p.color)][pieceTypeIndex(p.type)][x][y];
        }
    }
    hash ^= z.castle[0][0] ^ z.castle[0][1] ^ z.castle[1][0] ^ z.castle[1][1];
}

void Board::addSlidingMoves(std::vector<Move>& moves, int x, int y, Color color,
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

void Board::addPawnMoves(std::vector<Move>& moves, int x, int y, Color color) const {
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

void Board::addKingMoves(std::vector<Move>& moves, int x, int y, Color color) const {
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

std::vector<Move> Board::pseudoLegalMoves(Color color) const {
    std::vector<Move> moves;
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

std::vector<Move> Board::legalMoves(Color color) const {
    std::vector<Move> result;
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

    Piece moving = squares[m.fromX][m.fromY];
    Color color = moving.color;
    Piece captured = squares[m.toX][m.toY];

    togglePiece(moving.color, moving.type, m.fromX, m.fromY);

    if (m.isEnPassant) {
        Piece capturedPawn = squares[m.toX][m.fromY];
        togglePiece(capturedPawn.color, capturedPawn.type, m.toX, m.fromY);
        squares[m.toX][m.fromY] = Piece{};
    } else if (!captured.isEmpty()) {
        togglePiece(captured.color, captured.type, m.toX, m.toY);
    }
    if (m.isCastle) {
        int homeY = m.fromY;
        if (m.toX == 6) {
            togglePiece(color, PieceType::Rook, 7, homeY);
            togglePiece(color, PieceType::Rook, 5, homeY);
            squares[5][homeY] = squares[7][homeY];
            squares[7][homeY] = Piece{};
        } else {
            togglePiece(color, PieceType::Rook, 0, homeY);
            togglePiece(color, PieceType::Rook, 3, homeY);
            squares[3][homeY] = squares[0][homeY];
            squares[0][homeY] = Piece{};
        }
    }

    squares[m.toX][m.toY] = moving;
    squares[m.fromX][m.fromY] = Piece{};
    if (m.promotion != PieceType::None) squares[m.toX][m.toY].type = m.promotion;
    togglePiece(color, squares[m.toX][m.toY].type, m.toX, m.toY);

    if (moving.type == PieceType::King) {
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
