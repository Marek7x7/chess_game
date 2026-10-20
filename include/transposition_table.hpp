#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>

#include "move.hpp"

enum class TTFlag : uint8_t { Exact, LowerBound, UpperBound };

// "No static eval stored" (the cached eval fits 14 bits, so only |eval| <= 8190 is kept).
constexpr int kTTNoEval = INT32_MIN;

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
    int eval = kTTNoEval; // the position's static eval for the side to move, if it was cached
};

// Fixed-size hash table of search results, keyed by Zobrist hash. Shared by
// every search thread within one findBestMove call (Lazy-SMP style) without
// any locking: each slot is two 64-bit words, the packed entry and the key
// XORed with it. A probe that reads a slot while another thread is half way
// through rewriting it sees a key that doesn't match and treats it as a
// miss, so a torn entry is never used (the classic lockless-hashing scheme).
//
// Slots come in buckets of four (one 64-byte cache line), and a position can
// live in any slot of its bucket: a new result evicts whichever of the four is
// worth least -- one left over from an earlier search before one from this
// search, then the shallowest -- so a deep result isn't lost to an unrelated
// shallow store landing on the same slot.
class TranspositionTable {
public:
    explicit TranspositionTable(size_t sizePowerOfTwo); // total slots (four per bucket), at least 4

    // Distinct for every table ever constructed: lets state that lives beside the
    // table (see ai.cpp) tell a new game's table from the previous one's.
    uint64_t instanceId() const { return id; }

    bool probe(uint64_t key, TTEntry& out) const;
    void store(uint64_t key, int depth, int score, TTFlag flag, const Move& best, bool hasMove,
               int eval = kTTNoEval);

    // How full the table is with entries from the current search, in permille (UCI "hashfull"),
    // estimated from the first 2000 slots.
    int hashfull() const;

    // Starts loading a key's bucket into the cache, for a probe a little later.
    void prefetch(uint64_t key) const { __builtin_prefetch(&table[key & bucketMask]); }

    // Marks the start of a new search (call before its threads start):
    // entries stored before it become the first to be replaced.
    void newSearch() { generation = (generation + 1) & 15; }

    // Bytes per entry, for sizing a table to a memory budget.
    static constexpr size_t kEntryBytes = 16;

private:
    struct Slot {
        std::atomic<uint64_t> check{0}; // key ^ data
        std::atomic<uint64_t> data{0};  // see pack() in transposition_table.cpp
    };
    static_assert(sizeof(Slot) == kEntryBytes, "kEntryBytes must match the slot layout");

    // Cache-line aligned: a bucket is 64 bytes, exactly one line, so it must start on one.
    struct FreeDeleter {
#ifdef _WIN32
        void operator()(void* p) const { _aligned_free(p); } // memory from _aligned_malloc
#else
        void operator()(void* p) const { std::free(p); }
#endif
    };
    static constexpr size_t kBucketSlots = 4;
    std::unique_ptr<Slot[], FreeDeleter> table;
    size_t bucketMask; // slot index of a key's bucket = key & bucketMask (a multiple of 4)
    uint64_t generation = 0;
    uint64_t id = 0;
};
