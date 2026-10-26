// Trains the evaluation network (nnue_arch.hpp) on records made by nnue_data (or the engine's datagen mode).
//
//   nnue_train train <data.bin>... --out net.nnue [--epochs 20] [--batch 16384] [--lr 1e-3] [--lambda 0.7]
//                    [--k 173.7] [--min-depth N] [--min-ply N] [--max-score N] [--init net.nnue] [--seed N]
//   nnue_train eval  <data.bin>... (--net net.nnue | --hce) [--valid-only] [--k 173.7] [same filters]
//
// The loss is the mean squared error between the predicted and the target win probability, where
//   predicted = sigmoid(output * kScale / K)             (output in the units of the network's last layer)
//   target    = lambda * sigmoid(score / K) + (1 - lambda) * (result + 1) / 2
// so the network's centipawns mean the same as the hand-crafted evaluation's, whose weights were fitted with
// K = 400 / ln 10 = 173.7. About 5% of the data (blocks of 4096 consecutive records, which keeps whole games
// together) is held out and reported after every epoch. Training is plain mini-batch Adam; every batch is split
// over the OpenMP threads, each with its own gradient buffer.
//
// `eval` scores a data set with a network file through the engine's own integer code (the same as the engine
// uses), or with the hand-crafted evaluation, and prints the same two losses: a like-for-like comparison.
#include <omp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "board.hpp"
#include "nnue.hpp"
#include "nnue_record.hpp"

int tunerEvaluate(const Board& board); // the hand-crafted evaluation, White's point of view (needs -DTUNING)

