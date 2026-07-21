#pragma once

#include <cstdint>
#include <vector>

#include "move.hpp"

enum class TTFlag : uint8_t { Exact, LowerBound, UpperBound };

struct TTEntry {
    uint64_t key = 0;
    int depth = -1;
    int score = 0;
    TTFlag flag = TTFlag::Exact;
    Move best{};
    bool hasMove = false;
};

// Fixed-size hash table of search results, keyed by Zobrist hash. An entry
// is only replaced by a shallower search if the incoming one is at least as
// deep, so deeper (more trustworthy) results aren't evicted by noise.
class TranspositionTable {
public:
    explicit TranspositionTable(size_t sizePowerOfTwo);

    const TTEntry* probe(uint64_t key) const;
    void store(uint64_t key, int depth, int score, TTFlag flag, const Move& best, bool hasMove);

private:
    std::vector<TTEntry> table;
    size_t mask;
};
