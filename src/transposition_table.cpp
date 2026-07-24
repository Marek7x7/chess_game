#include "transposition_table.hpp"

TranspositionTable::TranspositionTable(size_t sizePowerOfTwo)
    : table(sizePowerOfTwo), mask(sizePowerOfTwo - 1), shards(kNumShards) {}

std::mutex& TranspositionTable::shardFor(uint64_t key) const {
    return shards[(key & mask) & (kNumShards - 1)];
}

bool TranspositionTable::probe(uint64_t key, TTEntry& out) const {
    std::lock_guard<std::mutex> lock(shardFor(key));
    const TTEntry& e = table[key & mask];
    if (e.key == key && e.depth >= 0) {
        out = e;
        return true;
    }
    return false;
}

void TranspositionTable::store(uint64_t key, int depth, int score, TTFlag flag, const Move& best, bool hasMove) {
    std::lock_guard<std::mutex> lock(shardFor(key));
    TTEntry& slot = table[key & mask];
    if (slot.key != key || depth >= slot.depth) {
        slot.key = key;
        slot.depth = depth;
        slot.score = score;
        slot.flag = flag;
        slot.best = best;
        slot.hasMove = hasMove;
    }
}