namespace {

using nnue::data::Record;
constexpr int H = nnue::kHidden;
constexpr int kBlock = 4096;

struct Options {
    std::vector<std::string> inputs;
    std::string out, init, net;
    bool hce = false, validOnly = false;
    int epochs = 20, batch = 16384, seed = 1;
    double lr = 1e-3, lambda = 0.7, k = 400.0 / std::log(10.0);
    int minDepth = 0, minPly = 0, maxScore = 3000;
};

bool isValidationBlock(size_t block) {
    uint64_t x = block * 0x9E3779B97F4A7C15ULL;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 32;
    return x % 20 == 0;
}

float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

// All parameters in one array so Adam can treat them alike.
struct Layout {
    static constexpr size_t ftW = 0;
    static constexpr size_t ftB = ftW + size_t(nnue::kInputs) * H;
    static constexpr size_t ow = ftB + H;
    static constexpr size_t ob = ow + size_t(nnue::kBuckets) * 2 * H;
    // second layer (nnue::kL2 > 0; the single-layer output weights above are then unused)
    static constexpr size_t L = nnue::kL2;
    static constexpr size_t w1 = ob + nnue::kBuckets;
    static constexpr size_t b1 = w1 + size_t(nnue::kBuckets) * L * 2 * H;
    static constexpr size_t w2 = b1 + size_t(nnue::kBuckets) * L;
    static constexpr size_t b2 = w2 + size_t(nnue::kBuckets) * L;
    static constexpr size_t total = b2 + nnue::kBuckets;
};

struct Active {
    int n[2];
    int f[2][32];
};

// The input features of a record from both sides' points of view.
inline void features(const Record& r, Active& a) {
    a.n[0] = a.n[1] = 0;
    nnue::data::forEachPiece(r, [&](int sq, int color, int type) {
        const int x = sq / 8, y = sq % 8;
        for (int p = 0; p < 2; p++) a.f[p][a.n[p]++] = nnue::featureIndex(p, color, type, x, y);
    });
}

struct Forward {
    float h[2][H];
    float out;
    int bucket;
    float x[2 * H]; // second layer: screlu of both accumulators, side to move first
    float z[Layout::L > 0 ? Layout::L : 1];
};

inline void forward(const std::vector<float>& th, const Record& r, const Active& a, Forward& fw) {
    const float* ftW = th.data() + Layout::ftW;
    const float* ftB = th.data() + Layout::ftB;
    for (int p = 0; p < 2; p++) {
        float* h = fw.h[p];
        for (int i = 0; i < H; i++) h[i] = ftB[i];
        for (int j = 0; j < a.n[p]; j++) {
            const float* w = ftW + size_t(a.f[p][j]) * H;
            for (int i = 0; i < H; i++) h[i] += w[i];
        }
    }
    fw.bucket = nnue::bucketOf(nnue::data::pieceCount(r));
    const int us = r.stm, them = 1 - r.stm;
    if constexpr (Layout::L > 0) {
        for (int i = 0; i < H; i++) {
            const float a1 = std::min(std::max(fw.h[us][i], 0.0f), 1.0f);
            const float a2 = std::min(std::max(fw.h[them][i], 0.0f), 1.0f);
            fw.x[i] = a1 * a1;
            fw.x[H + i] = a2 * a2;
        }
        float out = th[Layout::b2 + fw.bucket];
        for (int j = 0; j < nnue::kL2; j++) {
            const float* w = th.data() + Layout::w1 + (size_t(fw.bucket) * Layout::L + j) * 2 * H;
            float z = th[Layout::b1 + size_t(fw.bucket) * Layout::L + j];
            for (int i = 0; i < 2 * H; i++) z += w[i] * fw.x[i];
            fw.z[j] = z;
            const float c = std::min(std::max(z, 0.0f), 1.0f);
            out += th[Layout::w2 + size_t(fw.bucket) * Layout::L + j] * c * c;
        }
        fw.out = out;
        return;
    }
    const float* ow = th.data() + Layout::ow + size_t(fw.bucket) * 2 * H;
    float out = th[Layout::ob + fw.bucket];
    for (int i = 0; i < H; i++) {
        const float a1 = std::min(std::max(fw.h[us][i], 0.0f), 1.0f);
        const float a2 = std::min(std::max(fw.h[them][i], 0.0f), 1.0f);
        out += a1 * a1 * ow[i] + a2 * a2 * ow[H + i];
    }
    fw.out = out;
}

float targetOf(const Record& r, const Options& o) {
    const float fromScore = sigmoid(static_cast<float>(r.score) / static_cast<float>(o.k));
    const float fromResult = 0.5f * (static_cast<float>(r.result) + 1.0f);
    return static_cast<float>(o.lambda) * fromScore + (1.0f - static_cast<float>(o.lambda)) * fromResult;
}

bool keep(const Record& r, const Options& o) {
    return r.depth >= o.minDepth && r.ply >= o.minPly && std::abs(r.score) <= o.maxScore;
}

bool loadData(const Options& o, std::vector<Record>& train, std::vector<Record>& valid) {
    size_t block = 0, total = 0;
    for (const std::string& path : o.inputs) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", path.c_str());
            return false;
        }
        std::vector<Record> buf(kBlock);
        size_t got;
        while ((got = std::fread(buf.data(), sizeof(Record), kBlock, f)) > 0) {
            auto& dst = isValidationBlock(block) ? valid : train;
            for (size_t i = 0; i < got; i++)
                if (keep(buf[i], o)) dst.push_back(buf[i]);
            total += got;
            block++;
        }
        std::fclose(f);
    }
    std::printf("%zu records read, %zu for training, %zu held out\n", total, train.size(), valid.size());
    return !train.empty();
}

// Mean squared loss of the network under training on a data set (no gradients).
double evaluateLoss(const std::vector<float>& th, const std::vector<Record>& data, const Options& o, double* lossResult) {
    double sum = 0, sumResult = 0;
    const float scaleOut = nnue::kScale / static_cast<float>(o.k);
#pragma omp parallel for reduction(+ : sum, sumResult) schedule(static)
    for (size_t i = 0; i < data.size(); i++) {
        Active a;
        Forward fw;
        features(data[i], a);
        forward(th, data[i], a, fw);
        const float pred = sigmoid(fw.out * scaleOut);
        const float e = pred - targetOf(data[i], o);
        const float eResult = pred - 0.5f * (static_cast<float>(data[i].result) + 1.0f);
        sum += e * e;
        sumResult += eResult * eResult;
    }
    if (lossResult) *lossResult = data.empty() ? 0 : sumResult / data.size();
    return data.empty() ? 0 : sum / data.size();
}

// ---- export -----------------------------------------------------------------------------------------------

int16_t q16(float v, float scale) {
    const long r = std::lround(v * scale);
    return static_cast<int16_t>(std::min(32767L, std::max(-32768L, r)));
}

