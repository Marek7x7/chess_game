#include "ai.hpp"

#include <algorithm>
#include <memory>
#include <array>
#include <chrono>
#include <climits>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <future>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "bitboard.hpp"
#include "nnue.hpp"
#include "opening_book.hpp"
#include "search_params.hpp"
#include "transposition_table.hpp"
#include "tune.hpp"

bool g_aiDebugLog = false;

namespace {

std::string sqStr(int x, int y) {
    std::string s;
    s += static_cast<char>('a' + x);
    s += static_cast<char>('1' + y);
    return s;
}

char pieceLetter(PieceType t) {
    switch (t) {
        case PieceType::Knight: return 'N';
        case PieceType::Bishop: return 'B';
        case PieceType::Rook: return 'R';
        case PieceType::Queen: return 'Q';
        case PieceType::King: return 'K';
        default: return '?';
    }
}

std::string moveStr(const Move& m) {
    std::string s = sqStr(m.fromX, m.fromY) + sqStr(m.toX, m.toY);
    if (m.promotion != PieceType::None) {
        s += '=';
        s += pieceLetter(m.promotion);
    }
    return s;
}

std::string movesStr(const std::vector<Move>& moves) {
    std::string s;
    for (size_t i = 0; i < moves.size(); i++) {
        if (i) s += ' ';
        s += moveStr(moves[i]);
    }
    return s;
}

// ---------------------------------------------------------------------
// Evaluation. Material + piece-square-table score is maintained
// incrementally by Board itself (see Board::rawEval()); pawn structure and
// mobility depend on where the other pieces are, so they're computed here
// from scratch.
// ---------------------------------------------------------------------

// Passed-pawn bonus by rank counted from the pawn's own side (1 = its
// starting rank, 6 = one step from promoting), middlegame and endgame. A
// passer is worth far more once pieces come off: fewer pieces are left to
// blockade it or to stop it while also doing everything else.
TUNE_ARR(kPassedMg, 8, {0, 1, 1, 1, 18, 44, 133, 0});
TUNE_ARR(kPassedEg, 8, {0, 1, 1, 23, 71, 136, 140, 0});

// Penalties per pawn, middlegame and endgame. Doubled: every pawn beyond the
// first on a file (the rear one blocks the front one and no pawn can defend
// either). Isolated: no friendly pawn on either adjacent file, so only
// pieces can ever defend it. Both weigh more in the endgame, where a weak
// pawn is a long-term target the defender runs out of pieces to protect.
TUNE_INT(kDoubledMg, 12);
TUNE_INT(kDoubledEg, 17);
TUNE_INT(kIsolatedMg, 6);
TUNE_INT(kIsolatedEg, 6);

// Backward pawn: every friendly pawn on the adjacent files has already
// advanced past it, so no pawn can ever defend it, and an enemy pawn
// controls the square in front of it, so it can't safely advance to catch
// up either (Fruit 2.1's values).
TUNE_INT(kBackwardMg, 5);
TUNE_INT(kBackwardEg, 11);

// Connected pawns, by rank counted from the pawn's own side (middlegame / endgame): a
// pawn defended by a pawn diagonally behind it, and one with a pawn beside it on the
// same rank (a phalanx, which can advance together and defend each other).
TUNE_ARR(kSupportedMg, 8, {0, 0, 14, 7, 17, 36, 197, 0});
TUNE_ARR(kSupportedEg, 8, {0, 0, 3, 1, 1, 29, 60, 0});
TUNE_ARR(kPhalanxMg, 8, {0, 1, 4, 11, 25, 42, 112, 0});
TUNE_ARR(kPhalanxEg, 8, {0, 1, 1, 3, 25, 42, 156, 0});

uint64_t squareBit(int x, int y) { return uint64_t(1) << (x * 8 + y); }
constexpr uint64_t kFileMask = 0xFF;                  // file a; shift by x * 8 for file x
constexpr uint64_t kRankMask = 0x0101010101010101ULL; // rank 1; shift by y for rank y

// Everything the evaluation needs from the pawns alone. Pawns move far less
// often than the evaluation runs, so probePawns() keeps this in a per-thread
// cache keyed by Board::pawnKey(): most evaluations neither recompute the
// pawn structure nor re-scan the board for the pawn questions that rooks,
// outposts and king shelter ask.
struct PawnInfo {
    uint64_t key;
    int mg, eg;             // pawn structure score (see computePawnInfo), positive = good for White
    uint64_t pawns[2];      // [White, Black] pawn squares, as squareBit()s
    uint64_t attacks[2];    // squares each side's pawns attack
    uint64_t attackSpan[2]; // squares each side's pawns could ever attack by advancing
    uint64_t passed[2];     // each side's passed pawns
};

bool fileHasPawn(const PawnInfo& info, int side, int x) { return (info.pawns[side] >> (x * 8)) & kFileMask; }

// Fills in everything but the key. The score: passed-pawn bonuses minus
// doubled/isolated/backward penalties. A passed pawn has no enemy pawn in
// front of it on its own or an adjacent file.
void computePawnInfo(const Board& board, PawnInfo& info) {
    // Per file, the rank of the rearmost pawn of each color: the only one
    // that decides whether an enemy pawn on that file is passed.
    int whiteLowest[8], blackHighest[8];
    std::fill(std::begin(whiteLowest), std::end(whiteLowest), 8);
    std::fill(std::begin(blackHighest), std::end(blackHighest), -1);
    int count[2][8] = {}; // [White, Black] pawns per file
    for (int side = 0; side < 2; side++) info.pawns[side] = info.attacks[side] = info.attackSpan[side] = info.passed[side] = 0;
    for (int x = 0; x < 8; x++) {
        for (int y = 1; y < 7; y++) {
            Piece p = board.at(x, y);
            if (p.type != PieceType::Pawn) continue;
            if (p.color == Color::White) whiteLowest[x] = std::min(whiteLowest[x], y);
            else blackHighest[x] = std::max(blackHighest[x], y);
            int side = p.color == Color::White ? 0 : 1;
            count[side][x]++;
            info.pawns[side] |= squareBit(x, y);
            int dir = side == 0 ? 1 : -1;
            for (int ax : {x - 1, x + 1}) {
                if (ax < 0 || ax > 7) continue;
                info.attacks[side] |= squareBit(ax, y + dir);
                for (int ty = y + dir; ty >= 0 && ty <= 7; ty += dir) info.attackSpan[side] |= squareBit(ax, ty);
            }
        }
    }

    int mg = 0, eg = 0;
    auto pawnAt = [&](int px, int py, Color c) {
        return Board::inBounds(px, py) && board.at(px, py).type == PieceType::Pawn && board.at(px, py).color == c;
    };
    for (int x = 0; x < 8; x++) {
        int lo = std::max(0, x - 1), hi = std::min(7, x + 1);
        for (int y = 1; y < 7; y++) {
            Piece p = board.at(x, y);
            if (p.type != PieceType::Pawn) continue;

            // Backward: friendly pawns on the adjacent files exist (so it's not
            // isolated) but all stand further up the board than this one --
            // whiteLowest/blackHighest hold each file's rearmost pawn.
            bool white = p.color == Color::White;
            int side = white ? 0 : 1;
            bool hasNeighbour = false, neighbourLevelOrBehind = false;
            for (int f : {x - 1, x + 1}) {
                if (f < 0 || f > 7 || count[side][f] == 0) continue;
                hasNeighbour = true;
                if (white ? whiteLowest[f] <= y : blackHighest[f] >= y) neighbourLevelOrBehind = true;
            }
            int dir = white ? 1 : -1;
            bool stopControlled = pawnAt(x - 1, y + 2 * dir, opponent(p.color)) || pawnAt(x + 1, y + 2 * dir, opponent(p.color));
            if (hasNeighbour && !neighbourLevelOrBehind && stopControlled) {
                mg -= (white ? 1 : -1) * kBackwardMg;
                eg -= (white ? 1 : -1) * kBackwardEg;
            }

            // Connected: supported by a pawn behind it, or side by side with one.
            {
                const int own = white ? 1 : -1;
                const int rank = white ? y : 7 - y;
                if (pawnAt(x - 1, y - dir, p.color) || pawnAt(x + 1, y - dir, p.color)) {
                    mg += own * kSupportedMg[rank];
                    eg += own * kSupportedEg[rank];
                }
                if (pawnAt(x - 1, y, p.color) || pawnAt(x + 1, y, p.color)) {
                    mg += own * kPhalanxMg[rank];
                    eg += own * kPhalanxEg[rank];
                }
            }

            bool passed = true;
            for (int f = lo; f <= hi; f++) {
                if (p.color == Color::White ? blackHighest[f] > y : whiteLowest[f] < y) passed = false;
            }
            if (!passed) continue;
            info.passed[side] |= squareBit(x, y);
            int rank = p.color == Color::White ? y : 7 - y;
            int sign = p.color == Color::White ? 1 : -1;
            mg += sign * kPassedMg[rank];
            eg += sign * kPassedEg[rank];
        }
    }

    for (int side = 0; side < 2; side++) {
        int sign = side == 0 ? 1 : -1;
        for (int x = 0; x < 8; x++) {
            int n = count[side][x];
            if (n == 0) continue;
            bool isolated = (x == 0 || count[side][x - 1] == 0) && (x == 7 || count[side][x + 1] == 0);
            mg -= sign * ((n - 1) * kDoubledMg + (isolated ? n * kIsolatedMg : 0));
            eg -= sign * ((n - 1) * kDoubledEg + (isolated ? n * kIsolatedEg : 0));
        }
    }
    info.mg = mg;
    info.eg = eg;
}

// Per-thread pawn cache. Zero-initialized, so an empty slot reads as key 0
// with no pawns -- exactly right for a pawnless position, whose key is 0.
constexpr size_t kPawnCacheSize = size_t(1) << 13;
thread_local PawnInfo pawnCache[kPawnCacheSize];

const PawnInfo& probePawns(const Board& board) {
#ifdef TUNING
    // The tuner changes the weights the cached scores were computed with.
    thread_local PawnInfo scratch;
    computePawnInfo(board, scratch);
    scratch.key = board.pawnKey();
    return scratch;
#endif
    uint64_t key = board.pawnKey();
    PawnInfo& entry = pawnCache[key & (kPawnCacheSize - 1)];
    if (entry.key != key) {
        computePawnInfo(board, entry);
        entry.key = key;
    }
    return entry;
}

// Mobility, per square a knight/bishop/rook/queen can move to above or below
// a typical count for that piece, middlegame and endgame, indexed by
// PieceType (Fruit 2.1's weights and offsets). A piece with few moves is
// boxed in, tied to defending, or blocked by its own pawns -- invisible to
// material and piece-square tables, which score a passive piece exactly
// like an active one on the same square.
TUNE_ARR(kMobilityMg, 7, {0, 0, 6, 6, 5, 2, 0});
TUNE_ARR(kMobilityEg, 7, {0, 0, 3, 3, 1, 5, 0});
TUNE_ARR(kMobilityTypical, 7, {0, 0, 6, 6, 32, 41, 0});

// Pieces bearing on the squares around one king, gathered by pieceTerms()
// (which already walks every square each piece can reach) for kingSafety().
struct KingAttack {
    int attackers = 0; // knights/bishops/rooks/queens reaching a square next to the king
    int weight = 0;    // sum of their kKingAttackWeight
};

// How dangerous each piece type is as a king attacker, indexed by PieceType.
TUNE_ARR(kKingAttackWeight, 7, {0, 0, 25, 24, 36, 63, 0});

// A rook on a file with no pawns at all, or with only enemy pawns, has a
// lane into the enemy position (Fruit 2.1's values; same in both phases).
TUNE_INT(kRookOpenFile, 18);
TUNE_INT(kRookSemiOpenFile, 14);

// A rook or queen on the 7th rank (counted from its own side), when there
// are still enemy pawns there to attack or the enemy king is cut off on the
// back rank, middlegame / endgame (Fruit 2.1's values).
TUNE_INT(kRookOn7thMg, 1);
TUNE_INT(kRookOn7thEg, 22);
TUNE_INT(kQueenOn7thMg, 1);
TUNE_INT(kQueenOn7thEg, 51);

// Outpost: a knight or bishop on the 4th-6th rank (counted from its own
// side), defended by an own pawn, on a square no enemy pawn can ever attack
// -- it can only be driven off by trading a piece for it. Middlegame /
// endgame; a knight gains more, having no long-range alternative.
TUNE_INT(kKnightOutpostMg, 34);
TUNE_INT(kKnightOutpostEg, 8);
TUNE_INT(kBishopOutpostMg, 37);
TUNE_INT(kBishopOutpostEg, 6);

bool isOutpost(const PawnInfo& pawns, int x, int y, Color color) {
    int rank = color == Color::White ? y : 7 - y;
    if (rank < 3 || rank > 5) return false;
    int side = color == Color::White ? 0 : 1;
    // Defended by an own pawn, and no enemy pawn could ever advance to attack it.
    return (pawns.attacks[side] & squareBit(x, y)) && !(pawns.attackSpan[1 - side] & squareBit(x, y));
}

// A bishop hemmed in by its own pawns: a penalty per own pawn standing on squares of
// the bishop's colour (those pawns block it, and it cannot defend the other colour's
// squares), middlegame / endgame.
TUNE_INT(kBadBishopMg, 3);
TUNE_INT(kBadBishopEg, 8);

constexpr uint64_t parityMask(int parity) {
    uint64_t mask = 0;
    for (int sq = 0; sq < 64; sq++)
        if ((sq / 8 + sq % 8) % 2 == parity) mask |= uint64_t(1) << sq;
    return mask;
}
constexpr uint64_t kSquaresOfParity[2] = {parityMask(0), parityMask(1)};

// The pieces of one colour pinned against its king by an enemy slider, and the
// squares each may still move to: along the pin line, up to the pinner.
struct Pins {
    uint64_t pinned = 0;
    int count = 0;
    int square[8];
    uint64_t line[8];

