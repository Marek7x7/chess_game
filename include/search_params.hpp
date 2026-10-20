#pragma once

// Hand-set search constants are declared with SEARCH_PARAM(name, default, min, max).
// In the normal build they are plain constexpr values, exactly as if written out
// by hand. In the SPSA build (-DSPSA, target chess_spsa) they are mutable
// variables that show up as UCI spin options, so tools/spsa.py can set them
// per engine process without recompiling.
#ifdef SPSA
#include <vector>

struct SearchParam {
    const char* name;
    int* value;
    int def, lo, hi;
};

inline std::vector<SearchParam>& searchParamRegistry() {
    static std::vector<SearchParam> registry;
    return registry;
}

struct SearchParamReg {
    SearchParamReg(const char* name, int* value, int def, int lo, int hi) {
        searchParamRegistry().push_back({name, value, def, lo, hi});
    }
};

#define SEARCH_PARAM(name, def, lo, hi) \
    inline int name = def;              \
    inline SearchParamReg name##_reg(#name, &name, def, lo, hi)
#else
#define SEARCH_PARAM(name, def, lo, hi) constexpr int name = def
#endif

SEARCH_PARAM(spRfpMargin, 59, 50, 220);        // reverse futility, per remaining ply
SEARCH_PARAM(spFutMargin1, 291, 50, 600);       // futility margin at depth 1
SEARCH_PARAM(spFutMargin2, 246, 120, 550);      // futility margin at depth 2
SEARCH_PARAM(spLmpBase, 5, 1, 8);               // late-move pruning: moveIndex >= base + depth^2
SEARCH_PARAM(spLmrBase, 114, 20, 130);           // LMR table: (base + ln d ln m / div) / 100
SEARCH_PARAM(spLmrDiv, 238, 140, 380);          // LMR table divisor, in hundredths
SEARCH_PARAM(spLmrHistDiv, 13033, 2500, 30000);  // history score per extra/less LMR ply
SEARCH_PARAM(spLmrStart, 3, 2, 7);              // first move index that may be reduced
SEARCH_PARAM(spNullBase, 3, 2, 5);              // null-move reduction: base + depth / div
SEARCH_PARAM(spNullDiv, 8, 3, 12);
SEARCH_PARAM(spSingularMargin, 1, 1, 5);        // singular beta = ttScore - margin * depth
SEARCH_PARAM(spSeePruneMargin, 76, 30, 220);   // SEE pruning of captures, per remaining ply
SEARCH_PARAM(spAspiration, 19, 8, 60);          // initial aspiration window
SEARCH_PARAM(spDeltaMargin, 307, 90, 380);      // quiescence delta pruning
SEARCH_PARAM(spLazyMargin, 267, 220, 700);      // quiescence lazy-eval cutoff
SEARCH_PARAM(spHistBonus, 7, 1, 12);            // history bonus: depth^2 * this, capped
SEARCH_PARAM(spHistCap, 2247, 400, 4000);
SEARCH_PARAM(spNullMinDepth, 2, 1, 5);
SEARCH_PARAM(spSingularMinDepth, 5, 3, 12);
SEARCH_PARAM(spSingularDepthGap, 3, 1, 5);      // the TT entry may be this much shallower
SEARCH_PARAM(spLmrMinDepth, 3, 3, 7);           // first depth that may reduce (>= 3: the reduction is clamped to [1, depth - 2])
SEARCH_PARAM(spLmpMaxDepth, 4, 2, 8);
SEARCH_PARAM(spRfpMaxDepth, 5, 2, 8);
SEARCH_PARAM(spSeePruneDepth, 5, 2, 10);

#ifndef SPSA
static_assert(spLmrMinDepth >= 3, "std::clamp(reduction, 1, depth - 2) in negamax needs depth - 2 >= 1");
#endif