bool writeNet(const std::vector<float>& th, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    nnue::FileHeader h;
    std::memcpy(h.magic, "CGNN", 4);
    h.version = nnue::kFileVersion;
    h.inputs = nnue::kInputs;
    h.hidden = nnue::kHidden;
    h.buckets = nnue::kBuckets;
    h.qa = nnue::kQA;
    h.qb = nnue::kQB;
    h.scale = nnue::kScale;
    std::fwrite(&h, sizeof h, 1, f);
    std::vector<int16_t> w(size_t(nnue::kInputs) * H);
    for (size_t i = 0; i < w.size(); i++) w[i] = q16(th[Layout::ftW + i], nnue::kQA);
    std::fwrite(w.data(), 2, w.size(), f);
    std::vector<int16_t> b(H);
    for (int i = 0; i < H; i++) b[i] = q16(th[Layout::ftB + i], nnue::kQA);
    std::fwrite(b.data(), 2, b.size(), f);
    if constexpr (Layout::L > 0) {
        auto q8 = [](float v) { return static_cast<int8_t>(std::min<long>(127, std::max<long>(-127, std::lround(v * nnue::kQB)))); };
        for (int bk = 0; bk < nnue::kBuckets; bk++) {
            std::vector<int8_t> w1(Layout::L * 2 * H), w2(Layout::L);
            std::vector<int32_t> b1(Layout::L);
            for (size_t i = 0; i < w1.size(); i++) w1[i] = q8(th[Layout::w1 + size_t(bk) * w1.size() + i]);
            for (int j = 0; j < nnue::kL2; j++) {
                b1[j] = static_cast<int32_t>(std::lround(th[Layout::b1 + size_t(bk) * Layout::L + j] * nnue::kZ));
                w2[j] = q8(th[Layout::w2 + size_t(bk) * Layout::L + j]);
            }
            const int32_t b2 = static_cast<int32_t>(std::lround(th[Layout::b2 + bk] * nnue::kH * nnue::kQB));
            std::fwrite(w1.data(), 1, w1.size(), f);
            std::fwrite(b1.data(), 4, b1.size(), f);
            std::fwrite(w2.data(), 1, w2.size(), f);
            std::fwrite(&b2, 4, 1, f);
        }
        std::fclose(f);
        return true;
    }
    std::vector<int16_t> ow(size_t(nnue::kBuckets) * 2 * H);
    for (size_t i = 0; i < ow.size(); i++) {
        // |w| <= 127 / 64 keeps (a * w) inside 16 bits for a <= 255, which the vector code relies on
        ow[i] = static_cast<int16_t>(std::min<int>(127, std::max<int>(-127, q16(th[Layout::ow + i], nnue::kQB))));
    }
    std::fwrite(ow.data(), 2, ow.size(), f);
    std::vector<int32_t> ob(nnue::kBuckets);
    for (int i = 0; i < nnue::kBuckets; i++)
        ob[i] = static_cast<int32_t>(std::lround(th[Layout::ob + i] * nnue::kQA * nnue::kQB));
    std::fwrite(ob.data(), 4, ob.size(), f);
    std::fclose(f);
    return true;
}

