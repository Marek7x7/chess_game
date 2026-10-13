#pragma once

// Evaluation weights are declared with these macros. In the normal build they
// are plain constexpr values, exactly as if written out by hand. In the
// tuning build (-DTUNING, target chess_tune) they are mutable variables that
// register themselves, so tools/tune.cpp can adjust every weight and re-run
// the evaluation without recompiling.
#ifdef TUNING
#include <vector>

struct TuneParam {
    const char* name;
    int* values;
    int count;
};

inline std::vector<TuneParam>& tuneRegistry() {
    static std::vector<TuneParam> registry;
    return registry;
}

struct TuneReg {
    TuneReg(const char* name, int* values, int count) { tuneRegistry().push_back({name, values, count}); }
};

#define TUNE_INT(name, value) \
    int name = value;         \
    static TuneReg name##_reg(#name, &name, 1)
#define TUNE_ARR(name, n, ...) \
    int name[n] = __VA_ARGS__; \
    static TuneReg name##_reg(#name, name, n)
#define TUNE_ARR2(name, rows, cols, ...) \
    int name[rows][cols] = __VA_ARGS__;  \
    static TuneReg name##_reg(#name, &name[0][0], (rows) * (cols))
#else
#define TUNE_INT(name, value) constexpr int name = value
#define TUNE_ARR(name, n, ...) constexpr int name[n] = __VA_ARGS__
#define TUNE_ARR2(name, rows, cols, ...) constexpr int name[rows][cols] = __VA_ARGS__
#endif
