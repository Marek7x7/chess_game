// Texel-style tuner: fits the engine's evaluation weights to reference
// evaluations (Stockfish's, for positions taken from the self-play games).
//
// Build: cmake --build build-tune --target chess_tune   (a -DTUNING build, where
// every TUNE_INT / TUNE_ARR weight is a registered mutable variable).
// Run:   build-tune/chess_tune labels.csv [out.txt] [name,name,...] [K]
//   labels.csv: header "game,fen,sf_cp_white", one quiet position per row.
//   out.txt:    one line per weight, "NAME v0 v1 ...", the best-validation weights.
//   names:      optional comma-separated subset of weights to tune (default: all).
//   K:          optional sigmoid scale in centipawns (default 400 / ln 10).
//
// The loss is the mean squared difference between sigmoid(reference / K) and
// sigmoid(ours / K) -- both mapped to a win-probability-like number, so a
// position that is +9 pawns for the reference and +5 for us costs little
// while +0.3 against -0.3 costs a lot.
// Whole games go to either the training or the validation side. Each weight is
// moved by +-step while that lowers the training loss (steps 8, 4, 2, 1), and
// the weights with the lowest validation loss seen are kept.

#include <omp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "board.hpp"
#include "tune.hpp"

int tunerEvaluate(const Board& board);

namespace {

struct Sample {
    Board board;
    double target; // reference score, centipawns, positive = good for White
};

double sigmoid(double x) { return 1.0 / (1.0 + std::exp(-x)); }

double lossOf(const std::vector<Sample>& set, double k) {
    double sum = 0;
#pragma omp parallel for reduction(+ : sum) schedule(static)
    for (size_t i = 0; i < set.size(); i++) {
        double d = sigmoid(set[i].target / k) - sigmoid(tunerEvaluate(set[i].board) / k);
        sum += d * d;
    }
    return sum / static_cast<double>(set.size());
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string item;
    std::stringstream ss(s);
    while (std::getline(ss, item, sep)) out.push_back(item);
    return out;
}

// Stable assignment of whole games to validation (about one in five).
bool isValidationGame(const std::string& game) {
    size_t h = std::hash<std::string>{}(game);
    return h % 5 == 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s labels.csv [out.txt] [name,name,...]\n", argv[0]);
        return 1;
    }
    std::string outPath = argc > 2 ? argv[2] : "tuned.txt";
    std::set<std::string> only;
    if (argc > 3)
        for (const std::string& n : split(argv[3], ',')) only.insert(n);

    std::vector<Sample> train, valid;
    {
        std::ifstream in(argv[1]);
        std::string line;
        std::getline(in, line); // header
        while (std::getline(in, line)) {
            std::vector<std::string> f = split(line, ',');
            if (f.size() < 3) continue;
            Sample s{Board::fromFEN(f[1]), std::atof(f[2].c_str())};
            (isValidationGame(f[0]) ? valid : train).push_back(std::move(s));
        }
    }
    std::printf("positions: %zu train, %zu validation, %d threads\n", train.size(), valid.size(), omp_get_max_threads());

    // Which weights to move.
    struct Slot {
        const char* name;
        int* value;
        int index;
        int start;
        int lo, hi; // the weight stays within these bounds
    };
    std::vector<Slot> slots;
    for (const TuneParam& p : tuneRegistry()) {
        if (!only.empty() && !only.count(p.name)) continue;
        // Bounds keep a weight near its starting value, so one that the data cannot
        // pin down stays plausible: between half and double (at least +-3), the
        // sign unchanged, and a weight that starts at 0 is an unused slot and
        // stays there. Piece-square tables are the exception: any square may move
        // by up to 15 either way.
        bool table = std::string(p.name).find("Table") != std::string::npos;
        for (int i = 0; i < p.count; i++) {
            int v = p.values[i];
            int lo, hi;
            if (table) {
                lo = v - 15;
                hi = v + 15;
            } else if (v == 0) {
                lo = hi = 0;
            } else {
                int a = std::max(std::abs(v) / 2, 3), b = std::max(std::abs(v), 3);
                lo = v > 0 ? std::max(1, v - a) : v - b;
                hi = v > 0 ? v + b : std::min(-1, v + a);
            }
            slots.push_back({p.name, p.values + i, i, v, lo, hi});
        }
    }
    std::printf("tuning %zu weights\n", slots.size());

    // K: the sigmoid scale. Both sides pass through it, so unlike tuning against
    // game results it cannot be fitted (the loss only shrinks as K grows); the
    // usual 400 centipawns per factor of ten in the odds is used instead.
    double k = argc > 4 ? std::atof(argv[4]) : 400.0 / std::log(10.0);
    double bestTrain = lossOf(train, k), startTrain = bestTrain;
    double bestValid = lossOf(valid, k), startValid = bestValid;
    std::printf("K = %.1f; start loss: train %.6f, validation %.6f\n", k, startTrain, startValid);
    auto slope = [&](const std::vector<Sample>& set) {
        double xy = 0, xx = 0;
        for (const Sample& smp : set) {
            double e = tunerEvaluate(smp.board);
            xy += e * smp.target;
            xx += e * e;
        }
        return xy / xx;
    };
    std::printf("scale: reference ~= %.3f x ours (train, least squares)\n", slope(train));

    std::vector<int> bestValues(slots.size());
    for (size_t i = 0; i < slots.size(); i++) bestValues[i] = *slots[i].value;
    double bestValidSeen = startValid;
    int sinceImprovement = 0;

    for (int step : {8, 4, 2, 1}) {
        for (int sweep = 0; sweep < 40; sweep++) {
            int accepted = 0;
            for (Slot& s : slots) {
                for (int dir : {1, -1}) {
                    int old = *s.value;
                    int next = old + dir * step;
                    if (next < s.lo || next > s.hi) continue;
                    *s.value = next;
                    double trial = lossOf(train, k);
                    if (trial < bestTrain - 1e-12) {
                        bestTrain = trial;
                        accepted++;
                        break; // keep the move; the next weight
                    }
                    *s.value = old;
                }
            }
            double v = lossOf(valid, k);
            std::printf("step %d sweep %2d: %4d moves, train %.6f, validation %.6f\n", step, sweep, accepted, bestTrain, v);
            std::fflush(stdout);
            if (v < bestValidSeen - 1e-9) {
                bestValidSeen = v;
                sinceImprovement = 0;
                for (size_t i = 0; i < slots.size(); i++) bestValues[i] = *slots[i].value;
            } else {
                sinceImprovement++;
            }
            if (accepted == 0 || sinceImprovement >= 3) break;
        }
        sinceImprovement = 0;
    }

    // Restore the weights that scored best on the validation games.
    for (size_t i = 0; i < slots.size(); i++) *slots[i].value = bestValues[i];
    std::printf("scale after tuning: reference ~= %.3f x ours\n", slope(train));
    std::printf("final loss: train %.6f (start %.6f), validation %.6f (start %.6f)\n", lossOf(train, k), startTrain,
                lossOf(valid, k), startValid);

    std::ofstream out(outPath);
    for (const TuneParam& p : tuneRegistry()) {
        bool tuned = only.empty() || only.count(p.name);
        if (!tuned) continue;
        out << p.name;
        for (int i = 0; i < p.count; i++) out << ' ' << p.values[i];
        out << '\n';
    }
    int changed = 0;
    for (const Slot& s : slots) changed += *s.value != s.start;
    std::printf("%d of %zu weights changed; wrote %s\n", changed, slots.size(), outPath.c_str());
    return 0;
}
