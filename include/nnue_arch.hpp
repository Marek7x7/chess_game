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

constexpr int kInputs = 768;
constexpr int kHidden = 256;
constexpr int kBuckets = 8; // output weight sets, by piece count
constexpr int kQA = 255;
constexpr int kQB = 64;
constexpr int kScale = 400;

// A piece of colour `pieceColor` (0 White, 1 Black) and type `type` (1 Pawn .. 6 King) on file x, rank y (0..7),
// seen by the side `perspective` (0 White, 1 Black).
constexpr int featureIndex(int perspective, int pieceColor, int type, int x, int y) {
    return ((pieceColor == perspective ? 0 : 1) * 6 + (type - 1)) * 64 + x * 8 + (perspective == 0 ? y : 7 - y);
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
constexpr uint32_t kFileVersion = 1;
// int16 featureWeights[kInputs][kHidden]; int16 featureBias[kHidden];
// int16 outputWeights[kBuckets][2 * kHidden]; int32 outputBias[kBuckets];
constexpr uint64_t kFileBytes = sizeof(FileHeader) + uint64_t(kInputs) * kHidden * 2 + kHidden * 2 +
                                uint64_t(kBuckets) * 2 * kHidden * 2 + kBuckets * 4;

} // namespace nnue