bool readNet(const std::string& path, std::vector<float>& th) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    nnue::FileHeader h;
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, "CGNN", 4) == 0 && h.hidden == nnue::kHidden &&
              h.inputs == nnue::kInputs && h.buckets == nnue::kBuckets;
    th.assign(Layout::total, 0.0f);
    if constexpr (Layout::L > 0) {
        std::vector<int16_t> w(size_t(nnue::kInputs) * H), b(H);
        ok = ok && std::fread(w.data(), 2, w.size(), f) == w.size() && std::fread(b.data(), 2, b.size(), f) == b.size();
        for (size_t i = 0; ok && i < w.size(); i++) th[Layout::ftW + i] = w[i] / float(nnue::kQA);
        for (int i = 0; ok && i < H; i++) th[Layout::ftB + i] = b[i] / float(nnue::kQA);
        for (int bk = 0; ok && bk < nnue::kBuckets; bk++) {
            std::vector<int8_t> w1(Layout::L * 2 * H), w2(Layout::L);
            std::vector<int32_t> b1(Layout::L);
            int32_t b2 = 0;
            ok = std::fread(w1.data(), 1, w1.size(), f) == w1.size() && std::fread(b1.data(), 4, b1.size(), f) == b1.size() &&
                 std::fread(w2.data(), 1, w2.size(), f) == w2.size() && std::fread(&b2, 4, 1, f) == 1;
            for (size_t i = 0; ok && i < w1.size(); i++) th[Layout::w1 + size_t(bk) * w1.size() + i] = w1[i] / float(nnue::kQB);
            for (int j = 0; ok && j < nnue::kL2; j++) {
                th[Layout::b1 + size_t(bk) * Layout::L + j] = b1[j] / float(nnue::kZ);
                th[Layout::w2 + size_t(bk) * Layout::L + j] = w2[j] / float(nnue::kQB);
            }
            if (ok) th[Layout::b2 + bk] = b2 / float(nnue::kH * nnue::kQB);
        }
        std::fclose(f);
        return ok;
    }
    if (ok) {
        std::vector<int16_t> w(size_t(nnue::kInputs) * H), b(H), ow(size_t(nnue::kBuckets) * 2 * H);
        std::vector<int32_t> ob(nnue::kBuckets);
        ok = std::fread(w.data(), 2, w.size(), f) == w.size() && std::fread(b.data(), 2, b.size(), f) == b.size() &&
             std::fread(ow.data(), 2, ow.size(), f) == ow.size() && std::fread(ob.data(), 4, ob.size(), f) == ob.size();
        for (size_t i = 0; ok && i < w.size(); i++) th[Layout::ftW + i] = w[i] / float(nnue::kQA);
        for (int i = 0; ok && i < H; i++) th[Layout::ftB + i] = b[i] / float(nnue::kQA);
        for (size_t i = 0; ok && i < ow.size(); i++) th[Layout::ow + i] = ow[i] / float(nnue::kQB);
        for (int i = 0; ok && i < nnue::kBuckets; i++) th[Layout::ob + i] = ob[i] / float(nnue::kQA * nnue::kQB);
    }
    std::fclose(f);
    return ok;
}

// ---- training ---------------------------------------------------------------------------------------------