    // Where the piece on `sq` may move, as far as the pin goes.
    uint64_t allowed(int sq) const {
        for (int i = 0; i < count; i++)
            if (square[i] == sq) return line[i];
        return ~uint64_t(0);
    }
};

Pins findPins(const Board& board, Color color) {
    Pins pins;
    const Color enemy = opponent(color);
    const int ksq = board.kingFile(color) * 8 + board.kingRank(color);
    const uint64_t occupied = board.occupancy(), own = board.occupancy(color);
    const uint64_t queens = board.pieces(enemy, PieceType::Queen);
    // Enemy sliders on one of the king's lines (blockers ignored), then the pieces between.
    uint64_t pinners = ((board.pieces(enemy, PieceType::Rook) | queens) & bb::rookRays[ksq]) |
                       ((board.pieces(enemy, PieceType::Bishop) | queens) & bb::bishopRays[ksq]);
    while (pinners) {
        const int sq = bb::lsb(pinners);
        pinners = bb::clearLsb(pinners);
        const uint64_t blockers = bb::between[ksq][sq] & occupied;
        if (blockers && !bb::clearLsb(blockers) && (blockers & own)) { // exactly one piece, and it is ours
            pins.pinned |= blockers;
            pins.square[pins.count] = bb::lsb(blockers);
            pins.line[pins.count++] = bb::between[ksq][sq] | (uint64_t(1) << sq);
        }
    }
    return pins;
}

// Threats, indexed by the type of the attacked enemy piece. A piece attacked by
// a pawn, or a rook or queen attacked by a minor piece, must move or be lost to
// an exchange; one attacked and not defended at all can simply be taken. Each
// attacked piece counts once, at the cheapest kind of attacker (pawn, then
// minor piece, then rook).
TUNE_ARR(kThreatPawn, 7, {0, 0, 42, 41, 36, 39, 0});
TUNE_ARR(kThreatMinor, 7, {0, 1, 23, 28, 37, 37, 0});
TUNE_ARR(kThreatRook, 7, {0, 1, 8, 8, 0, 52, 0});
TUNE_ARR(kHanging, 7, {0, 20, 20, 18, 11, 2, 0}); // attacked by anything, defended by nothing

// Safe checks, indexed by the type of the checking piece: per square from which one of
// the side's pieces of that type could give check to the enemy king, that is not
// attacked by the enemy (so the check cannot be met by simply taking the checker) and
// is not occupied by the side's own piece. Middlegame only.
TUNE_ARR(kSafeCheck, 7, {0, 0, 54, 28, 49, 40, 0});

// Positive = good for White. Terms for each knight, bishop, rook and queen:
// mobility -- every square that's empty or holds an enemy piece and that no enemy pawn
// attacks, checks aside, and for a pinned piece only the squares along its pin -- rooks on open
// files, rooks/queens on the 7th rank, and outposts.
// Also fills attacks[c] with the
// pieces attacking color c's king zone (the squares next to that king).
int pieceTerms(const Board& board, const PawnInfo& pawns, KingAttack (&attacks)[2]) {
    int mg = 0, eg = 0;
    const uint64_t occupied = board.occupancy();
    const Pins pins[2] = {findPins(board, Color::White), findPins(board, Color::Black)};
    uint64_t attackedBy[2] = {0, 0};      // every square each side's knights, bishops, rooks, queens attack
    uint64_t attackedByMinor[2] = {0, 0}; // ... its knights and bishops
    uint64_t attackedByRook[2] = {0, 0};  // ... its rooks
    uint64_t attackedByType[2][7] = {};   // ... each type of its pieces separately
    for (int side = 0; side < 2; side++) {
        const Color color = side == 0 ? Color::White : Color::Black;
        const Color enemy = opponent(color);
        const int sign = side == 0 ? 1 : -1;
        const uint64_t own = board.occupancy(color);
        const int ekx = board.kingFile(enemy), eky = board.kingRank(enemy);
        // The enemy king's square and the squares around it.
        const uint64_t zone = bb::kingAttacks[ekx * 8 + eky] | bb::bit(ekx, eky);
        for (PieceType type : {PieceType::Knight, PieceType::Bishop, PieceType::Rook, PieceType::Queen}) {
            const int t = static_cast<int>(type);
            uint64_t pieces = board.pieces(color, type);
            while (pieces) {
                const int sq = bb::lsb(pieces);
                pieces = bb::clearLsb(pieces);
                const int x = sq / 8, y = sq % 8;
                // Every square the piece could move to: empty or holding an enemy piece.
                uint64_t reach;
                switch (type) {
                    case PieceType::Knight: reach = bb::knightAttacks[sq]; break;
                    case PieceType::Bishop: reach = bb::bishopAttacks(sq, occupied); break;
                    case PieceType::Rook: reach = bb::rookAttacks(sq, occupied); break;
                    default: reach = bb::queenAttacks(sq, occupied); break;
                }
                attackedBy[side] |= reach;
                attackedByType[side][t] |= reach;
                if (type == PieceType::Knight || type == PieceType::Bishop) attackedByMinor[side] |= reach;
                else if (type == PieceType::Rook) attackedByRook[side] |= reach;
                reach &= ~own;
                // A pinned piece still attacks along every line (the king zone test
                // below), but can only move along its pin; and a square an enemy pawn
                // attacks is no use to it, so it does not count as mobility.
                const uint64_t mobile = reach & ~pawns.attacks[1 - side];
                const int moves = bb::popcount((pins[side].pinned >> sq) & 1 ? mobile & pins[side].allowed(sq) : mobile);
                mg += sign * (moves - kMobilityTypical[t]) * kMobilityMg[t];
                eg += sign * (moves - kMobilityTypical[t]) * kMobilityEg[t];
                if (type == PieceType::Bishop) {
                    const int blockers = bb::popcount(pawns.pawns[side] & kSquaresOfParity[(x + y) % 2]);
                    mg -= sign * kBadBishopMg * blockers;
                    eg -= sign * kBadBishopEg * blockers;
                }
                if (type == PieceType::Rook) {
                    int bonus = fileHasPawn(pawns, side, x)       ? 0
                                : fileHasPawn(pawns, 1 - side, x) ? kRookSemiOpenFile
                                                                  : kRookOpenFile;
                    mg += sign * bonus;
                    eg += sign * bonus;
                }
                if ((type == PieceType::Knight || type == PieceType::Bishop) && isOutpost(pawns, x, y, color)) {
                    bool knight = type == PieceType::Knight;
                    mg += sign * (knight ? kKnightOutpostMg : kBishopOutpostMg);
                    eg += sign * (knight ? kKnightOutpostEg : kBishopOutpostEg);
                }
                int seventh = color == Color::White ? 6 : 1;
                if ((type == PieceType::Rook || type == PieceType::Queen) && y == seventh) {
                    bool targets = eky == (color == Color::White ? 7 : 0) || (pawns.pawns[1 - side] & (kRankMask << y));
                    if (targets) {
                        bool rook = type == PieceType::Rook;
                        mg += sign * (rook ? kRookOn7thMg : kQueenOn7thMg);
                        eg += sign * (rook ? kRookOn7thEg : kQueenOn7thEg);
                    }
                }
                if (reach & zone) {
                    KingAttack& a = attacks[enemy == Color::White ? 0 : 1];
                    a.attackers++;
                    a.weight += kKingAttackWeight[t];
                }
            }
        }
    }

    // Safe checks against each side's king.
    for (int side = 0; side < 2; side++) {
        const Color color = side == 0 ? Color::White : Color::Black;
        const Color enemy = opponent(color);
        const int sign = side == 0 ? 1 : -1;
        const int eksq = board.kingFile(enemy) * 8 + board.kingRank(enemy);
        const uint64_t defended = attackedBy[1 - side] | pawns.attacks[1 - side] | bb::kingAttacks[eksq];
        const uint64_t usable = ~board.occupancy(color) & ~defended;
        const uint64_t diagonalChecks = bb::bishopAttacks(eksq, occupied), straightChecks = bb::rookAttacks(eksq, occupied);
        const int knightN = bb::popcount(attackedByType[side][2] & bb::knightAttacks[eksq] & usable);
        const int bishopN = bb::popcount(attackedByType[side][3] & diagonalChecks & usable);
        const int rookN = bb::popcount(attackedByType[side][4] & straightChecks & usable);
        const int queenN = bb::popcount(attackedByType[side][5] & (diagonalChecks | straightChecks) & usable);
        mg += sign * (kSafeCheck[2] * knightN + kSafeCheck[3] * bishopN + kSafeCheck[4] * rookN + kSafeCheck[5] * queenN);
    }

    // Threats against each side's pieces.
    for (int side = 0; side < 2; side++) {
        const Color color = side == 0 ? Color::White : Color::Black;
        const Color enemy = opponent(color);
        const int sign = side == 0 ? 1 : -1;
        const uint64_t anyAttack =
            attackedBy[side] | pawns.attacks[side] | bb::kingAttacks[board.kingFile(color) * 8 + board.kingRank(color)];
        const uint64_t defended = attackedBy[1 - side] | pawns.attacks[1 - side] |
                                  bb::kingAttacks[board.kingFile(enemy) * 8 + board.kingRank(enemy)];
        int threat = 0;
        for (int t = static_cast<int>(PieceType::Pawn); t <= static_cast<int>(PieceType::Queen); t++) {
            const uint64_t victims = board.pieces(enemy, static_cast<PieceType>(t));
            const uint64_t byPawn = victims & pawns.attacks[side];
            const uint64_t byMinor = victims & attackedByMinor[side] & ~byPawn;
            const uint64_t byRook = victims & attackedByRook[side] & ~byPawn & ~byMinor;
            threat += kThreatPawn[t] * bb::popcount(byPawn) + kThreatMinor[t] * bb::popcount(byMinor) +
                      kThreatRook[t] * bb::popcount(byRook) + kHanging[t] * bb::popcount(victims & anyAttack & ~defended);
        }
        mg += sign * threat;
        eg += sign * threat;
    }

    double phase = board.gamePhase();
    return static_cast<int>(mg * phase + eg * (1.0 - phase));
}

// King shelter, per file on and next to the king's: a pawn still right in
// front of the king is ideal; one pushed a square further guards less; none
// at all leaves a hole. A file with none of the king's own pawns is a lane
// for enemy rooks and queens, worse still with no enemy pawn in the way
// either. Middlegame only: with the queens and most pieces gone there's no
// attack left to shelter from.
TUNE_INT(kShelterPushedOnce, 6);
TUNE_INT(kShelterMissing, 6);
TUNE_INT(kKingFileSemiOpen, 4);
TUNE_INT(kKingFileOpen, 20); // on top of kKingFileSemiOpen

// Shelter penalty for `color`'s king, as a positive number.
int kingShelterPenalty(const Board& board, const PawnInfo& pawns, Color color) {
    int kx = board.kingFile(color), ky = board.kingRank(color);
    int side = color == Color::White ? 0 : 1;
    int dir = side == 0 ? 1 : -1;
    auto ownPawn = [&](int x, int y) { return Board::inBounds(x, y) && (pawns.pawns[side] & squareBit(x, y)); };
    int penalty = 0;
    for (int x = std::max(0, kx - 1); x <= std::min(7, kx + 1); x++) {
        if (!ownPawn(x, ky + dir)) penalty += ownPawn(x, ky + 2 * dir) ? kShelterPushedOnce : kShelterMissing;
        if (!fileHasPawn(pawns, side, x)) {
            penalty += kKingFileSemiOpen + (fileHasPawn(pawns, 1 - side, x) ? 0 : kKingFileOpen);
        }
    }
    return penalty;
}

// King attack penalty, in percent of the attackers' summed weight, by how
// many pieces take part: one piece alone rarely mates, so it counts for
// nothing, and each extra attacker makes the attack disproportionately more
// dangerous.
TUNE_ARR(kAttackerScalePercent, 8, {0, 0, 33, 68, 110, 94, 97, 99});

int kingAttackPenalty(const KingAttack& a) {
    return a.weight * kAttackerScalePercent[std::min(a.attackers, 7)] / 100;
}

// Pawn storm: enemy pawns advancing on the king's file and the two beside it
// are the usual spearhead of an attack (most of all when the kings castled on
// opposite wings). Penalty per such pawn by how many ranks it stands in front
// of the king (1 = the next rank); pawns level with or behind the king count
// for nothing.
TUNE_ARR(kStormMg, 8, {0, 1, 19, 3, 2, 1, 0, 0});

int pawnStormPenalty(const Board& board, const PawnInfo& pawns, Color color) {
    const int kx = board.kingFile(color), ky = board.kingRank(color);
    const int enemySide = color == Color::White ? 1 : 0;
    int penalty = 0;
    for (int x = std::max(0, kx - 1); x <= std::min(7, kx + 1); x++) {
        uint64_t filePawns = (pawns.pawns[enemySide] >> (x * 8)) & 0xFF; // bit y: a pawn on rank y
        while (filePawns) {
            const int y = bb::lsb(filePawns);
            filePawns = bb::clearLsb(filePawns);
            const int ahead = color == Color::White ? y - ky : ky - y;
            if (ahead >= 1) penalty += kStormMg[std::min(ahead, 7)];
        }
    }
    return penalty;
}

// Positive = good for White. `attacks` comes from pieceTerms().
int kingSafety(const Board& board, const PawnInfo& pawns, const KingAttack (&attacks)[2]) {
    int mg = kingShelterPenalty(board, pawns, Color::Black) + kingAttackPenalty(attacks[1]) +
             pawnStormPenalty(board, pawns, Color::Black) - kingShelterPenalty(board, pawns, Color::White) -
             kingAttackPenalty(attacks[0]) - pawnStormPenalty(board, pawns, Color::White);
    return static_cast<int>(mg * board.gamePhase());
}

// Mating material: enough that the side could checkmate a lone king with its
// king's help -- a queen or a rook, two bishops, or a bishop and a knight.
// Without it (a lone king, king and one minor piece, king and two knights) it
// has no forced mate however far ahead it looks.
bool hasMatingMaterial(const Board& board, Color color) {
    if (board.pieces(color, PieceType::Queen) | board.pieces(color, PieceType::Rook)) return true;
    const uint64_t bishopBB = board.pieces(color, PieceType::Bishop);
    constexpr uint64_t kDarkSquares = 0xAA55AA55AA55AA55ULL; // squares with (file + rank) even
    const int bishops = bb::popcount(bishopBB);
    // Two bishops mate only when they stand on opposite colours.
    if (bishops >= 2) return (bishopBB & kDarkSquares) && (bishopBB & ~kDarkSquares);
    return bishops == 1 && board.pieces(color, PieceType::Knight);
}

// A rook behind a passed pawn, either side's (the rook's owner gains): from
// behind it keeps supporting (or attacking) the pawn all the way to the
// queening square. Middlegame / endgame.
TUNE_INT(kRookBehindPasserMg, 1);
TUNE_INT(kRookBehindPasserEg, 42);

// Positive = good for White.
int rookBehindPasser(const Board& board, const PawnInfo& pawns) {
    int mg = 0, eg = 0;
    for (int side = 0; side < 2; side++) {
        const int dir = side == 0 ? 1 : -1;
        uint64_t passers = pawns.passed[side];
        while (passers) {
            const int sq = bb::lsb(passers);
            passers = bb::clearLsb(passers);
            const int x = sq / 8, y = sq % 8;
            for (int ry = y - dir; ry >= 0 && ry <= 7; ry -= dir) {
                const Piece q = board.at(x, ry);
                if (q.isEmpty()) continue;
                if (q.type == PieceType::Rook) {
                    const int rookSign = q.color == Color::White ? 1 : -1;
                    mg += rookSign * kRookBehindPasserMg;
                    eg += rookSign * kRookBehindPasserEg;
                }
                break;
            }
        }
    }
    const double phase = board.gamePhase();
    return static_cast<int>(mg * phase + eg * (1.0 - phase));
}

// A passed pawn whose stop square (the square in front of it) is held by an enemy
// piece cannot advance until that piece is driven off, and a blockader on it is
// well placed: a penalty by the pawn's rank (counted from its own side), middlegame
// / endgame.
TUNE_ARR(kBlockedPasserMg, 8, {0, 0, 16, 24, 1, 3, 94, 0});
TUNE_ARR(kBlockedPasserEg, 8, {0, 0, 2, 15, 35, 40, 68, 0});

// Positive = good for White.
int blockedPassers(const Board& board, const PawnInfo& pawns) {
    int mg = 0, eg = 0;
    for (int side = 0; side < 2; side++) {
        const Color enemy = side == 0 ? Color::Black : Color::White;
        const int sign = side == 0 ? 1 : -1;
        const int dir = side == 0 ? 1 : -1;
        uint64_t passers = pawns.passed[side];
        while (passers) {
            const int sq = bb::lsb(passers);
            passers = bb::clearLsb(passers);
            const int x = sq / 8, y = sq % 8;
            const int rank = side == 0 ? y : 7 - y;
            if (board.at(x, y + dir).color == enemy) {
                mg -= sign * kBlockedPasserMg[rank];
                eg -= sign * kBlockedPasserEg[rank];
            }
        }
    }
    const double phase = board.gamePhase();
    return static_cast<int>(mg * phase + eg * (1.0 - phase));
}

// King distance to passed pawns, endgame only: per square of Chebyshev
// distance, a passer is worth more the closer its own king is to the square
// in front of it and the farther the enemy king is, scaled by how far the
// pawn has advanced (rank counted from its own side).
TUNE_ARR(kPasserKingEg, 8, {0, 0, 2, 12, 21, 29, 33, 0});

// Positive = good for White.
int passerKingDistance(const Board& board, const PawnInfo& pawns) {
    int eg = 0;
    for (int side = 0; side < 2; side++) {
        const Color color = side == 0 ? Color::White : Color::Black;
        const int sign = side == 0 ? 1 : -1;
        const int dir = side == 0 ? 1 : -1;
        const int okx = board.kingFile(color), oky = board.kingRank(color);
        const int ekx = board.kingFile(opponent(color)), eky = board.kingRank(opponent(color));
        uint64_t passers = pawns.passed[side];
        while (passers) {
            const int sq = bb::lsb(passers);
            passers = bb::clearLsb(passers);
            const int x = sq / 8, y = sq % 8;
            const int rank = side == 0 ? y : 7 - y;
            const int stopY = y + dir;
            const int own = std::max(std::abs(okx - x), std::abs(oky - stopY));
            const int enemy = std::max(std::abs(ekx - x), std::abs(eky - stopY));
            eg += sign * kPasserKingEg[rank] * (enemy - own);
        }
    }
    return static_cast<int>(eg * (1.0 - board.gamePhase()));
}

// Mop-up: against a bare king, a side with mating material wins by driving
// the king to the edge (to a corner of the bishop's colour with bishop and
// knight) and bringing its own king close, which nothing else in the
// evaluation rewards. Positive = good for White.
TUNE_INT(kMopEdge, 20);   // per step the bare king is nearer the board's edge
TUNE_INT(kMopKing, 15);   // per step the two kings are nearer each other
TUNE_INT(kMopCorner, 120); // bishop and knight: per step nearer the right corner

int mopUp(const Board& board) {
    for (int side = 0; side < 2; side++) {
        const Color winner = side == 0 ? Color::White : Color::Black;
        const Color loser = opponent(winner);
        if (board.pieces(loser, PieceType::Pawn)) continue;
        if (board.pieces(loser, PieceType::Knight) | board.pieces(loser, PieceType::Bishop) |
            board.pieces(loser, PieceType::Rook) | board.pieces(loser, PieceType::Queen))
            continue;
        if (!hasMatingMaterial(board, winner)) continue;
        const int lx = board.kingFile(loser), ly = board.kingRank(loser);
        const int distance = std::abs(lx - board.kingFile(winner)) + std::abs(ly - board.kingRank(winner));
        int bonus = kMopKing * (14 - distance);
        const uint64_t bishops = board.pieces(winner, PieceType::Bishop);
        const bool bishopAndKnight = bb::popcount(bishops) == 1 && board.pieces(winner, PieceType::Knight) &&
                                     !(board.pieces(winner, PieceType::Queen) | board.pieces(winner, PieceType::Rook));
        if (bishopAndKnight) {
            const int bs = bb::lsb(bishops);
            const int parity = (bs / 8 + bs % 8) % 2; // the colour of the bishop's squares
            // Manhattan distance to the nearest corner of that colour: (0,0), (7,7) or (0,7), (7,0).
            const int d = parity == 0 ? std::min(lx + ly, 14 - lx - ly) : std::min(lx + 7 - ly, 7 - lx + ly);
            bonus += kMopCorner * (14 - d);
        } else {
            const int fromCentre = (lx < 4 ? 3 - lx : lx - 4) + (ly < 4 ? 3 - ly : ly - 4);
            bonus += kMopEdge * fromCentre;
        }
        return side == 0 ? bonus : -bonus;
    }
    return 0;
}

// The side to move gets the first chance to use any advantage, worth a little on top of the rest.
TUNE_INT(kTempo, 14);

// Opposite-coloured bishops, one each and nothing but pawns besides, is the most drawish of
// endings: the defender's bishop holds the squares the attacker's cannot reach, so a material
// edge counts for much less. The evaluation is scaled to this percentage there.
TUNE_INT(kOppositeBishopsScale, 28);

// Endgame scaling, in percent by how many pawns the side ahead has (0 to 5 or more): with few pieces
// left a lead without pawns is mostly a draw. With none the other side must not be a bare king (which
// mop-up handles).
TUNE_ARR(kPawnsScale, 6, {31, 65, 99, 115, 124, 126});
TUNE_INT(kScalePhasePercent, 87); // the phase (in percent of the opening's) below which this applies

bool oppositeColouredBishops(const Board& board) {
    if (board.gamePhase() > 0.09) return false; // two bishops are 2 of 24 phase units; anything more is not this ending
    const uint64_t white = board.pieces(Color::White, PieceType::Bishop), black = board.pieces(Color::Black, PieceType::Bishop);
    if (!white || !black || bb::clearLsb(white) || bb::clearLsb(black)) return false;
    const int w = bb::lsb(white), b = bb::lsb(black);
    return (w / 8 + w % 8) % 2 != (b / 8 + b % 8) % 2;
}

// The neural network's evaluation (see nnue.hpp): it scores the position for the side to move. What stays of the
// hand-crafted evaluation is the dead-draw rule and mop-up, which a network trained on games that are decided long
// before a lone king is mated has nothing to say about.
int evaluateNetwork(const Board& board, Color color) {
    if (!(board.pieces(Color::White, PieceType::Pawn) | board.pieces(Color::Black, PieceType::Pawn)) &&
        !hasMatingMaterial(board, Color::White) && !hasMatingMaterial(board, Color::Black))
        return 0;
    int score = nnue::evaluate(board.nnueAccumulator(), board.sideToMove(), bb::popcount(board.occupancy()));
    if (board.sideToMove() == Color::Black) score = -score; // White's point of view, as in evaluate() below
    score += mopUp(board);
    return color == Color::White ? score : -score;
}

// Positive = good for `color`.
int evaluate(const Board& board, Color color) {
    if (nnue::active()) return evaluateNetwork(board, color);
    const PawnInfo& pawns = probePawns(board);
    // A dead draw: no pawns and neither side can mate.
    if (!(pawns.pawns[0] | pawns.pawns[1]) && !hasMatingMaterial(board, Color::White) &&
        !hasMatingMaterial(board, Color::Black))
        return 0;
    double phase = board.gamePhase();
    KingAttack attacks[2]; // on White's king, on Black's king
    int score = board.rawEval() + static_cast<int>(pawns.mg * phase + pawns.eg * (1.0 - phase)) +
                pieceTerms(board, pawns, attacks) + kingSafety(board, pawns, attacks) + mopUp(board) +
                rookBehindPasser(board, pawns) + passerKingDistance(board, pawns) + blockedPassers(board, pawns) +
                (board.sideToMove() == Color::White ? kTempo : -kTempo);
    if (oppositeColouredBishops(board)) {
        score = score * kOppositeBishopsScale / 100;
    } else if (phase * 100 < kScalePhasePercent && score != 0) {
        // Few pieces left: a side ahead with no pawn (or just one) usually cannot win.
        const int strong = score > 0 ? 0 : 1;
        const Color strongColor = strong == 0 ? Color::White : Color::Black;
        const int strongPawns = bb::popcount(pawns.pawns[strong]);
        const bool weakBare = !(pawns.pawns[1 - strong] | board.pieces(opponent(strongColor), PieceType::Knight) |
                                board.pieces(opponent(strongColor), PieceType::Bishop) |
                                board.pieces(opponent(strongColor), PieceType::Rook) |
                                board.pieces(opponent(strongColor), PieceType::Queen));
        if (strongPawns > 0 || !weakBare) score = score * kPawnsScale[std::min(strongPawns, 5)] / 100;
    }
    return (color == Color::White) ? score : -score; // score: positive = good for White
}

// ---------------------------------------------------------------------
// Search.
// ---------------------------------------------------------------------

constexpr int kMaxPly = 64;
// kMateScore/kMateThreshold are declared in ai.hpp (needed there so external
// callers formatting scores, e.g. UCI's "score mate <n>", don't duplicate
// these magic numbers).
constexpr int kMaxCheckExtensions = 16;

// kTTSize is declared in ai.hpp (needed there so callers can construct the
// persistent TranspositionTable they now own and pass into findBestMove).

struct TimeUp {};

constexpr int kCorrSize = 16384;  // entries per side, indexed by the pawn key
constexpr int kCorrGrain = 256;   // stored units per centipawn
constexpr int kCorrLimit = 32 * kCorrGrain;

struct SearchContext {
    TranspositionTable& tt;
    Move killers[kMaxPly][2]{};
    int history[2][8][8][8][8]{};
    long long nodes = 0;
    std::chrono::steady_clock::time_point deadline;
    const std::atomic<bool>* stopFlag = nullptr;
    // Game history followed by every position on the current search line
    // (root included), for repetition detection.
    std::vector<uint64_t> path;
    // The move that answered each earlier move best (a quiet one that caused a
    // cutoff): counter[color][from][to of the previous move], 0 = none, else
    // 1 + (from * 64 + to) of the reply. prevMove[ply] is the move that led to
    // the node at that ply (-1: none, as at the root or after a null move).
    uint16_t counter[2][64][64]{};
    // How far the search result has tended to land from the static eval in positions with this
    // pawn structure, by side to move (see kCorrGrain): added to the eval the pruning rules look at.
    int16_t corrHist[2][kCorrSize]{};
    int prevMove[kMaxPly + 1]{};
    // Stop (as at the deadline) once this thread has searched this many nodes.
    long long nodeLimit = std::numeric_limits<long long>::max();
    // Where this thread publishes its node count (every 4096 nodes) for the others to add up.
    std::atomic<long long>* nodeSlot = nullptr;
    // Set by thread 0 when it is done: the helper threads' work is then of no use, so they stop too.
    const std::atomic<bool>* haltFlag = nullptr;
};

int colorIdx(Color c) { return c == Color::White ? 0 : 1; }

// A move as (from square) << 6 | (to square), squares numbered x * 8 + y.
int encodeMove(const Move& m) { return ((m.fromX * 8 + m.fromY) << 6) | (m.toX * 8 + m.toY); }

// History scores stay within +-kHistoryLimit, far below the killer moves' (79'000
// and 80'000) in moveScore(). A move that causes a cutoff gets a bonus and the
// quiet moves tried before it a malus; each update pulls the score back toward
// zero in proportion to its size (history gravity), so old results fade and the
// score can never run past the limit.
constexpr int kHistoryLimit = 16'384;

void updateHistory(int& h, int bonus) { h += bonus - h * std::abs(bonus) / kHistoryLimit; }

bool seeNonNegative(const Board& board, const Move& m); // below

// Score used to order moves before searching them: TT move first, then
// captures via MVV-LVA, then promotions, then killer moves, then history.
// With `seeOrder`, a capture that loses material once the exchange plays out
// goes after all the quiet moves instead of before them.
int moveScore(const Board& board, const Move& m, const Move& ttMove, const SearchContext& ctx, int ply,
              Color color, bool seeOrder = false) {
    if (sameMove(m, ttMove)) return 1'000'000;
    if (m.captured != PieceType::None) {
        const int mvvLva = pieceValue(m.captured) * 10 - pieceValue(board.at(m.fromX, m.fromY).type);
        if (seeOrder && m.promotion == PieceType::None && !seeNonNegative(board, m)) return -50'000 + mvvLva;
        return 100'000 + mvvLva;
    }
    if (m.promotion == PieceType::Queen) return 90'000;
    if (ply < kMaxPly) {
        if (sameMove(m, ctx.killers[ply][0])) return 80'000;
        if (sameMove(m, ctx.killers[ply][1])) return 79'000;
        if (ply > 0 && ctx.prevMove[ply] >= 0 &&
            ctx.counter[colorIdx(color)][ctx.prevMove[ply] >> 6][ctx.prevMove[ply] & 63] ==
                1 + (m.fromX * 8 + m.fromY) * 64 + m.toX * 8 + m.toY)
            return 78'000;
    }
    return ctx.history[colorIdx(color)][m.fromX][m.fromY][m.toX][m.toY];
}

void orderMoves(const Board& board, MoveList& moves, const Move& ttMove, const SearchContext& ctx,
                 int ply, Color color, bool seeOrder = false) {
    // Score each move once, sort the small (score, index) pairs, then permute
    // the moves. Sorting the moves directly re-scored them on every comparison;
    // the comparison results are the same, so the order is too.
    struct Scored {
        int score;
        int index;
    };
    Scored scored[MoveList::kCapacity];
    const int n = static_cast<int>(moves.size());
    for (int i = 0; i < n; i++) scored[i] = {moveScore(board, moves[i], ttMove, ctx, ply, color, seeOrder), i};
    std::sort(scored, scored + n, [](const Scored& a, const Scored& b) { return a.score > b.score; });
    alignas(Move) unsigned char sorted[MoveList::kCapacity * sizeof(Move)];
    for (int i = 0; i < n; i++) std::memcpy(sorted + i * sizeof(Move), &moves[scored[i].index], sizeof(Move));
    std::memcpy(moves.begin(), sorted, n * sizeof(Move));
}

// ---------------------------------------------------------------------
// Static exchange evaluation: the material outcome of the capture sequence
// on one square, each side recapturing with its least valuable attacker and
// free to stop whenever continuing would lose more. Pins are ignored.
// ---------------------------------------------------------------------

// The least valuable piece of `color` attacking (tx, ty), treating every
// square in `removed` as empty -- so once a piece has captured (and is
// removed from its origin), a slider lined up behind it joins in, which is
// how x-ray recaptures come out of this without special-casing. Piece types
// are tried from pawn up to king (the king last: it can only be a last resort).
bool leastValuableAttacker(const Board& board, int tx, int ty, Color color, uint64_t removed, int& ax, int& ay) {
    const int sq = tx * 8 + ty;
    const uint64_t occupied = board.occupancy() & ~removed;
    const uint64_t diagonal = bb::bishopAttacks(sq, occupied);
    const uint64_t straight = bb::rookAttacks(sq, occupied);
    const uint64_t attackers[6] = {
        bb::pawnAttacks[color == Color::White ? 1 : 0][sq] & board.pieces(color, PieceType::Pawn),
        bb::knightAttacks[sq] & board.pieces(color, PieceType::Knight),
        diagonal & board.pieces(color, PieceType::Bishop),
        straight & board.pieces(color, PieceType::Rook),
        (diagonal | straight) & board.pieces(color, PieceType::Queen),
        bb::kingAttacks[sq] & board.pieces(color, PieceType::King),
    };
    for (uint64_t found : attackers) {
        found &= ~removed;
        if (!found) continue;
        const int from = bb::lsb(found);
        ax = from / 8;
        ay = from % 8;
        return true;
    }
    return false;
}

// Material gained by `m` (a capture or promotion) once the exchange on its
// target square has played out, in centipawns from the mover's side.
int see(const Board& board, const Move& m) {
    const int tx = m.toX, ty = m.toY;
    Piece mover = board.at(m.fromX, m.fromY);
    uint64_t removed = squareBit(m.fromX, m.fromY);
    if (m.isEnPassant) removed |= squareBit(tx, m.fromY);

    int gain[32];
    int d = 0;
    gain[0] = pieceValue(m.captured);
    if (m.promotion != PieceType::None) gain[0] += pieceValue(m.promotion) - pieceValue(PieceType::Pawn);
    int onSquare = pieceValue(m.promotion != PieceType::None ? m.promotion : mover.type);
    Color side = opponent(mover.color);
    int ax, ay;
    while (d < 31 && leastValuableAttacker(board, tx, ty, side, removed, ax, ay)) {
        // A king can only recapture if nothing defends the square anymore.
        int ox, oy;
        if (board.at(ax, ay).type == PieceType::King &&
            leastValuableAttacker(board, tx, ty, opponent(side), removed | squareBit(ax, ay), ox, oy))
            break;
        d++;
        gain[d] = onSquare - gain[d - 1];
        if (std::max(-gain[d - 1], gain[d]) < 0) break; // neither side can come out ahead by continuing
        onSquare = pieceValue(board.at(ax, ay).type);
        removed |= squareBit(ax, ay);
        side = opponent(side);
    }
    for (; d > 0; d--) gain[d - 1] = -std::max(-gain[d - 1], gain[d]);
    return gain[0];
}

// see(board, m) >= 0, skipping the exchange when taking a piece worth at
// least as much as the capturer can't come out behind.
bool seeNonNegative(const Board& board, const Move& m) {
    if (m.promotion == PieceType::None &&
        pieceValue(m.captured) >= pieceValue(board.at(m.fromX, m.fromY).type))
        return true;
    return see(board, m) >= 0;
}

// Hands out a node's moves best-first without sorting them all: most nodes cut off after
// a move or two, so picking the best remaining move each time costs less than a full
// sort, and a capture's exchange (SEE) is only worked out when the capture comes up,
// rather than for every capture at move generation. A capture that loses material is
// demoted behind the quiet moves at that point, so the order is moveScore()'s, with
// seeOrder, throughout.
class MovePicker {
public:
    static constexpr int kSeeUnknown = INT_MIN;

