#include "transposition_table.hpp"

#include <algorithm>
#include <stdexcept>

namespace {

// Entry layout in one 64-bit word: bits 0-31 score, 32-39 depth + 1 (0 marks
// an empty slot), 40-41 flag, 42 hasMove, 43-57 the move's from/to squares
// (3 bits per coordinate) and promotion piece (3 bits).
uint64_t pack(int depth, int score, TTFlag flag, const Move& best, bool hasMove) {
    uint64_t data = static_cast<uint32_t>(score);
    data |= uint64_t(std::clamp(depth + 1, 1, 255)) << 32;
    data |= uint64_t(static_cast<uint8_t>(flag)) << 40;
    data |= uint64_t(hasMove) << 42;
    data |= uint64_t(best.fromX) << 43 | uint64_t(best.fromY) << 46 | uint64_t(best.toX) << 49 |
            uint64_t(best.toY) << 52 | uint64_t(static_cast<int>(best.promotion)) << 55;
    return data;
}

int storedDepth(uint64_t data) { return static_cast<int>((data >> 32) & 0xFF) - 1; }

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

TranspositionTable::TranspositionTable(size_t sizePowerOfTwo) : table(sizePowerOfTwo), mask(sizePowerOfTwo - 1) {
    if (sizePowerOfTwo == 0 || (sizePowerOfTwo & (sizePowerOfTwo - 1)) != 0) {
        throw std::invalid_argument("TranspositionTable: sizePowerOfTwo must be a power of two");
    }
}

bool TranspositionTable::probe(uint64_t key, TTEntry& out) const {
    const Slot& slot = table[key & mask];
    uint64_t data = slot.data.load(std::memory_order_relaxed);
    uint64_t check = slot.check.load(std::memory_order_relaxed);
    if ((check ^ data) != key || storedDepth(data) < 0) return false;
    out = unpack(key, data);
    return true;
}

// Overwrites the slot unless it already holds a deeper result for this same
// position; a different position colliding into the slot always replaces it.
void TranspositionTable::store(uint64_t key, int depth, int score, TTFlag flag, const Move& best, bool hasMove) {
    Slot& slot = table[key & mask];
    uint64_t old = slot.data.load(std::memory_order_relaxed);
    bool samePosition = (slot.check.load(std::memory_order_relaxed) ^ old) == key && storedDepth(old) >= 0;
    if (samePosition && depth < storedDepth(old)) return;
    uint64_t data = pack(depth, score, flag, best, hasMove);
    slot.data.store(data, std::memory_order_relaxed);
    slot.check.store(key ^ data, std::memory_order_relaxed);
}