int train(Options o) { // (by value: the batch size may be lowered)
    std::vector<Record> data, valid;
    if (!loadData(o, data, valid)) return 1;

    std::vector<float> th(Layout::total, 0.0f);
    if (!o.init.empty()) {
        if (!readNet(o.init, th)) {
            std::fprintf(stderr, "cannot read the network %s\n", o.init.c_str());
            return 1;
        }
        std::printf("continuing from %s\n", o.init.c_str());
    } else {
        std::mt19937 rng(static_cast<unsigned>(o.seed));
        std::normal_distribution<float> gw(0.0f, 0.10f), go(0.0f, 0.05f);
        for (size_t i = 0; i < size_t(nnue::kInputs) * H; i++) th[Layout::ftW + i] = gw(rng);
        for (int i = 0; i < H; i++) th[Layout::ftB + i] = 0.5f;
        for (size_t i = 0; i < size_t(nnue::kBuckets) * 2 * H; i++) th[Layout::ow + i] = go(rng);
        if constexpr (Layout::L > 0) {
            std::normal_distribution<float> g1(0.0f, 0.05f);
            for (size_t i = 0; i < size_t(nnue::kBuckets) * Layout::L * 2 * H; i++) th[Layout::w1 + i] = g1(rng);
            for (size_t i = 0; i < size_t(nnue::kBuckets) * Layout::L; i++) {
                th[Layout::b1 + i] = 0.3f;
                th[Layout::w2 + i] = go(rng);
            }
        }
    }
    std::vector<float> m(Layout::total, 0.0f), v(Layout::total, 0.0f);
    const int threads = omp_get_max_threads();
    std::vector<std::vector<float>> grads(threads, std::vector<float>(Layout::total, 0.0f));
    std::vector<float> g(Layout::total);
    std::vector<size_t> order(data.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::mt19937_64 shuffler(static_cast<uint64_t>(o.seed) * 7919u);

    if (data.size() < static_cast<size_t>(o.batch)) { // fewer positions than one batch: a batch is all of them
        std::printf("only %zu training positions: batch size %d lowered to match\n", data.size(), o.batch);
        o.batch = static_cast<int>(data.size());
    }
    const size_t batchesPerEpoch = std::max<size_t>(1, data.size() / static_cast<size_t>(o.batch));
    const long long totalSteps = static_cast<long long>(batchesPerEpoch) * o.epochs;
    const float scaleOut = nnue::kScale / static_cast<float>(o.k);
    long long step = 0;
    std::printf("training on %d threads: %d epochs of %zu batches of %d\n", threads, o.epochs, batchesPerEpoch, o.batch);
    double validResult = 0;
    std::printf("epoch 0: validation loss %.6f (against the result alone %.6f)\n", evaluateLoss(th, valid, o, &validResult), validResult);

    for (int epoch = 1; epoch <= o.epochs; epoch++) {
        const auto t0 = std::chrono::steady_clock::now();
        std::shuffle(order.begin(), order.end(), shuffler);
        double epochLoss = 0;
        for (size_t b = 0; b < batchesPerEpoch; b++, step++) {
            const size_t first = b * static_cast<size_t>(o.batch);
            double batchLoss = 0;
#pragma omp parallel num_threads(threads) reduction(+ : batchLoss)
            {
                float* grad = grads[omp_get_thread_num()].data();
                std::memset(grad, 0, Layout::total * sizeof(float));
                Active a;
                Forward fw;
                float dh[2][H];
#pragma omp for schedule(static)
                for (long long s = 0; s < o.batch; s++) {
                    const Record& r = data[order[first + static_cast<size_t>(s)]];
                    features(r, a);
                    forward(th, r, a, fw);
                    const float pred = sigmoid(fw.out * scaleOut);
                    const float err = pred - targetOf(r, o);
                    batchLoss += err * err;
                    const float dout = 2.0f * err * pred * (1.0f - pred) * scaleOut;
                    const int us = r.stm, them = 1 - r.stm;
                    if constexpr (Layout::L > 0) {
                        float dx[2 * H] = {};
                        grad[Layout::b2 + fw.bucket] += dout;
                        for (int j = 0; j < nnue::kL2; j++) {
                            const size_t jb = size_t(fw.bucket) * Layout::L + j;
                            const float z = fw.z[j], c = std::min(std::max(z, 0.0f), 1.0f);
                            grad[Layout::w2 + jb] += dout * c * c;
                            if (!(z > 0.0f && z < 1.0f)) continue;
                            const float dz = dout * th[Layout::w2 + jb] * 2.0f * c;
                            grad[Layout::b1 + jb] += dz;
                            const float* w = th.data() + Layout::w1 + jb * 2 * H;
                            float* gw1 = grad + Layout::w1 + jb * 2 * H;
                            for (int i = 0; i < 2 * H; i++) {
                                gw1[i] += dz * fw.x[i];
                                dx[i] += dz * w[i];
                            }
                        }
                        for (int i = 0; i < H; i++) {
                            const float hu = fw.h[us][i], ht = fw.h[them][i];
                            const float au = std::min(std::max(hu, 0.0f), 1.0f), at = std::min(std::max(ht, 0.0f), 1.0f);
                            dh[us][i] = (hu > 0.0f && hu < 1.0f) ? dx[i] * 2.0f * au : 0.0f;
                            dh[them][i] = (ht > 0.0f && ht < 1.0f) ? dx[H + i] * 2.0f * at : 0.0f;
                        }
                    } else {
                    const float* ow = th.data() + Layout::ow + size_t(fw.bucket) * 2 * H;
                    float* gow = grad + Layout::ow + size_t(fw.bucket) * 2 * H;
                    grad[Layout::ob + fw.bucket] += dout;
                    for (int i = 0; i < H; i++) {
                        const float hu = fw.h[us][i], ht = fw.h[them][i];
                        const float au = std::min(std::max(hu, 0.0f), 1.0f), at = std::min(std::max(ht, 0.0f), 1.0f);
                        gow[i] += dout * au * au;
                        gow[H + i] += dout * at * at;
                        dh[us][i] = (hu > 0.0f && hu < 1.0f) ? dout * ow[i] * 2.0f * au : 0.0f;
                        dh[them][i] = (ht > 0.0f && ht < 1.0f) ? dout * ow[H + i] * 2.0f * at : 0.0f;
                    }
                    }
                    float* gb = grad + Layout::ftB;
                    for (int i = 0; i < H; i++) gb[i] += dh[0][i] + dh[1][i];
                    for (int p = 0; p < 2; p++)
                        for (int j = 0; j < a.n[p]; j++) {
                            float* gw = grad + Layout::ftW + size_t(a.f[p][j]) * H;
                            for (int i = 0; i < H; i++) gw[i] += dh[p][i];
                        }
                }
            }
            epochLoss += batchLoss;

            // Adam on the summed gradients (divided by the batch size), cosine learning-rate decay.
            const double progress = static_cast<double>(step) / static_cast<double>(totalSteps);
            const float lr = static_cast<float>(o.lr * (0.01 + 0.99 * 0.5 * (1.0 + std::cos(3.14159265358979323846 * progress))));
            const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
            const float c1 = 1.0f - std::pow(b1, static_cast<float>(step + 1)), c2 = 1.0f - std::pow(b2, static_cast<float>(step + 1));
            const float inv = 1.0f / static_cast<float>(o.batch);
#pragma omp parallel for schedule(static)
            for (long long j = 0; j < static_cast<long long>(Layout::total); j++) {
                float sum = 0;
                for (int t = 0; t < threads; t++) sum += grads[t][static_cast<size_t>(j)];
                const float gj = sum * inv;
                m[j] = b1 * m[j] + (1 - b1) * gj;
                v[j] = b2 * v[j] + (1 - b2) * gj * gj;
                th[j] -= lr * (m[j] / c1) / (std::sqrt(v[j] / c2) + eps);
            }
            // The integer code needs these ranges (see writeNet).
            for (size_t j = Layout::ow; j < Layout::ob; j++) th[j] = std::min(1.98f, std::max(-1.98f, th[j]));
            if constexpr (Layout::L > 0) { // int8 weights
                for (size_t j = Layout::w1; j < Layout::b1; j++) th[j] = std::min(1.98f, std::max(-1.98f, th[j]));
                for (size_t j = Layout::w2; j < Layout::b2; j++) th[j] = std::min(1.98f, std::max(-1.98f, th[j]));
            }
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double trainLoss = epochLoss / static_cast<double>(batchesPerEpoch * static_cast<size_t>(o.batch));
        std::printf("epoch %d: training loss %.6f, validation loss %.6f (result alone %.6f), %.1f s (%.2f M positions/s)\n", epoch,
                    trainLoss, evaluateLoss(th, valid, o, &validResult), validResult, secs,
                    static_cast<double>(batchesPerEpoch * static_cast<size_t>(o.batch)) / secs / 1e6);
        std::fflush(stdout);
        if (!o.out.empty() && !writeNet(th, o.out)) {
            std::fprintf(stderr, "cannot write %s\n", o.out.c_str());
            return 1;
        }
    }
    if (!o.out.empty()) std::printf("wrote %s\n", o.out.c_str());

    // The exported integers against the float network they were made from.
    if (!o.out.empty()) {
        std::string error;
        if (!nnue::loadFile(o.out, error)) {
            std::fprintf(stderr, "exported network does not load: %s\n", error.c_str());
            return 1;
        }
        double sumAbs = 0, maxAbs = 0, sumAbsRounded = 0;
        const size_t n = std::min<size_t>(valid.size(), 200000);
        std::vector<float> rounded; // the exported weights read back as floats: isolates the integer arithmetic
        if (!readNet(o.out, rounded)) rounded = th;
        for (size_t i = 0; i < n; i++) {
            Active a;
            Forward fw, fwRounded;
            features(valid[i], a);
            forward(th, valid[i], a, fw);
            forward(rounded, valid[i], a, fwRounded);
            nnue::Accumulator acc;
            nnue::reset(acc);
            nnue::data::forEachPiece(valid[i], [&](int sq, int color, int type) {
                nnue::add(acc, color == 0 ? Color::White : Color::Black, static_cast<PieceType>(type), sq / 8, sq % 8);
            });
            const int q = nnue::evaluate(acc, valid[i].stm == 0 ? Color::White : Color::Black, nnue::data::pieceCount(valid[i]));
            const double d = std::abs(q - fw.out * nnue::kScale);
            sumAbs += d;
            maxAbs = std::max(maxAbs, d);
            sumAbsRounded += std::abs(q - fwRounded.out * nnue::kScale);
        }
        std::printf("quantization, %zu positions: integer evaluation vs the float network: mean |difference| %.2f cp, max %.1f cp;\n"
                    "  vs the float network with the rounded weights (the integer arithmetic alone): mean %.2f cp\n",
                    n, n ? sumAbs / n : 0.0, maxAbs, n ? sumAbsRounded / n : 0.0);
    }
    return 0;
}

// ---- eval -------------------------------------------------------------------------------------------------

int evaluateSet(const Options& o) {
    std::vector<Record> train, valid;
    if (!loadData(o, train, valid)) return 1;
    // By default everything counts: a network under test was not necessarily trained on this data. --valid-only
    // restricts it to the held-out blocks, which a network trained here by `train` never saw.
    std::vector<Record>& data = o.validOnly ? valid : train;
    if (!o.validOnly) data.insert(data.end(), valid.begin(), valid.end());
    std::string error;
    if (!o.hce && !nnue::loadFile(o.net, error)) {
        std::fprintf(stderr, "cannot load %s: %s\n", o.net.c_str(), error.c_str());
        return 1;
    }
    if (o.hce) nnue::unload();
    double sumScore = 0, sumResult = 0;
#pragma omp parallel for reduction(+ : sumScore, sumResult) schedule(static)
    for (size_t i = 0; i < data.size(); i++) {
        const Record& r = data[i];
        double cp;
        if (o.hce) {
            Board board;
            const bool ok = Board::tryFromFEN(nnue::data::toFen(r), board);
            if (!ok) continue;
            cp = tunerEvaluate(board);
            if (r.stm == 1) cp = -cp;
        } else {
            nnue::Accumulator acc;
            nnue::reset(acc);
            nnue::data::forEachPiece(r, [&](int sq, int color, int type) {
                nnue::add(acc, color == 0 ? Color::White : Color::Black, static_cast<PieceType>(type), sq / 8, sq % 8);
            });
            cp = nnue::evaluate(acc, r.stm == 0 ? Color::White : Color::Black, nnue::data::pieceCount(r));
        }
        const double pred = 1.0 / (1.0 + std::exp(-cp / o.k));
        const double dScore = pred - 1.0 / (1.0 + std::exp(-static_cast<double>(r.score) / o.k));
        const double dResult = pred - 0.5 * (r.result + 1.0);
        sumScore += dScore * dScore;
        sumResult += dResult * dResult;
    }
    std::printf("%zu positions, %s: loss against the search score %.6f, against the game result %.6f\n", data.size(),
                o.hce ? "hand-crafted evaluation" : o.net.c_str(), sumScore / data.size(), sumResult / data.size());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: nnue_train train|eval <data.bin>... [options]; see the comment at the top of tools/nnue/train.cpp\n");
        return 2;
    }
    const std::string mode = argv[1];
    Options o;
    for (int a = 2; a < argc; a++) {
        const std::string arg = argv[a];
        auto value = [&]() -> const char* { return a + 1 < argc ? argv[++a] : "0"; };
        if (arg == "--out") o.out = value();
        else if (arg == "--init") o.init = value();
        else if (arg == "--net") o.net = value();
        else if (arg == "--hce") o.hce = true;
        else if (arg == "--valid-only") o.validOnly = true;
        else if (arg == "--epochs") o.epochs = std::atoi(value());
        else if (arg == "--batch") o.batch = std::atoi(value());
        else if (arg == "--lr") o.lr = std::atof(value());
        else if (arg == "--lambda") o.lambda = std::atof(value());
        else if (arg == "--k") o.k = std::atof(value());
        else if (arg == "--seed") o.seed = std::atoi(value());
        else if (arg == "--min-depth") o.minDepth = std::atoi(value());
        else if (arg == "--min-ply") o.minPly = std::atoi(value());
        else if (arg == "--max-score") o.maxScore = std::atoi(value());
        else o.inputs.push_back(arg);
    }
    if (o.inputs.empty()) {
        std::fprintf(stderr, "no data files\n");
        return 2;
    }
    if (mode == "train") return train(o);
    if (mode == "eval") return evaluateSet(o);
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
}