    MovePicker(const Board& board, MoveList& moves, const Move& ttMove, const SearchContext& ctx, int ply, Color color)
        : board_(board), moves_(moves), n_(static_cast<int>(moves.size())) {
        for (int i = 0; i < n_; i++) {
            scores_[i] = moveScore(board, moves[i], ttMove, ctx, ply, color, false);
            see_[i] = kSeeUnknown;
            // Only a capture scored by MVV-LVA (not the TT move) has its exchange still to check.
            unchecked_[i] = scores_[i] >= 100'000 && scores_[i] < 1'000'000 && moves[i].promotion == PieceType::None;
        }
    }

    // The next move, or null when none is left. The pointer stays valid until the next call.
    const Move* next() {
        while (next_ < n_) {
            int bestIdx = next_;
            for (int i = next_ + 1; i < n_; i++)
                if (scores_[i] > scores_[bestIdx]) bestIdx = i;
            if (unchecked_[bestIdx]) {
                unchecked_[bestIdx] = false;
                const Move& m = moves_[bestIdx];
                if (pieceValue(m.captured) < pieceValue(board_.at(m.fromX, m.fromY).type)) {
                    see_[bestIdx] = see(board_, m);
                    if (see_[bestIdx] < 0) {
                        scores_[bestIdx] -= 150'000; // 100'000 + mvvLva -> -50'000 + mvvLva
                        continue;
                    }
                }
            }
            std::swap(moves_[bestIdx], moves_[next_]);
            std::swap(scores_[bestIdx], scores_[next_]);
            std::swap(see_[bestIdx], see_[next_]);
            std::swap(unchecked_[bestIdx], unchecked_[next_]);
            lastSee_ = see_[next_];
            return &moves_[next_++];
        }
        return nullptr;
    }

