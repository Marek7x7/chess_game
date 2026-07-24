#pragma once

#include <cstdint>
#include <mutex>
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

// Fixed-size hash table of search results, keyed by Zobrist hash. Shared by
// every search thread within one findBestMove call (Lazy-SMP style), so
// probe()/store() are safe to call concurrently: access is guarded by a
// fixed bank of shard mutexes (one per block of slots, far fewer than the
// slot count) rather than a single global lock, keeping contention low
// without a mutex per slot. store()'s read-compare-write that decides
// whether to keep the existing (possibly deeper) entry happens under a
// single shard-lock acquisition, so that decision can't race with a
// concurrent store to the same slot. probe() returns a copy of the entry,
// not a pointer into the table, because a pointer would remain readable by
// the caller after the lock is released, racing with another thread's
// concurrent store to that same slot.
class TranspositionTable {
public:
    explicit TranspositionTable(size_t sizePowerOfTwo);

    bool probe(uint64_t key, TTEntry& out) const;
    void store(uint64_t key, int depth, int score, TTFlag flag, const Move& best, bool hasMove);

private:
    static constexpr size_t kNumShards = size_t(1) << 16;

    std::mutex& shardFor(uint64_t key) const;

    std::vector<TTEntry> table;
    size_t mask;
    mutable std::vector<std::mutex> shards;
};
