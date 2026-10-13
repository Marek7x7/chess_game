#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>

#include "move.hpp"

enum class TTFlag : uint8_t { Exact, LowerBound, UpperBound };

// What probe() hands back: an unpacked copy of one stored entry. `best`
// carries only the squares and the promotion piece -- all that sameMove()
// compares -- not the capture/castling/en-passant details, so a caller that
// needs to play it must look up the matching legal move.
struct TTEntry {
    uint64_t key = 0;
    int depth = -1;
    int score = 0;
    TTFlag flag = TTFlag::Exact;
    Move best{};
    bool hasMove = false;
};

// Fixed-size hash table of search results, keyed by Zobrist hash. Shared by
// every search thread within one findBestMove call (Lazy-SMP style) without
// any locking: each slot is two 64-bit words, the packed entry and the key
// XORed with it. A probe that reads a slot while another thread is half way
// through rewriting it sees a key that doesn't match and treats it as a
// miss, so a torn entry is never used (the classic lockless-hashing scheme).
//
// Slots come in buckets of two, and a position can live in either slot of
// its bucket: a new result evicts whichever of the two is worth less -- one
// left over from an earlier search before one from this search, then the
// shallower one -- so a deep result isn't lost to an unrelated shallow
// store landing on the same slot.
class TranspositionTable {
public:
    explicit TranspositionTable(size_t sizePowerOfTwo); // total slots (two per bucket), at least 2

    bool probe(uint64_t key, TTEntry& out) const;
    void store(uint64_t key, int depth, int score, TTFlag flag, const Move& best, bool hasMove);

    // Starts loading a key's bucket into the cache, for a probe a little later.
    void prefetch(uint64_t key) const { __builtin_prefetch(&table[key & bucketMask]); }

    // Marks the start of a new search (call before its threads start):
    // entries stored before it become the first to be replaced.
    void newSearch() { generation = (generation + 1) & 63; }

    // Bytes per entry, for sizing a table to a memory budget.
    static constexpr size_t kEntryBytes = 16;

private:
    struct Slot {
        std::atomic<uint64_t> check{0}; // key ^ data
        std::atomic<uint64_t> data{0};  // see pack() in transposition_table.cpp
    };
    static_assert(sizeof(Slot) == kEntryBytes, "kEntryBytes must match the slot layout");

    // Cache-line aligned: a bucket is 32 bytes, so with only the allocator's
    // 16-byte alignment half the buckets would straddle two cache lines.
    struct FreeDeleter {
        void operator()(void* p) const { std::free(p); }
    };
    std::unique_ptr<Slot[], FreeDeleter> table;
    size_t bucketMask; // slot index of a key's bucket = key & bucketMask (always even)
    uint64_t generation = 0;
};