    // The exchange value worked out for the move last returned, or kSeeUnknown.
    int lastSee() const { return lastSee_; }

private:
    const Board& board_;
    MoveList& moves_;
    int n_;
    int next_ = 0;
    int lastSee_ = kSeeUnknown;
    int scores_[MoveList::kCapacity];
    int see_[MoveList::kCapacity];
    bool unchecked_[MoveList::kCapacity];
};

void checkTime(SearchContext& ctx) {
    if (ctx.nodes >= ctx.nodeLimit) throw TimeUp{};
    if ((ctx.nodes & 0xFFF) != 0) return;
    if (ctx.nodeSlot) ctx.nodeSlot->store(ctx.nodes, std::memory_order_relaxed);
    if (std::chrono::steady_clock::now() >= ctx.deadline) throw TimeUp{};
    if (ctx.stopFlag && ctx.stopFlag->load(std::memory_order_relaxed)) throw TimeUp{};
    if (ctx.haltFlag && ctx.haltFlag->load(std::memory_order_relaxed)) throw TimeUp{};
}

// Converts a score found `ply` levels below the search root into one
// relative to mate-in-N-from-root, for stable storage/lookup in the TT
// across different root distances.
int toTT(int score, int ply) {
    if (score > kMateThreshold) return score + ply;
    if (score < -kMateThreshold) return score - ply;
    return score;
}
int fromTT(int score, int ply) {
    if (score > kMateThreshold) return score - ply;
    if (score < -kMateThreshold) return score + ply;
    return score;
}

int negamax(Board& board, int depth, int ply, int alpha, int beta, Color color, SearchContext& ctx,
            int extensionsLeft, const Move* excluded = nullptr);

// Late-move reduction: how many plies less a late, quiet move gets searched
// with. Grows with the remaining depth and with how late the move comes in
// the ordering -- the later it's ordered, the less likely it is to be best
// -- from the standard 0.75 + ln(depth) * ln(move number) / 2.25 formula.
int lmrReduction(int depth, int moveNumber) {
    static const auto table = [] {
        std::array<std::array<int, 64>, 64> t{};
        for (int d = 1; d < 64; d++)
            for (int m = 1; m < 64; m++) t[d][m] = static_cast<int>(spLmrBase / 100.0 + std::log(d) * std::log(m) / (spLmrDiv / 100.0));
        return t;
    }();
    return table[std::min(depth, 63)][std::min(moveNumber, 63)];
}

// Searches captures/promotions (all moves if in check) until the position is
// "quiet," to avoid evaluating mid-capture-sequence (the horizon effect).
//
// Mutates `board` in place via make/unmake instead of copying it per move.
// If checkTime() throws TimeUp mid-recursion, the unwind skips the pending
// unmakeMove() calls on every frame between here and searchRootFull's catch,
// leaving `board` with unbalanced moves applied. That's safe only because
// the catch site in searchRootFull never reads `board` again afterward — if
// that ever changes, this would need a scope-guard around each make/unmake.
int quiescence(Board& board, int alpha, int beta, Color color, int ply, SearchContext& ctx) {
    ctx.nodes++;
    checkTime(ctx);
    if (ply >= kMaxPly - 1) return evaluate(board, color);

    // Quiescence's own conceptual search depth is 0, so any stored entry
    // (from a full negamax search or a prior quiescence visit) is at least
    // as deep -- the `ttEntry.depth >= depth` gate that guards negamax's
    // probe is unconditionally true here and is omitted accordingly.
    uint64_t key = board.hashKey();
    Move ttMove{};
    TTEntry ttEntry;
    if (ctx.tt.probe(key, ttEntry)) {
        if (ttEntry.hasMove) ttMove = ttEntry.best;
        int ttScore = fromTT(ttEntry.score, ply);
        if (ttEntry.flag == TTFlag::Exact) return ttScore;
        if (ttEntry.flag == TTFlag::LowerBound) alpha = std::max(alpha, ttScore);
        else if (ttEntry.flag == TTFlag::UpperBound) beta = std::min(beta, ttScore);
        if (alpha >= beta) return ttScore;
    }

    bool inCheck = board.isInCheck(color);
    int standPat = 0;
    if (!inCheck) {
        // Lazy evaluation: when material and piece-square values alone are already this far
        // above beta, the positional terms (rarely worth that much) cannot bring the score
        // back under it, so skip them.
        const int kLazyMargin = spLazyMargin;
        const int quick = color == Color::White ? board.rawEval() : -board.rawEval();
        if (quick - kLazyMargin >= beta) return quick;
        standPat = ttEntry.eval != kTTNoEval ? ttEntry.eval : evaluate(board, color);
        if (standPat >= beta) return standPat;
        if (standPat > alpha) alpha = standPat;
    }

    // Each move is tested for legality when it is played (all of them in check, only a king move, en
    // passant or a pinned piece's move otherwise).
    MoveList candidates;
    const int ownKingSq = board.kingFile(color) * 8 + board.kingRank(color);
    uint64_t pinnedMask = 0;
    if (inCheck) {
        candidates = board.pseudoLegal(color, false);
    } else {
        candidates = board.pseudoLegal(color, true);
        pinnedMask = board.pinnedPieces(color);
    }
    bool anyLegal = false;
    orderMoves(board, candidates, ttMove, ctx, ply, color);

    int origAlpha = alpha;
    Move bestMove{};
    bool haveBestMove = false;
    int best = inCheck ? INT_MIN : standPat;
    // Delta pruning margin: the most a capture is assumed to swing the
    // positional part of the eval, on top of the captured piece's value.
    const int kDeltaMargin = spDeltaMargin;
    for (const Move& m : candidates) {
        // Delta pruning: even winning the captured piece for free can't lift
        // the score to alpha, so this capture can't matter.
        if (!inCheck && m.promotion == PieceType::None && standPat + pieceValue(m.captured) + kDeltaMargin <= alpha)
            continue;
        // A capture that loses material once the exchange plays out can't
        // raise the stand-pat score, so it isn't worth searching.
        if (!inCheck && !seeNonNegative(board, m)) continue;
        Board::UndoState undo;
        board.makeMove(m, undo);
        {
            const int from = m.fromX * 8 + m.fromY;
            if ((inCheck || from == ownKingSq || m.isEnPassant || ((pinnedMask >> from) & 1)) &&
                board.isInCheck(color)) {
                board.unmakeMove(m, undo); // illegal: leaves the mover's king attacked
                continue;
            }
        }
        anyLegal = true;
        ctx.tt.prefetch(board.hashKey());
        int score = -quiescence(board, -beta, -alpha, opponent(color), ply + 1, ctx);
        board.unmakeMove(m, undo);
        if (score > best) {
            best = score;
            bestMove = m;
            haveBestMove = true;
        }
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
    }
    if (inCheck && !anyLegal) return -(kMateScore - ply); // checkmate

    TTFlag flag = best <= origAlpha ? TTFlag::UpperBound : (best >= beta ? TTFlag::LowerBound : TTFlag::Exact);
    ctx.tt.store(key, 0, toTT(best, ply), flag, bestMove, haveBestMove, inCheck ? kTTNoEval : standPat);
    return best;
}

// A checking move often only looks safe because the fixed-depth search
// stops right after it, one ply before the opponent's forced reply exposes
// that it actually lost material for nothing. Extending search depth by one
// ply whenever the side to move is in check lets the search see past that
// forcing sequence. `extensionsLeft` bounds the total extensions per line so
// a long forced-check sequence can't blow up recursion depth or search time.
int negamax(Board& board, int depth, int ply, int alpha, int beta, Color color, SearchContext& ctx,
            int extensionsLeft, const Move* excluded) {
    ctx.nodes++;
    checkTime(ctx);

    bool inCheck = board.isInCheck(color);
    if (extensionsLeft > 0 && inCheck) {
        depth++;
        extensionsLeft--;
    }

    // Any repetition of an earlier position (in the game or on this line)
    // is scored as a draw: repeating once more is always available to the
    // side it suits, so the true value is at best a draw either way.
    uint64_t key = board.hashKey();
    // Only the plies since the last capture or pawn move can hold the same position (the
    // fifty-move clock counts them), so scan back no further than that instead of the whole
    // game history.
    {
        const size_t window = std::min(ctx.path.size(), static_cast<size_t>(board.halfmoveClock()));
        const uint64_t* last = ctx.path.data() + ctx.path.size();
        for (size_t i = 1; i <= window; i++)
            if (last[-static_cast<std::ptrdiff_t>(i)] == key) return 0;
    }
    // Fifty-move rule: 100 plies without a capture or pawn move is a draw.
    if (board.halfmoveClock() >= 100) return 0;

    // Mate distance pruning: nothing from here can beat mating right at
    // this ply or be worse than being mated at the next, so a window
    // outside those bounds can't hold a better result.
    alpha = std::max(alpha, -(kMateScore - ply));
    beta = std::min(beta, kMateScore - ply - 1);
    if (alpha >= beta) return alpha;

    if (depth <= 0) return quiescence(board, alpha, beta, color, ply, ctx);

    int origAlpha = alpha;
    bool pvNode = beta - alpha > 1; // searched with an open window: its exact score matters
    Move ttMove{};
    TTEntry ttEntry;
    bool haveTTMove = false;
    const bool ttHit = ctx.tt.probe(key, ttEntry);
    if (ttHit) {
        if (ttEntry.hasMove) {
            ttMove = ttEntry.best;
            haveTTMove = true;
        }
        // (A singular-extension search excludes a move, so a stored result for the whole
        // position says nothing about it.)
        if (!excluded && ttEntry.depth >= depth) {
            int score = fromTT(ttEntry.score, ply);
            if (ttEntry.flag == TTFlag::Exact) return score;
            if (ttEntry.flag == TTFlag::LowerBound) alpha = std::max(alpha, score);
            else if (ttEntry.flag == TTFlag::UpperBound) beta = std::min(beta, score);
            if (alpha >= beta) return score;
        }
    }

    // The static eval, for the pruning decisions below -- only computed where
    // one of them can apply, since it isn't free.
    bool haveStaticEval = !pvNode && !inCheck;
    const int rawEval = haveStaticEval ? (ttEntry.eval != kTTNoEval ? ttEntry.eval : evaluate(board, color)) : 0;
    int16_t* corr = haveStaticEval ? &ctx.corrHist[colorIdx(color)][board.pawnKey() & (kCorrSize - 1)] : nullptr;
    const int staticEval = haveStaticEval ? rawEval + *corr / kCorrGrain : 0;

    // Reverse futility pruning: if the static eval beats beta by more than
    // the opponent could plausibly win back in the remaining plies, assume
    // the position holds and cut without searching it.
    const int kReverseFutilityMargin = spRfpMargin; // per remaining ply
    if (haveStaticEval && depth <= spRfpMaxDepth && beta < kMateThreshold && beta > -kMateThreshold &&
        staticEval - kReverseFutilityMargin * depth >= beta)
        return staticEval;

    // Razoring: this far below alpha at low depth, only a capture sequence could save the node, so
    // let quiescence decide and cut when it confirms that nothing does.
    if (haveStaticEval && depth <= spRazorMaxDepth && !excluded && alpha > -kMateThreshold && alpha < kMateThreshold &&
        staticEval + spRazorMargin * depth <= alpha) {
        const int q = quiescence(board, alpha, alpha + 1, color, ply, ctx);
        if (q <= alpha) return q;
    }

    // Null-move pruning: "pass" (give the opponent a free move) and search
    // at a reduced depth with a minimal window around beta. If the
    // position is still so good after passing that it fails high, a real
    // move is almost certainly at least that good too, so prune without
    // searching the real moves properly.
    //
    // Guarded against the two classic failure modes: never in check (no
    // legal "pass" exists from check, and the resulting position would be
    // nonsense), and never without at least one piece besides pawns/king
    // (`hasNonPawnMaterial`) -- king+pawn endgames are exactly where
    // zugzwang (passing being *better* than every legal move) shows up, so
    // "if passing is fine, a real move is even better" breaks down hardest
    // there; this is the highest-risk part of this technique and the guard
    // this codebase's endgame benchmark position specifically exercises.
    // Also skipped near mate scores, since a reduced-depth null search
    // can't be trusted to find or avoid a forced mate.
    //
    // If checkTime() throws TimeUp inside the null-move search, the
    // pending unmakeNullMove() below is skipped -- safe under the same
    // existing invariant as every other make/unmake in this function: the
    // unwind only ever stops at searchRootFull's catch, which never reads
    // `board` again.
    const int kNullMoveMinDepth = spNullMinDepth;
    const int nullMoveReduction = spNullBase + depth / spNullDiv; // deeper nodes can afford to cut deeper
    // Only at a non-PV node whose static eval already reaches beta: where it
    // doesn't, passing is unlikely to hold the score, and the search it costs is wasted.
    if (haveStaticEval && staticEval >= beta && depth >= kNullMoveMinDepth && beta < kMateThreshold &&
        beta > -kMateThreshold && board.hasNonPawnMaterial(color)) {
        Board::NullMoveUndo nu;
        board.makeNullMove(nu);
        if (ply + 1 <= kMaxPly) ctx.prevMove[ply + 1] = -1;
        int nullScore =
            -negamax(board, depth - 1 - nullMoveReduction, ply + 1, -beta, -beta + 1, opponent(color), ctx, 0);
        board.unmakeNullMove(nu);
        if (nullScore >= beta) return beta;
    }

    // ProbCut: a capture that still beats beta by a margin after a quiescence search and a
    // shallower full search very probably makes this node fail high, so cut without searching it.
    // (Skipped when the table already says the position is worth less than that.)
    if (haveStaticEval && !excluded && depth >= spProbCutMinDepth && std::abs(beta) < kMateThreshold) {
        const int probBeta = beta + spProbCutMargin;
        if (!(ttHit && ttEntry.depth >= depth - 3 && ttEntry.flag != TTFlag::LowerBound &&
              fromTT(ttEntry.score, ply) < probBeta)) {
            MoveList captures = board.pseudoLegal(color, true);
            ctx.path.push_back(key);
            for (const Move& m : captures) {
                if (m.captured == PieceType::None || see(board, m) < probBeta - staticEval) continue;
                Board::UndoState undo;
                board.makeMove(m, undo);
                if (board.isInCheck(color)) { // illegal: leaves the mover's king attacked
                    board.unmakeMove(m, undo);
                    continue;
                }
                if (ply + 1 <= kMaxPly) ctx.prevMove[ply + 1] = encodeMove(m);
                int v = -quiescence(board, -probBeta, -probBeta + 1, opponent(color), ply + 1, ctx);
                if (v >= probBeta)
                    v = -negamax(board, depth - 4, ply + 1, -probBeta, -probBeta + 1, opponent(color), ctx, extensionsLeft);
                board.unmakeMove(m, undo);
                if (v >= probBeta) {
                    ctx.path.pop_back();
                    return v - (probBeta - beta);
                }
            }
            ctx.path.pop_back();
        }
    }

    // Singular extension: if the stored best move is the only one that holds its
    // score -- every other move, searched shallower against a bound just below it,
    // fails low -- it deserves an extra ply; and if the others fail high even so,
    // the node can be cut right away.
    const int kSingularMinDepth = spSingularMinDepth;
    int singularExtension = 0;
    if (!excluded && !inCheck && haveTTMove && depth >= kSingularMinDepth && ttEntry.depth >= depth - spSingularDepthGap &&
        ttEntry.flag != TTFlag::UpperBound && std::abs(fromTT(ttEntry.score, ply)) < kMateThreshold) {
        const int sBeta = fromTT(ttEntry.score, ply) - spSingularMargin * depth;
        const int s = negamax(board, (depth - 1) / 2, ply, sBeta - 1, sBeta, color, ctx, extensionsLeft, &ttMove);
        if (s < sBeta - spSingularDoubleMargin) singularExtension = 2; // no other move comes close: two plies
        else if (s < sBeta) singularExtension = 1;
        else if (sBeta >= beta) return sBeta;
        else if (fromTT(ttEntry.score, ply) >= beta) singularExtension = -1; // the others nearly match it: a ply less
    }

    // Legality is tested lazily: only a move that is actually searched needs it, and a node that cuts
    // off never reaches most of its moves. Not in check, only a king move, en passant or a move by a
    // pinned piece can leave the king attacked; in check every move must be tested.
    const int ownKingSq = board.kingFile(color) * 8 + board.kingRank(color);
    const uint64_t pinnedMask = inCheck ? 0 : board.pinnedPieces(color);
    auto mayBeIllegal = [&](const Move& mv) {
        if (inCheck) return true;
        const int from = mv.fromX * 8 + mv.fromY;
        return from == ownKingSq || mv.isEnPassant || ((pinnedMask >> from) & 1);
    };
    MoveList moves = board.pseudoLegal(color, false);
    MovePicker picker(board, moves, ttMove, ctx, ply, color);

    const int kLmrStartIndex = spLmrStart; // first 4 moves (index 0-3) always searched at full depth
    const int kLmrMinDepth = spLmrMinDepth;

    // Futility pruning: this close to the leaves, a quiet move is assumed to
    // move the eval by at most kFutilityMargin[depth]. If even that can't
    // lift the static eval to alpha, every quiet, non-checking move after
    // the first is skipped. Not at PV nodes, in check, or near mate scores.
    const int kFutilityMargin[3] = {0, spFutMargin1, spFutMargin2};
    bool futile = haveStaticEval && depth <= 2 && alpha > -kMateThreshold && alpha < kMateThreshold &&
                  staticEval + kFutilityMargin[depth] <= alpha;

    Move bestMove = moves.front(); // (the first generated; overwritten by the first move searched)
    int best = INT_MIN;
    int moveIndex = 0;
    bool anyLegal = false; // some legal move exists (a lazily generated list may hold only illegal ones)
    Move triedQuiets[64]; // quiet moves searched without a cutoff, for the history malus
    int numTriedQuiets = 0;
    ctx.path.push_back(key); // popped below; a TimeUp unwind leaves it, see searchWindow
    while (const Move* picked = picker.next()) {
        const Move& m = *picked;
        if (excluded && sameMove(m, *excluded)) {
            anyLegal = true; // the excluded move is legal by construction
            continue;
        }
        // SEE pruning: at shallow depth in a non-PV node, a capture that loses more
        // than spSeePruneMargin cp per remaining ply is skipped (a legal move was
        // already searched, since moveIndex > 0 and best is not a mate score).
        if (!pvNode && !inCheck && moveIndex > 0 && depth <= spSeePruneDepth && best > -kMateThreshold &&
            m.captured != PieceType::None && m.promotion == PieceType::None &&
            (picker.lastSee() != MovePicker::kSeeUnknown ? picker.lastSee() : see(board, m)) <
                -spSeePruneMargin * depth) {
            moveIndex++;
            continue;
        }
        // History pruning: at shallow depth in a non-PV node, a quiet move that has mostly failed
        // before is skipped.
        if (!pvNode && !inCheck && moveIndex > 0 && depth <= spHistPruneDepth && best > -kMateThreshold &&
            m.captured == PieceType::None && m.promotion == PieceType::None &&
            ctx.history[colorIdx(color)][m.fromX][m.fromY][m.toX][m.toY] < -spHistPruneMargin * depth) {
            moveIndex++;
            continue;
        }
        // Quiet-move SEE pruning: at shallow depth in a non-PV node, a quiet move onto a square where it
        // would simply be lost (or lose more than a margin) is skipped.
        if (!pvNode && !inCheck && moveIndex > 0 && depth <= spQuietSeeDepth && best > -kMateThreshold &&
            m.captured == PieceType::None && m.promotion == PieceType::None &&
            see(board, m) < -spQuietSeeMargin * depth) {
            moveIndex++;
            continue;
        }
        Board::UndoState undo;
        board.makeMove(m, undo);
        if (mayBeIllegal(m) && board.isInCheck(color)) { // illegal: leaves the mover's king attacked
            board.unmakeMove(m, undo);
            continue;
        }
        anyLegal = true;
        ctx.tt.prefetch(board.hashKey());
        if (ply + 1 <= kMaxPly) ctx.prevMove[ply + 1] = encodeMove(m);
        const int extension = sameMove(m, ttMove) ? singularExtension : 0;

        // Late-move pruning: at shallow depth in a non-PV node, the quiet moves ordered
        // after the first few are very unlikely to matter.
        const bool lateQuiet = !pvNode && !inCheck && depth <= spLmpMaxDepth && moveIndex >= spLmpBase + depth * depth &&
                               best > -kMateThreshold;
        if ((futile || lateQuiet) && moveIndex > 0 && m.captured == PieceType::None &&
            m.promotion == PieceType::None && !board.isInCheck(opponent(color))) {
            board.unmakeMove(m, undo);
            moveIndex++;
            continue;
        }

        // LMR candidate check: quiet (no capture/promotion), ordered late,
        // not a killer, at enough depth to meaningfully reduce. isInCheck()
        // (whether THIS move checks the opponent) is the one relatively
        // expensive part, so it's only evaluated once the cheap conditions
        // already qualify -- a checking move is excluded from reduction
        // since, like the check-extension above, a fixed-depth search cut
        // short right after a check is exactly where tactics hide.
        bool isKiller = ply < kMaxPly && (sameMove(m, ctx.killers[ply][0]) || sameMove(m, ctx.killers[ply][1]));
        bool lmrCandidate = moveIndex >= kLmrStartIndex && depth >= kLmrMinDepth && m.captured == PieceType::None &&
                             m.promotion == PieceType::None && !isKiller;
        bool doLMR = lmrCandidate && !board.isInCheck(opponent(color));
        int reduction = 0;
        if (doLMR) {
            // At least 1 ply, and never so much that the move drops straight
            // into quiescence; a PV node gets one ply less.
            // A move with a good history reduces less, one with a bad history more.
            const int h = ctx.history[colorIdx(color)][m.fromX][m.fromY][m.toX][m.toY];
            reduction = std::clamp(lmrReduction(depth, moveIndex + 1) - (pvNode ? 1 : 0) - h / spLmrHistDiv, 1, depth - 2);
        }

        // PVS: the first move (typically the TT move / best-ordered guess)
        // gets the full window, since it's expected to be the actual best
        // move and we want its exact score. Every later move is searched
        // with a zero/null window first -- a cheap "is this better than
        // what we already have?" test -- optionally at reduced depth too
        // if it's an LMR candidate -- and re-searched (first at full depth
        // if it was reduced, then with the full window if it's still
        // between alpha and beta) only when it unexpectedly fails high,
        // meaning it might actually be better and its exact value matters.
        // With good move ordering (already in place via TT/MVV-LVA/
        // killers/history) most non-first moves fail low and never need
        // either re-search.
        int score;
        if (moveIndex == 0) {
            score = -negamax(board, depth - 1 + extension, ply + 1, -beta, -alpha, opponent(color), ctx, extensionsLeft);
        } else {
            score = -negamax(board, depth - 1 - reduction + extension, ply + 1, -alpha - 1, -alpha, opponent(color), ctx,
                              extensionsLeft);
            if (reduction > 0 && score > alpha) {
                score = -negamax(board, depth - 1 + extension, ply + 1, -alpha - 1, -alpha, opponent(color), ctx,
                                  extensionsLeft);
            }
            if (score > alpha && score < beta) {
                score = -negamax(board, depth - 1 + extension, ply + 1, -beta, -alpha, opponent(color), ctx,
                                  extensionsLeft);
            }
        }
        board.unmakeMove(m, undo);
        moveIndex++;
        if (score > best) {
            best = score;
            bestMove = m;
        }
        if (best > alpha) alpha = best;
        if (alpha >= beta) {
            if (m.captured == PieceType::None && ply < kMaxPly) {
                if (!sameMove(m, ctx.killers[ply][0])) {
                    ctx.killers[ply][1] = ctx.killers[ply][0];
                    ctx.killers[ply][0] = m;
                }
                const int bonus = std::min(depth * depth * spHistBonus, spHistCap);
                auto history = [&](const Move& q) -> int& {
                    return ctx.history[colorIdx(color)][q.fromX][q.fromY][q.toX][q.toY];
                };
                updateHistory(history(m), bonus);
                for (int i = 0; i < numTriedQuiets; i++) updateHistory(history(triedQuiets[i]), -bonus);
                if (ply > 0 && ctx.prevMove[ply] >= 0)
                    ctx.counter[colorIdx(color)][ctx.prevMove[ply] >> 6][ctx.prevMove[ply] & 63] =
                        static_cast<uint16_t>(1 + (m.fromX * 8 + m.fromY) * 64 + m.toX * 8 + m.toY);
            }
            break;
        }
        if (m.captured == PieceType::None && m.promotion == PieceType::None && numTriedQuiets < 64)
            triedQuiets[numTriedQuiets++] = m;
    }
    ctx.path.pop_back();
    if (!anyLegal) return inCheck ? -(kMateScore - ply) : 0; // checkmate or stalemate

    TTFlag flag = best <= origAlpha ? TTFlag::UpperBound : (best >= beta ? TTFlag::LowerBound : TTFlag::Exact);
    // Learn from the result unless it says nothing about the eval: a capture as best move, a bound
    // on the wrong side of the eval, or a mate score.
    if (haveStaticEval && !excluded && std::abs(best) < kMateThreshold && bestMove.captured == PieceType::None &&
        !(flag == TTFlag::LowerBound && best <= staticEval) && !(flag == TTFlag::UpperBound && best >= staticEval)) {
        const int target = std::clamp((best - rawEval) * kCorrGrain, -kCorrLimit, kCorrLimit);
        const int weight = std::min(depth + 1, 16);
        *corr = static_cast<int16_t>((*corr * (256 - weight) + target * weight) / 256);
    }
    if (!excluded) ctx.tt.store(key, depth, toTT(best, ply), flag, bestMove, true, haveStaticEval ? rawEval : kTTNoEval);
    return best;
}

struct RootSearchResult {
    Move move{};
    int score = INT_MIN;
    int depthReached = 0;
    long long nodes = 0;
};

// Runs a full iterative-deepening search over the *entire* root move list —
// every thread sees every candidate move, Lazy-SMP style — sharing `tt`
// with every other concurrently-running thread. A cutoff or refutation one
// thread stores becomes an instant pruning opportunity for every other
// thread searching the same subtree, instead of each thread redoing the
// same work in isolation. `threadIndex` seeds a small move-ordering jitter
// for threads other than 0, so helper threads don't all walk an identical
// tree in lockstep; thread 0 always searches with the unperturbed,
// TT/killer/history-driven ordering as the deterministic baseline. Jitter
// only ever reorders candidates — it never removes one from consideration
// at any depth — so every thread's search remains fully sound.
// The history and counter-move tables a search thread ends with, kept for its next search
// (halved on the way back in) so a move starts with what the last one learned. One per
// thread; they belong to one game, so a new transposition table (ucinewgame) drops them.
struct PersistedHeuristics {
    int history[2][8][8][8][8]{};
    uint16_t counter[2][64][64]{};
};
std::vector<std::unique_ptr<PersistedHeuristics>> g_persisted;
uint64_t g_persistedOwner = 0;

RootSearchResult searchRootFull(Board& board, Color aiColor, const std::vector<Move>& rootMoves, int maxDepth,
                                 std::chrono::steady_clock::time_point deadline, TranspositionTable& tt,
                                 size_t threadIndex, const std::atomic<bool>* stopFlag = nullptr,
                                 const std::function<void(const SearchProgress&)>& onDepthComplete = {},
                                 const std::vector<uint64_t>& gameHistory = {},
                                 std::chrono::steady_clock::time_point softDeadline =
                                     std::chrono::steady_clock::time_point::max(),
                                 PersistedHeuristics* persisted = nullptr,
                                 long long nodeLimit = std::numeric_limits<long long>::max(),
                                 std::atomic<long long>* nodeSlots = nullptr, size_t numSlots = 0,
                                 const std::atomic<bool>* haltFlag = nullptr) {
    RootSearchResult result;
    result.move = rootMoves.front();
    const auto searchStart = std::chrono::steady_clock::now();
    int stableDepths = 0; // consecutive completed depths whose best move did not change

    SearchContext ctx{tt, {}, {}, 0, deadline, stopFlag, gameHistory};
    std::fill(std::begin(ctx.prevMove), std::end(ctx.prevMove), -1);
    ctx.nodeLimit = nodeLimit;
    ctx.haltFlag = haltFlag;
    if (nodeSlots) ctx.nodeSlot = &nodeSlots[threadIndex];
    if (persisted) {
        std::memcpy(ctx.counter, persisted->counter, sizeof(ctx.counter));
        int* hist = &ctx.history[0][0][0][0][0];
        const int* old = &persisted->history[0][0][0][0][0];
        for (size_t i = 0; i < sizeof(ctx.history) / sizeof(int); i++) hist[i] = old[i] / 2;
    }
    ctx.path.push_back(board.hashKey());
    const size_t rootPathSize = ctx.path.size();
    std::mt19937 jitterRng(0x9E3779B9u + static_cast<unsigned>(threadIndex));
    std::uniform_int_distribution<int> jitterDist(0, 24);

    for (int depth = 1; depth <= maxDepth; depth++) {
        Move ttMoveAtRoot{};
        TTEntry rootEntry;
        if (tt.probe(board.hashKey(), rootEntry)) {
            if (rootEntry.hasMove) ttMoveAtRoot = rootEntry.best;
        }

        std::vector<std::pair<int, Move>> scored;
        scored.reserve(rootMoves.size());
        for (const Move& m : rootMoves) {
            int s = moveScore(board, m, ttMoveAtRoot, ctx, 0, aiColor);
            if (threadIndex != 0 && !sameMove(m, ttMoveAtRoot)) s += jitterDist(jitterRng);
            scored.push_back({s, m});
        }
        std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<Move> moves;
        moves.reserve(scored.size());
        for (const auto& sm : scored) moves.push_back(sm.second);

        // Runs the root move loop within a given (alpha, beta) window.
        // Factored out so aspiration windows (below) can retry the same
        // depth with a wider window on failure without duplicating the loop.
        auto searchWindow = [&](int aLo, int aHi) -> std::tuple<bool, int, Move> {
            int lo = aLo, hi = aHi;
            Move bestMoveLocal = moves.front();
            int bestLocal = INT_MIN;
            bool completedLocal = true;
            ctx.path.resize(rootPathSize); // drop anything a previous TimeUp unwind left behind
            try {
                bool first = true;
                for (const Move& m : moves) {
                    Board::UndoState undo;
                    board.makeMove(m, undo);
                    ctx.prevMove[1] = encodeMove(m);
                    // PVS, as in negamax: the first move gets the full window;
                    // the rest a null window that only shows they're no
                    // better, re-searched in full when one might be.
                    int score;
                    if (first) {
                        score = -negamax(board, depth - 1, 1, -hi, -lo, opponent(aiColor), ctx, kMaxCheckExtensions);
                        first = false;
                    } else {
                        score =
                            -negamax(board, depth - 1, 1, -lo - 1, -lo, opponent(aiColor), ctx, kMaxCheckExtensions);
                        if (score > lo && score < hi) {
                            score =
                                -negamax(board, depth - 1, 1, -hi, -lo, opponent(aiColor), ctx, kMaxCheckExtensions);
                        }
                    }
                    board.unmakeMove(m, undo);
                    if (score > bestLocal) {
                        bestLocal = score;
                        bestMoveLocal = m;
                    }
                    if (bestLocal > lo) lo = bestLocal;
                    if (lo >= hi) break; // failed high: the caller widens the window and redoes this depth
                }
            } catch (const TimeUp&) {
                completedLocal = false;
            }
            return {completedLocal, bestLocal, bestMoveLocal};
        };

        // Once a prior depth has completed, its score is usually close to
        // this depth's true score, so search a narrow window around it
        // first -- most of the tree fails low/high against a wide window
        // that alpha-beta then has to spend nodes narrowing down anyway.
        // Any window failure (score lands outside the narrow window, so
        // its exact value wasn't determined) falls back to a full-window
        // re-search of the same depth, guaranteeing a correct result --
        // this is a pure search-order optimization, never a source of
        // incorrect scores.
        constexpr int kInfinity = INT_MAX - 1;
        int delta = spAspiration; // grows by half each time the score lands outside the window
        int alpha = -kInfinity, beta = kInfinity;
        if (result.depthReached > 0) {
            alpha = std::max(-kInfinity, result.score - delta);
            beta = std::min(kInfinity, result.score + delta);
        }

        auto [completed, depthBest, depthBestMove] = searchWindow(alpha, beta);
        // A score outside the window only bounds the true one: widen that side (the
        // first fail high or low) and search the depth again, falling back to a full
        // window once the margin is large.
        while (completed && (depthBest <= alpha || depthBest >= beta) && !(alpha == -kInfinity && beta == kInfinity)) {
            if (depthBest <= alpha) alpha = std::max(-kInfinity, depthBest - delta);
            else beta = std::min(kInfinity, depthBest + delta);
            delta += delta / 2;
            if (delta > 400) alpha = -kInfinity, beta = kInfinity;
            std::tie(completed, depthBest, depthBestMove) = searchWindow(alpha, beta);
        }

        if (!completed) break;

        const bool moveChanged = result.depthReached > 0 && !sameMove(depthBestMove, result.move);
        const int scoreDrop = result.depthReached > 0 ? result.score - depthBest : 0;
        stableDepths = moveChanged ? 0 : stableDepths + 1;
        result.move = depthBestMove;
        result.score = depthBest;
        result.depthReached = depth;
        tt.store(board.hashKey(), depth, depthBest, TTFlag::Exact, depthBestMove, true);

        // Fired after the store above, so extractPV()'s first hop (this
        // root move) is guaranteed present in tt at this instant.
        if (onDepthComplete) {
            // This thread's exact count plus what the others last published.
            long long total = ctx.nodes;
            for (size_t i = 0; i < numSlots; i++)
                if (i != threadIndex) total += nodeSlots[i].load(std::memory_order_relaxed);
            onDepthComplete(SearchProgress{depth, depthBest, total, depthBestMove});
        }

        if (depthBest > kMateThreshold) break; // forced mate found, no need to search deeper
        // Past the soft limit, the next depth -- typically taking as long as
        // all the previous ones together -- would likely be cut off
        // unfinished and thrown away, so stop here instead.
        if (softDeadline != std::chrono::steady_clock::time_point::max()) {
            // Spend less when the best move has held for several depths, more
            // when it just changed or the score fell: those are the moments an
            // extra depth is most likely to change the move.
            double scale = 1.0;
            if (depth >= 6) {
                if (moveChanged) scale = 1.5;
                else if (stableDepths >= 3) scale = 0.6;
                if (scoreDrop > 30) scale *= 1.3;
            }
            const auto soft = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                (softDeadline - searchStart) * scale);
            if (std::chrono::steady_clock::now() >= searchStart + soft) break;
        }
    }

