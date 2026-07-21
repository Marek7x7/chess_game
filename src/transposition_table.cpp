#include "transposition_table.hpp"

TranspositionTable::TranspositionTable(size_t sizePowerOfTwo)
    : table(sizePowerOfTwo), mask(sizePowerOfTwo - 1) {}

const TTEntry* TranspositionTable::probe(uint64_t key) const {
    const TTEntry& e = table[key & mask];
    if (e.key == key && e.depth >= 0) return &e;
    return nullptr;
}

void TranspositionTable::store(uint64_t key, int depth, int score, TTFlag flag, const Move& best, bool hasMove) {
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
