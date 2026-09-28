#include "transposition_table.hpp"

#include <algorithm>
#include <stdexcept>

namespace {

// Entry layout in one 64-bit word: bits 0-31 score, 32-39 depth + 1 (0 marks
// an empty slot), 40-41 flag, 42 hasMove, 43-57 the move's from/to squares
// (3 bits per coordinate) and promotion piece (3 bits), 58-63 the search
// generation that stored it.
uint64_t pack(int depth, int score, TTFlag flag, const Move& best, bool hasMove, uint64_t generation) {
    uint64_t data = static_cast<uint32_t>(score);
    data |= uint64_t(std::clamp(depth + 1, 1, 255)) << 32;
    data |= uint64_t(static_cast<uint8_t>(flag)) << 40;
    data |= uint64_t(hasMove) << 42;
    data |= uint64_t(best.fromX) << 43 | uint64_t(best.fromY) << 46 | uint64_t(best.toX) << 49 |
            uint64_t(best.toY) << 52 | uint64_t(static_cast<int>(best.promotion)) << 55;
    data |= generation << 58;
    return data;
}

int storedDepth(uint64_t data) { return static_cast<int>((data >> 32) & 0xFF) - 1; }
uint64_t storedGeneration(uint64_t data) { return data >> 58; }

TTEntry unpack(uint64_t key, uint64_t data) {
    TTEntry e;
    e.key = key;
    e.score = static_cast<int32_t>(static_cast<uint32_t>(data));
    e.depth = storedDepth(data);
    e.flag = static_cast<TTFlag>((data >> 40) & 0x3);
    e.hasMove = (data >> 42) & 1;
    e.best.fromX = (data >> 43) & 7;
    e.best.fromY = (data >> 46) & 7;
    e.best.toX = (data >> 49) & 7;
    e.best.toY = (data >> 52) & 7;
    e.best.promotion = static_cast<PieceType>((data >> 55) & 7);
    return e;
}

} // namespace

TranspositionTable::TranspositionTable(size_t sizePowerOfTwo)
    : table(sizePowerOfTwo), bucketMask((sizePowerOfTwo - 1) & ~size_t(1)) {
    if (sizePowerOfTwo < 2 || (sizePowerOfTwo & (sizePowerOfTwo - 1)) != 0) {
        throw std::invalid_argument("TranspositionTable: sizePowerOfTwo must be a power of two, at least 2");
    }
}

bool TranspositionTable::probe(uint64_t key, TTEntry& out) const {
    const Slot* bucket = &table[key & bucketMask];
    for (int i = 0; i < 2; i++) {
        uint64_t data = bucket[i].data.load(std::memory_order_relaxed);
        uint64_t check = bucket[i].check.load(std::memory_order_relaxed);
        if ((check ^ data) == key && storedDepth(data) >= 0) {
            out = unpack(key, data);
            return true;
        }
    }
    return false;
}

// If the bucket already holds this position, updates it in place -- unless
// that's a deeper result, which is kept. Otherwise the new result evicts the
// less valuable of the two slots.
void TranspositionTable::store(uint64_t key, int depth, int score, TTFlag flag, const Move& best, bool hasMove) {
    Slot* bucket = &table[key & bucketMask];
    uint64_t old[2];
    for (int i = 0; i < 2; i++) old[i] = bucket[i].data.load(std::memory_order_relaxed);

    Slot* target = nullptr;
    for (int i = 0; i < 2 && !target; i++) {
        bool samePosition =
            (bucket[i].check.load(std::memory_order_relaxed) ^ old[i]) == key && storedDepth(old[i]) >= 0;
        if (!samePosition) continue;
        if (depth < storedDepth(old[i])) return;
        target = &bucket[i];
    }
    if (!target) {
        // Worth of a slot: its depth, far less if it's left over from an
        // earlier search (an empty slot counts as depth -1).
        auto worth = [&](uint64_t data) {
            return storedDepth(data) - (storedGeneration(data) == generation ? 0 : 256);
        };
        target = worth(old[0]) <= worth(old[1]) ? &bucket[0] : &bucket[1];
    }
    uint64_t data = pack(depth, score, flag, best, hasMove, generation);
    target->data.store(data, std::memory_order_relaxed);
    target->check.store(key ^ data, std::memory_order_relaxed);
}