    result.nodes = ctx.nodes;
    if (persisted) {
        std::memcpy(persisted->counter, ctx.counter, sizeof(ctx.counter));
        std::memcpy(persisted->history, ctx.history, sizeof(ctx.history));
    }
    return result;
}

} // namespace

#ifdef TUNING
// Static evaluation from White's point of view, for tools/tune.cpp.
int tunerEvaluate(const Board& board) { return evaluate(board, Color::White); }
#endif

std::vector<Move> extractPV(const Board& rootBoard, const TranspositionTable& tt, int maxLen,
                            const std::vector<uint64_t>& history) {
    std::vector<Move> pv;
    std::vector<uint64_t> seen;
    Board board = rootBoard;
    for (int i = 0; i < maxLen; i++) {
        if (board.halfmoveClock() >= 100) break; // drawn by the fifty-move rule: the search scores it 0, the line ends
        uint64_t key = board.hashKey();
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) break; // cycle guard
        if (std::count(history.begin(), history.end(), key) >= 2) break;   // third occurrence: a draw
        seen.push_back(key);

        TTEntry entry;
        if (!tt.probe(key, entry) || !entry.hasMove) break;

        // The table keeps only the move's squares: play the matching legal
        // move, which also rejects a hash collision or a stale entry.
        MoveList legal = board.legalMoves(board.sideToMove());
        const Move* found = nullptr;
        for (const Move& m : legal) {
            if (sameMove(m, entry.best)) {
                found = &m;
                break;
            }
        }
        if (!found) break;

        pv.push_back(*found);
        board.makeMove(*found);
    }
    return pv;
}

