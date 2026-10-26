#include "syzygy.hpp"

#include <atomic>

#include "bitboard.hpp"

#ifdef CHESS_SYZYGY
extern "C" {
#include "tbprobe.h"
}
#endif

namespace syzygy {

namespace {
std::atomic<long long> g_hits{0};
int g_probeDepth = 1;
bool g_searchUse = true;
} // namespace

int probeDepth() { return g_probeDepth; }
void setProbeDepth(int depth) { g_probeDepth = depth; }
void setSearchUse(bool on) { g_searchUse = on; }
long long hits() { return g_hits.load(std::memory_order_relaxed); }
void resetHits() { g_hits.store(0, std::memory_order_relaxed); }

#ifdef CHESS_SYZYGY

namespace {

// Board numbers its squares file * 8 + rank, Fathom rank * 8 + file: a transposition of the 8x8 bit matrix.
uint64_t transpose(uint64_t x) {
    constexpr uint64_t k1 = 0x5500550055005500ULL, k2 = 0x3333000033330000ULL, k4 = 0x0f0f0f0f00000000ULL;
    uint64_t t = k4 & (x ^ (x << 28));
    x ^= t ^ (t >> 28);
    t = k2 & (x ^ (x << 14));
    x ^= t ^ (t >> 14);
    t = k1 & (x ^ (x << 7));
    x ^= t ^ (t >> 7);
    return x;
}

struct Position {
    uint64_t white, black, kings, queens, rooks, bishops, knights, pawns;
    unsigned ep;
    bool whiteToMove;
};

Position convert(const Board& b) {
    auto both = [&](PieceType t) { return transpose(b.pieces(Color::White, t) | b.pieces(Color::Black, t)); };
    Position p;
    p.white = transpose(b.occupancy(Color::White));
    p.black = transpose(b.occupancy(Color::Black));
    p.kings = both(PieceType::King);
    p.queens = both(PieceType::Queen);
    p.rooks = both(PieceType::Rook);
    p.bishops = both(PieceType::Bishop);
    p.knights = both(PieceType::Knight);
    p.pawns = both(PieceType::Pawn);
    p.ep = b.enPassantFile() >= 0 ? static_cast<unsigned>(b.enPassantRank() * 8 + b.enPassantFile()) : 0;
    p.whiteToMove = b.sideToMove() == Color::White;
    return p;
}

Wdl toWdl(unsigned v) {
    switch (v) {
        case TB_LOSS: return Wdl::Loss;
        case TB_BLESSED_LOSS: return Wdl::BlessedLoss;
        case TB_DRAW: return Wdl::Draw;
        case TB_CURSED_WIN: return Wdl::CursedWin;
        default: return Wdl::Win;
    }
}

} // namespace

bool init(const std::string& path, std::string& message) {
    tb_free();
    if (path.empty() || path == "<empty>") {
        message = "tablebases unloaded";
        return true;
    }
    if (!tb_init(path.c_str())) {
        message = "could not initialize the tablebases in " + path;
        return false;
    }
    message = TB_LARGEST ? std::to_string(TB_LARGEST) + "-piece tablebases loaded from " + path
                         : "no tablebase files found in " + path;
    return true;
}

int largest() { return g_searchUse ? static_cast<int>(TB_LARGEST) : 0; }
int tableSize() { return static_cast<int>(TB_LARGEST); }

bool probeLabel(const Board& board, Wdl& wdl) {
    if (board.hasCastlingRights() || bb::popcount(board.occupancy()) > static_cast<int>(TB_LARGEST)) return false;
    const Position p = convert(board);
    const unsigned r = tb_probe_wdl(p.white, p.black, p.kings, p.queens, p.rooks, p.bishops, p.knights, p.pawns, 0, 0,
                                    p.ep, p.whiteToMove);
    if (r == TB_RESULT_FAILED) return false;
    wdl = toWdl(r);
    if (wdl == Wdl::CursedWin || wdl == Wdl::BlessedLoss) wdl = Wdl::Draw;
    return true;
}

bool probeWdl(const Board& board, Wdl& wdl) {
    if (board.hasCastlingRights() || board.halfmoveClock() != 0) return false;
    if (bb::popcount(board.occupancy()) > static_cast<int>(TB_LARGEST)) return false;
    const Position p = convert(board);
    const unsigned r = tb_probe_wdl(p.white, p.black, p.kings, p.queens, p.rooks, p.bishops, p.knights, p.pawns, 0, 0,
                                    p.ep, p.whiteToMove);
    if (r == TB_RESULT_FAILED) return false;
    g_hits.fetch_add(1, std::memory_order_relaxed);
    wdl = toWdl(r);
    return true;
}

bool probeRoot(const Board& board, Move& best, Wdl& wdl) {
    if (board.hasCastlingRights() || bb::popcount(board.occupancy()) > static_cast<int>(TB_LARGEST)) return false;
    const Position p = convert(board);
    const unsigned r = tb_probe_root(p.white, p.black, p.kings, p.queens, p.rooks, p.bishops, p.knights, p.pawns,
                                     static_cast<unsigned>(board.halfmoveClock()), 0, p.ep, p.whiteToMove, nullptr);
    if (r == TB_RESULT_FAILED || r == TB_RESULT_CHECKMATE || r == TB_RESULT_STALEMATE) return false;
    const int from = static_cast<int>(TB_GET_FROM(r)), to = static_cast<int>(TB_GET_TO(r));
    PieceType promotion = PieceType::None;
    switch (TB_GET_PROMOTES(r)) {
        case TB_PROMOTES_QUEEN: promotion = PieceType::Queen; break;
        case TB_PROMOTES_ROOK: promotion = PieceType::Rook; break;
        case TB_PROMOTES_BISHOP: promotion = PieceType::Bishop; break;
        case TB_PROMOTES_KNIGHT: promotion = PieceType::Knight; break;
        default: break;
    }
    for (const Move& m : board.legalMoves(board.sideToMove())) {
        if (m.fromX == (from & 7) && m.fromY == (from >> 3) && m.toX == (to & 7) && m.toY == (to >> 3) &&
            m.promotion == promotion) {
            best = m;
            wdl = toWdl(TB_GET_WDL(r));
            g_hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false; // Fathom's move is not legal here: an inconsistent probe, so do not trust it
}

#else // built without CHESS_SYZYGY

bool init(const std::string&, std::string& message) {
    message = "this build has no tablebase support";
    return false;
}
int largest() { return 0; }
int tableSize() { return 0; }
bool probeLabel(const Board&, Wdl&) { return false; }
bool probeWdl(const Board&, Wdl&) { return false; }
bool probeRoot(const Board&, Move&, Wdl&) { return false; }

#endif

} // namespace syzygy
