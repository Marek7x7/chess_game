#pragma once

#include <cstdint>

// The shape of the neural-network evaluation, shared by the engine (src/nnue.cpp) and the trainer
// (tools/nnue/train.cpp), so that the two cannot drift apart.
//
// A perspective network: the position is described twice, once from each side's point of view (own pieces
// first, the board flipped vertically for Black), as 768 one-hot inputs
// (own/enemy x 6 piece types x 64 squares). Each description is summed into a vector of kHidden numbers (the
// accumulator, kept up to date move by move), clipped to [0, 1] and squared (SCReLU), and the vector of the
// side to move followed by the one of the other side is weighed into one output, from a set of weights that
// depends on how many pieces are left.
//
//   out = bias[b] + sum_i  screlu(acc_stm[i]) * w[b][i]  +  sum_i  screlu(acc_other[i]) * w[b][kHidden + i]
//   centipawns = out * kScale
//
// In the engine everything is integers: accumulator and feature weights are scaled by kQA, output weights by
// kQB, the output bias by kQA * kQB.
namespace nnue {

// King-aware inputs (build with -DCHESS_NNUE_KB=8; 1, the default, is the plain 768 inputs). Each side's view
// is then indexed by where THAT side's own king stands: the board is mirrored left-right when that king is on
// files e-h (so the king is always on files a-d), and the king square picks one of 8 regions (rank 1 / rank 2 /
// ranks 3-4 / ranks 5-8, times files a-b / c-d), each with its own copy of the 768 piece-square inputs. The network
// then learns piece placement relative to the king. A king move that changes region or mirror rebuilds that side's
// accumulator from scratch (see Board::makeMove).
#ifndef CHESS_NNUE_KB
#define CHESS_NNUE_KB 8
#endif
constexpr int kKingBuckets = CHESS_NNUE_KB;
static_assert(kKingBuckets == 1 || kKingBuckets == 8, "king buckets: 1 or 8");
constexpr int kPieceInputs = 768;
constexpr int kInputs = kPieceInputs * kKingBuckets;
#ifndef CHESS_NNUE_HIDDEN
#define CHESS_NNUE_HIDDEN 512 // (256, 512 or 768; the network file must be trained at the same width)
#endif
constexpr int kHidden = CHESS_NNUE_HIDDEN;
constexpr int kBuckets = 8; // output weight sets, by piece count
constexpr int kQA = 255;
constexpr int kQB = 64;
constexpr int kScale = 400;

// An optional second layer (build with -DCHESS_NNUE_L2=16; 0, the default, is the single-layer network above).
// With it, per bucket:  x = screlu of both accumulators (512 values, stm first), z = b1 + W1 x, h = screlu(z)
// (16 values), out = b2 + w2 . h.  Integers: y = (clip(a, 0, kQA)^2) >> 8 (unsigned 8 bit, scale kY = 254), W1 and
// w2 are int8 at scale kQB, so z has scale kZ = kY * kQB; h = (clip(z, 0, kZ)^2) >> 13 has scale kH = 2 * 127^2.
#ifndef CHESS_NNUE_L2
#define CHESS_NNUE_L2 0
#endif
constexpr int kL2 = CHESS_NNUE_L2;
constexpr int kY = 254;
constexpr int kZ = kY * kQB;
constexpr int kH = 32258; // (kZ * kZ) >> 13, exactly
static_assert(kL2 == 0 || (int64_t(kZ) * kZ) >> 13 == kH, "kH");

// A piece of colour `pieceColor` (0 White, 1 Black) and type `type` (1 Pawn .. 6 King) on file x, rank y (0..7),
// seen by the side `perspective` (0 White, 1 Black).
// `ctx` is the side's king context (kingContext below; 0 without king buckets).
constexpr int featureIndex(int perspective, int pieceColor, int type, int x, int y, int ctx = 0) {
    const int xm = (ctx & 1) ? 7 - x : x;
    return (ctx >> 1) * kPieceInputs + ((pieceColor == perspective ? 0 : 1) * 6 + (type - 1)) * 64 + xm * 8 +
           (perspective == 0 ? y : 7 - y);
}

// The king context of the side `perspective` (0 White, 1 Black) whose own king stands on file kx, rank ky:
// region * 2 + (1 if the board is mirrored left-right). Always 0 without king buckets.
constexpr int kingContext(int perspective, int kx, int ky) {
    if (kKingBuckets == 1) return 0;
    const bool mirror = kx >= 4;
    const int fx = mirror ? 7 - kx : kx;
    const int ry = perspective == 0 ? ky : 7 - ky;
    const int rankGroup = ry == 0 ? 0 : (ry == 1 ? 1 : (ry <= 3 ? 2 : 3));
    return (rankGroup * 2 + (fx < 2 ? 0 : 1)) * 2 + (mirror ? 1 : 0);
}

// Which set of output weights to use, from the number of pieces on the board (2..32).
constexpr int bucketOf(int pieceCount) {
    int b = (pieceCount - 2) / 4;
    return b < 0 ? 0 : (b >= kBuckets ? kBuckets - 1 : b);
}

// The network file: this header, then the arrays below in this order, all little-endian.
struct FileHeader {
    char magic[4];       // "CGNN"
    uint32_t version;    // 1
    uint32_t inputs;     // kInputs
    uint32_t hidden;     // kHidden
    uint32_t buckets;    // kBuckets
    uint32_t qa, qb;     // kQA, kQB
    uint32_t scale;      // kScale
};
constexpr uint32_t kFileVersion = kL2 == 0 ? 1 : 2;
// int16 featureWeights[kInputs][kHidden]; int16 featureBias[kHidden];
// int16 outputWeights[kBuckets][2 * kHidden]; int32 outputBias[kBuckets];
// With kL2 > 0 the output weights are replaced by, per bucket: int8 W1[kL2][2 * kHidden]; int32 b1[kL2];
// int8 w2[kL2]; int32 b2 (b1 at scale kZ, b2 at scale kH * kQB).
constexpr uint64_t kOutputBytes = kL2 == 0 ? uint64_t(kBuckets) * 2 * kHidden * 2 + kBuckets * 4
                                           : uint64_t(kBuckets) * (kL2 * 2 * kHidden + kL2 * 4 + kL2 + 4);
constexpr uint64_t kFileBytes = sizeof(FileHeader) + uint64_t(kInputs) * kHidden * 2 + kHidden * 2 + kOutputBytes;

} // namespace nnue