std::string formatUciMove(const Move& m) {
    std::string s;
    s += static_cast<char>('a' + m.fromX);
    s += static_cast<char>('1' + m.fromY);
    s += static_cast<char>('a' + m.toX);
    s += static_cast<char>('1' + m.toY);
    switch (m.promotion) {
        case PieceType::Queen: s += 'q'; break;
        case PieceType::Rook: s += 'r'; break;
        case PieceType::Bishop: s += 'b'; break;
        case PieceType::Knight: s += 'n'; break;
        default: break;
    }
    return s;
}

std::string formatUciMoveList(const std::vector<Move>& moves) {
    std::string s;
    for (size_t i = 0; i < moves.size(); i++) {
        if (i) s += ' ';
        s += formatUciMove(moves[i]);
    }
    return s;
}

std::string formatUciScore(int score) {
    if (score > kMateThreshold) {
        int pliesToMate = kMateScore - score;
        return "mate " + std::to_string((pliesToMate + 1) / 2);
    }
    if (score < -kMateThreshold) {
        int pliesToMate = kMateScore + score;
        return "mate -" + std::to_string((pliesToMate + 1) / 2);
    }
    return "cp " + std::to_string(score);
}

BenchResult benchSearch(const Board& board, Color color, int depth) {
    // searchRootFull() (below) is outside Step 3's scope and still takes a
    // std::vector<Move>&, so convert once here at the root (not a hot path).
    MoveList rootMoveList = board.legalMoves(color);
    std::vector<Move> rootMoves;
    for (const Move& m : rootMoveList) rootMoves.push_back(m);
    BenchResult result;
    if (rootMoves.empty()) return result;

    Board local = board;
    TranspositionTable tt(kTTSize);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(24);
    const auto start = std::chrono::steady_clock::now();
    RootSearchResult r = searchRootFull(local, color, rootMoves, depth, deadline, tt, 0);
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    result.move = r.move;
    result.score = r.score;
    result.nodes = r.nodes;
    return result;
}

