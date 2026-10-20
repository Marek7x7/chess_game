#include <atomic>
#include "transposition_table.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace {

// Entry layout in one 64-bit word: bits 0-20 score (21 bits, signed: a mate
// score is about +-1'000'000), 21-34 static eval + 8192 (16383 = none), 35-41
// depth + 1 (0 marks an empty slot), 42-43 flag, 44 hasMove, 45-59 the move's
// from/to squares (3 bits per coordinate) and promotion piece (3 bits), 60-63
// the search generation that stored it (mod 16).
constexpr int kEvalNone = 16383;
constexpr int kEvalBias = 8192;

uint64_t pack(int depth, int score, TTFlag flag, const Move& best, bool hasMove, uint64_t generation, int eval) {
    uint64_t data = static_cast<uint64_t>(static_cast<uint32_t>(score)) & 0x1FFFFF;
    const uint64_t e = (eval == kTTNoEval || eval < -kEvalBias + 1 || eval > kEvalBias - 2)
                           ? uint64_t(kEvalNone)
                           : uint64_t(eval + kEvalBias);
    data |= e << 21;
    data |= uint64_t(std::clamp(depth + 1, 1, 127)) << 35;
    data |= uint64_t(static_cast<uint8_t>(flag)) << 42;
    data |= uint64_t(hasMove) << 44;
    data |= uint64_t(best.fromX) << 45 | uint64_t(best.fromY) << 48 | uint64_t(best.toX) << 51 |
            uint64_t(best.toY) << 54 | uint64_t(static_cast<int>(best.promotion)) << 57;
    data |= (generation & 15) << 60;
    return data;
}

int storedDepth(uint64_t data) { return static_cast<int>((data >> 35) & 0x7F) - 1; }
uint64_t storedGeneration(uint64_t data) { return data >> 60; }
int storedEval(uint64_t data) {
    const int e = static_cast<int>((data >> 21) & 0x3FFF);
    return e == kEvalNone ? kTTNoEval : e - kEvalBias;
}

TTEntry unpack(uint64_t key, uint64_t data) {
    TTEntry e;
    e.key = key;
    e.score = static_cast<int32_t>(static_cast<uint32_t>(data & 0x1FFFFF) << 11) >> 11; // sign-extend 21 bits
    e.eval = storedEval(data);
    e.depth = storedDepth(data);
    e.flag = static_cast<TTFlag>((data >> 42) & 0x3);
    e.hasMove = (data >> 44) & 1;
    e.best.fromX = (data >> 45) & 7;
    e.best.fromY = (data >> 48) & 7;
    e.best.toX = (data >> 51) & 7;
    e.best.toY = (data >> 54) & 7;
    e.best.promotion = static_cast<PieceType>((data >> 57) & 7);
    return e;
}

} // namespace

TranspositionTable::TranspositionTable(size_t sizePowerOfTwo) : bucketMask((sizePowerOfTwo - 1) & ~size_t(1)) {
    static std::atomic<uint64_t> nextId{1};
    id = nextId++;
    if (sizePowerOfTwo < 2 || (sizePowerOfTwo & (sizePowerOfTwo - 1)) != 0) {
        throw std::invalid_argument("TranspositionTable: sizePowerOfTwo must be a power of two, at least 2");
    }
    size_t bytes = (sizePowerOfTwo * sizeof(Slot) + 63) & ~size_t(63); // aligned_alloc wants a multiple of 64
#ifdef _WIN32
    void* memory = _aligned_malloc(bytes, 64); // the Windows C runtime has no aligned_alloc
#else
    void* memory = std::aligned_alloc(64, bytes);
#endif
    if (!memory) throw std::bad_alloc();
    table.reset(static_cast<Slot*>(memory));
    for (size_t i = 0; i < sizePowerOfTwo; i++) new (&table[i]) Slot();
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
void TranspositionTable::store(uint64_t key, int depth, int score, TTFlag flag, const Move& best, bool hasMove,
                               int eval) {
    Slot* bucket = &table[key & bucketMask];
    uint64_t old[2];
    for (int i = 0; i < 2; i++) old[i] = bucket[i].data.load(std::memory_order_relaxed);

    Slot* target = nullptr;
    for (int i = 0; i < 2 && !target; i++) {
        bool samePosition =
            (bucket[i].check.load(std::memory_order_relaxed) ^ old[i]) == key && storedDepth(old[i]) >= 0;
        if (!samePosition) continue;
        if (depth < storedDepth(old[i])) return;
        if (eval == kTTNoEval) eval = storedEval(old[i]); // keep an eval this position already has
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
    uint64_t data = pack(depth, score, flag, best, hasMove, generation, eval);
    target->data.store(data, std::memory_order_relaxed);
    target->check.store(key ^ data, std::memory_order_relaxed);
}

int TranspositionTable::hashfull() const {
    const size_t slots = std::min<size_t>(2000, bucketMask + 2); // bucketMask + 2 is the slot count
    size_t used = 0;
    for (size_t i = 0; i < slots; i++) {
        const uint64_t data = table[i].data.load(std::memory_order_relaxed);
        if (storedDepth(data) >= 0 && storedGeneration(data) == generation) used++;
    }
    return static_cast<int>(used * 1000 / slots);
}