Move findBestMove(const Board& board, Color aiColor, int maxDepth, int timeBudgetMs, TranspositionTable& tt,
                   const SearchOptions& options) {
    if (options.useOpeningBook) {
        std::vector<BookEntry> book = lookupBook(board);
        static std::mt19937 rng(std::random_device{}());
        if (!book.empty()) {
            // Cheap, unconditional safety net: even though hashKey() is a
            // complete encoding of position state (verified: pieces, side to
            // move, all four castling rights, en passant target), a book move
            // is only ever trustworthy if it's actually legal right now. Falls
            // through to normal search instead of returning a stale/bogus move
            // on any hash collision or book/board mismatch.
            MoveList legal = board.legalMoves(aiColor);
            std::vector<BookEntry> legalBookMoves;
            for (const BookEntry& be : book) {
                for (const Move& lm : legal) {
                    if (be.move.fromX == lm.fromX && be.move.fromY == lm.fromY && be.move.toX == lm.toX &&
                        be.move.toY == lm.toY && be.move.promotion == lm.promotion) {
                        legalBookMoves.push_back({lm, be.weight});
                        break;
                    }
                }
            }
            if (!legalBookMoves.empty()) {
                if (aiColor == Color::Black) {
                    // Rather than picking any known reply at random, play the
                    // most theoretically established counter to whatever White
                    // just did -- only tying among equally-weighted options
                    // falls back to randomness, for some variety.
                    int bestWeight = legalBookMoves.front().weight;
                    for (const BookEntry& be : legalBookMoves) bestWeight = std::max(bestWeight, be.weight);
                    std::vector<Move> bestReplies;
                    for (const BookEntry& be : legalBookMoves) {
                        if (be.weight == bestWeight) bestReplies.push_back(be.move);
                    }
                    std::uniform_int_distribution<size_t> dist(0, bestReplies.size() - 1);
                    return bestReplies[dist(rng)];
                }
                // White varies too, but only among moves within 10 points of the
                // best-weighted one (a weight-62 gambit is not as good a choice
                // as a weight-95 main line).
                int bestWeight = legalBookMoves.front().weight;
                for (const BookEntry& be : legalBookMoves) bestWeight = std::max(bestWeight, be.weight);
                std::vector<Move> choices;
                for (const BookEntry& be : legalBookMoves) {
                    if (be.weight >= bestWeight - 10) choices.push_back(be.move);
                }
                std::uniform_int_distribution<size_t> dist(0, choices.size() - 1);
                return choices[dist(rng)];
            }
        }
    }

    // searchRootFull() is outside Step 3's scope and still takes a
    // std::vector<Move>&, so convert once here at the root (not a hot path).
    MoveList rootMoveList = board.legalMoves(aiColor);
    std::vector<Move> rootMoves;
    for (const Move& m : rootMoveList) rootMoves.push_back(m);
    if (rootMoves.empty()) return Move{}; // checkmate or stalemate: the caller should not have asked
    if (!options.searchMoves.empty()) {
        std::vector<Move> allowed;
        for (const Move& m : rootMoves)
            for (const Move& wanted : options.searchMoves)
                if (sameMove(m, wanted)) {
                    allowed.push_back(m);
                    break;
                }
        if (!allowed.empty()) rootMoves = allowed;
    }
    // The only legal move needs no search -- unless the caller restricted the moves and wants it scored
    // (MultiPV asks for the last of its lines this way).
    if (rootMoves.size() == 1 && options.searchMoves.empty()) return rootMoves.front();

    if (g_aiDebugLog) {
        std::fprintf(stderr, "[ai] findBestMove: color=%s maxDepth=%d timeBudgetMs=%d rootMoves=%zu\n",
                     aiColor == Color::White ? "White" : "Black", maxDepth, timeBudgetMs, rootMoves.size());
    }

    // Endgame positions have far fewer pieces and thus a much lower
    // branching factor, so the same time budget can afford looking several
    // plies further — exactly where accurate defense/king activity matters
    // most. Difficulty presets otherwise apply the same depth cap regardless
    // of material left on the board.
    double phase = board.gamePhase();
    int effectiveMaxDepth = maxDepth;
    if (options.endgameDepthBonus) {
        if (phase < 0.5) effectiveMaxDepth += 2;
        if (phase < 0.2) effectiveMaxDepth += 2;
    }

    if (g_aiDebugLog) {
        std::fprintf(stderr, "[ai] gamePhase=%.3f effectiveMaxDepth=%d\n", phase, effectiveMaxDepth);
    }

    tt.newSearch();
    auto now = std::chrono::steady_clock::now();
    auto deadline = now + std::chrono::milliseconds(timeBudgetMs);
    auto softDeadline = options.softTimeMs > 0 ? now + std::chrono::milliseconds(options.softTimeMs)
                                               : std::chrono::steady_clock::time_point::max();

    // Use about half the machine's hardware threads (e.g. 12 of 24), leaving
    // headroom for the OS/UI thread. Every thread searches the FULL root
    // move list (Lazy-SMP), sharing one transposition table, so there's no
    // reason to cap thread count by the number of root moves anymore — that
    // cap only made sense when each thread owned a disjoint, non-empty
    // bucket to search. options.threadOverride > 0 (UCI's Threads option,
    // or a test harness pinning Threads=1) takes this over completely.
    size_t numThreads;
    if (options.threadOverride > 0) {
        numThreads = static_cast<size_t>(options.threadOverride);
    } else {
        int hwThreads = static_cast<int>(std::thread::hardware_concurrency());
        numThreads = static_cast<size_t>(std::max(1, hwThreads - 2));
    }

    if (g_aiDebugLog) {
        std::fprintf(stderr, "[ai] numThreads=%zu rootMoves: %s\n", numThreads, movesStr(rootMoves).c_str());
    }

    if (g_persistedOwner != tt.instanceId()) {
        g_persisted.clear();
        g_persistedOwner = tt.instanceId();
    }
    while (g_persisted.size() < numThreads) g_persisted.push_back(std::make_unique<PersistedHeuristics>());

    std::vector<std::atomic<long long>> nodeSlots(numThreads); // each thread's node count, for the total in "info"
    for (auto& slot : nodeSlots) slot.store(0, std::memory_order_relaxed);
    const long long nodeLimit = options.nodeLimit > 0 ? options.nodeLimit : std::numeric_limits<long long>::max();
    // Thread 0 decides the move (and, through its soft limit, when to stop). When it is done the helpers have
    // nothing left to contribute, so it tells them to stop instead of leaving them to run on by their own
    // clocks, which only the hard limit ends.
    std::atomic<bool> halt{false};

    std::vector<std::future<RootSearchResult>> futures;
    futures.reserve(numThreads);
    for (size_t t = 0; t < numThreads; t++) {
        // Each thread needs its own mutable board to make/unmake moves into,
        // so capture a fresh non-const copy per thread rather than the
        // outer `const Board&` (capturing that by value would still carry
        // the const through into the closure, `mutable` notwithstanding).
        Board threadBoard = board;
        const std::atomic<bool>* stopFlag = options.stopFlag;
        // Progress callback only from thread 0 -- the deterministic,
        // unperturbed-ordering thread (see searchRootFull's jitter comment)
        // -- so a caller reporting "the" search's progress gets one
        // consistent stream instead of interleaved reports from N threads.
        std::function<void(const SearchProgress&)> progressCb = (t == 0) ? options.onDepthComplete
                                                                           : std::function<void(const SearchProgress&)>{};
        futures.push_back(std::async(
            std::launch::async,
            [threadBoard, aiColor, rootMoves, effectiveMaxDepth, deadline, softDeadline, &tt, t, stopFlag, progressCb,
             &gameHistory = options.gameHistory, persisted = g_persisted[t].get(), nodeLimit,
             slots = nodeSlots.data(), numSlots = nodeSlots.size(), &halt]() mutable {
                RootSearchResult r =
                    searchRootFull(threadBoard, aiColor, rootMoves, effectiveMaxDepth, deadline, tt, t, stopFlag,
                                   progressCb, gameHistory, softDeadline, persisted, nodeLimit, slots, numSlots, &halt);
                if (t == 0) halt.store(true, std::memory_order_relaxed);
                return r;
            }));
    }

    std::vector<RootSearchResult> results;
    results.reserve(futures.size());
    for (size_t t = 0; t < futures.size(); t++) {
        RootSearchResult r = futures[t].get();
        if (g_aiDebugLog) {
            std::fprintf(stderr, "[ai]   thread[%zu] depthReached=%d nodes=%lld score=%d move=%s\n", t,
                         r.depthReached, r.nodes, r.score, moveStr(r.move).c_str());
        }
        results.push_back(r);
    }

    // Thread 0 -- the one with unperturbed move ordering -- decides. The
    // helper threads' job is filling the shared transposition table with
    // results thread 0 then finds there instead of searching them itself.
    const RootSearchResult& main = results.front();
    if (g_aiDebugLog && main.depthReached == 0) {
        std::fprintf(stderr, "[ai] FALLBACK: thread 0 completed no depth; returning rootMoves.front()=%s\n",
                     moveStr(rootMoves.front()).c_str());
    }
    return main.depthReached > 0 ? main.move : rootMoves.front();
}
