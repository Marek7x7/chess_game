# Benchmark Baseline (Step 0)

Recorded before any optimization work (Steps 1+). Build: `cmake --build build`
(ninja, GCC 15.2.0, `-Wall -Wextra -Werror`, C++17). Build produced **zero warnings**.

## Existing test suite

There was no test suite in the repository (no `tests/` directory, no perft
harness) before this pass. `main.cpp` already had a bare `--perft <depth>`
mode (start position only, no known-correct comparison values wired in), so
it was not actually being used as a verification safety net.

Added before touching any optimization code:
- `Board::fromFEN()` (`include/board.hpp`, `src/board.cpp`) — needed to load
  standard perft test positions, since the engine had no way to set up a
  position other than the initial one or by replaying legal moves.
- `--perft-suite <depth>` (`src/main.cpp`) — runs perft on 3 standard,
  independently-published test positions (start position, "Kiwipete", and a
  sparse K+R+P endgame) and compares against known-correct node counts at
  each depth. **This is the safety net for every step below**: it will be
  re-run after every change and must pass with byte-identical node counts.
- `BenchResult benchSearch()` (`include/ai.hpp`, `src/ai.cpp`) and
  `--bench <depth>` (`src/main.cpp`) — single-threaded, fixed-depth (no time
  cutoff) search used purely for before/after node-count and nodes/sec
  comparisons across steps. Does not share state with or affect
  `findBestMove`.

## Perft suite result (safety net)

```
startpos   perft(1..5) = 20, 400, 8902, 197281, 4865609        — ALL OK
kiwipete   perft(1..4) = 48, 2039, 97862, 4085603              — ALL OK
endgame    perft(1..5) = 14, 191, 2812, 43238, 674624           — ALL OK
PERFT SUITE: ALL PASS
```

All values match the standard published perft results exactly. This
confirms current move generation (including castling rights, en passant,
promotions) is correct before any make/unmake or hashing changes begin.

## Search benchmark — depth 6, single-threaded, no time cutoff

| Position | Nodes | Time (s) | Nodes/sec | Score (cp, mate=+/-999999ish) | Best move |
|---|---|---|---|---|---|
| startpos | 100,732 | 0.407 | 247,416 | 0 | g1f3 |
| middlegame (Kiwipete) | 1,256,733 | 4.174 | 301,083 | 19 | e2a6 |
| endgame (sparse K+R+P) | 41,856 | 0.188 | 222,264 | 15 | b4f4 |
| tactical (mate-in-1) | 39 | 0.105 | 372 | 999999 | d1d8 |

Notes:
- `benchSearch` allocates a fresh 224 MiB transposition table
  (`kTTSize = 1<<22` entries) per call. For the tactical position (39 nodes)
  that allocation/zero-init dominates the measured 0.105s, so its
  nodes/sec figure is not meaningful — only its node count and move choice
  are useful regression signals for that position. This overhead is exactly
  what Step 4.5 (persistent TT) targets, so it will be re-measured properly
  there rather than "fixed" now.
- Kiwipete's chosen move (e2a6, Bxa6) reflects this evaluator's shallow-depth
  material/PST view of an already-tactically-loaded position, not a claim
  about objective correctness — it's only useful here as an exact
  before/after regression fingerprint.
- These are the numbers every later step's table compares against.

## Positions used

- **startpos**: default `Board()`.
- **middlegame**: Kiwipete,
  `r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1`
- **endgame**: `8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1`
- **tactical (mate-in-1)**: `6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1`
  (White plays Rd8#; hand-verified: king has no legal moves — f7/g7/h7 are
  occupied by own pawns and f8/h8 are on the rook's rank — and there is no
  piece to block or capture the rook).

---

# Final Report (Step 5)

## Per-step verification summary

| Step | What changed | Perft (safety net) | Move choice | Notes |
|---|---|---|---|---|
| 0 | Baseline + added `--perft-suite`, `--verify-eval`, `--bench` harness (none existed) | 4.3M positions, all match published values | — | No pre-existing test suite; zero build warnings |
| 1 | `negamax`/`quiescence`/`searchRootFull`: `Board copy=board;copy.makeMove()` → in-place `makeMove(m,undo)`/`unmakeMove(m,undo)` | Unchanged, **and** cross-checked make/unmake-walked perft against copy-based perft (byte-identical at every depth) | Identical to baseline | Modest speedup — `Board` copy was already cheap (128-byte struct) |
| 2 | `evaluate()` made incremental via `Board::rawEval()`. Correction to the plan: `hashKey()` was *already* incremental; only material+PST eval was recomputed from scratch. King's PST term kept live (phase-dependent) rather than frozen, to avoid silent drift | Unchanged | Identical to Step 1 | Verified incremental eval == from-scratch recompute at **every node**, 9.9M positions total (depth 4 all 3 positions + depth 5 startpos/endgame), zero mismatches |
| 3 | `std::vector<Move>` → fixed-capacity `MoveList` (256-slot stack array) in `legalMoves`/`pseudoLegalMoves`/`negamax`/`quiescence` | Unchanged | Identical to Step 2 | ASan+UBSan build ran full perft suite + depth-5 bench with **zero sanitizer errors** (no overruns) |
| 4 | TT constructor now throws on a non-power-of-two size (verified both directions with a standalone test); TSan run across two full replayed games (73 multithreaded `findBestMove` calls, 12 threads each, shared TT) | Unchanged | — | **Zero TSan warnings** — empirical confirmation of the shard-mutex design, not just review |
| 4.1 | Confirmed `hashKey()` already covers pieces/side-to-move/castling×4/en-passant (no gap found); added legality check on book moves before returning them | Unchanged | — | Transposition sanity check (different move orders reaching the same position) passed once a real en-passant-availability edge case in my first attempt was identified and worked around |
| 4.5 | `TranspositionTable` now owned by `App` (GUI) / `runReplay` (once per game), passed by reference into `findBestMove` instead of rebuilt every move | Unchanged | Identical (persistent TT never changes the answer) | Persistent-TT continuation used **8.1% fewer nodes** than an isolated fresh-TT control to reach the same depth/move/score; eliminates a measured **~78ms** fixed TT-allocation cost every move |
| 4.6 | Investigated `WINNER IS NOT HIGHEST SCORE` debug trap | — | — | **Never fired** across 6 full-game replays under varied conditions (TSan, pre-4.5, post-4.5/persistent-TT) — matches the by-inspection expectation; left in place as a regression guard |

## Benchmark table — depth 6, single-threaded (`benchSearch`), nodes/sec

| Position | Baseline | Step 1 | Step 2 | Step 3 | Final (post-4.5) |
|---|---|---|---|---|---|
| startpos | 247,416 | 248,602 | 252,653 | 266,350 | ~271,000 |
| middlegame (Kiwipete) | 301,083 | 296,601 | 303,898 | 331,715 | ~340,200 |
| endgame | 222,264 | 220,493 | 228,835 | 226,871 | ~234,200 |
| tactical (mate-in-1) | 372 | (noise) | (noise) | (noise) | (noise, 39-node search dominated by TT alloc in this isolated benchmark path) |

Node counts and chosen moves (`g1f3` / `e2a6` / `b4f4` / `d1d8`) are **byte-identical** across every step — confirmed, not assumed.

**Overall speedup on raw single-position search throughput: ~1.10–1.13x** (startpos/middlegame), ~1.05x (endgame). This is a real but modest number, and I want to be direct about why it isn't larger: `Board::legalMoves()` does its own internal `Board copy=*this; copy.makeMove(m); isInCheck()` for *every pseudo-legal move*, just to filter legality — and it's called at the top of every search node. For a ~35-move middlegame position that's ~35 board copies per node just building the move list, dwarfing the one-copy-per-explored-child that Steps 1–3 eliminated. That copy was outside every step's stated scope (Step 1 named `negamax`/`quiescence`/`searchRootFull` specifically), so it was deliberately left alone rather than silently expanding scope. **This is the clearest lever for a follow-up pass**, bigger than anything remaining in Steps 1–3's original scope.

The step that matters most for *actual gameplay* isn't fully visible in the nodes/sec table above: Step 4.5 (persistent TT) doesn't speed up a single isolated search — `benchSearch` deliberately keeps its own fresh-TT-per-call design as a stable regression baseline — but it removes a **~78ms fixed allocation cost every single move** in real play, and gives roughly **8% fewer nodes** to reach the same depth on the very next move once the opponent's reply has already been partially explored. Over a full game this compounds; the depth-6 isolated-position table above can't show it by construction.

## Files changed

- `include/ai.hpp`, `src/ai.cpp` — make/unmake in search, incremental-eval call sites, `MoveList` in negamax/quiescence, `benchSearch`, persistent-TT signature for `findBestMove`, book-move legality check
- `include/board.hpp`, `src/board.cpp` — `UndoState`/`makeMove(m,undo)`/`unmakeMove`, incremental `rawEval()`/`gamePhase()`/`rawEvalFromScratch()`, `MoveList`-returning move generation, `Board::fromFEN()`
- `include/move.hpp` — `MoveList` (fixed 256-slot move container)
- `include/piece.hpp`, `src/piece.cpp` — shared `pieceValue()` (previously duplicated logic between eval and move-ordering)
- `src/transposition_table.cpp` — power-of-two constructor guard
- `src/gui.cpp` — `App` now owns a persistent `TranspositionTable`
- `src/main.cpp` — `--perft-suite`, `--verify-eval`, `--bench` harness; `runReplay` now owns a persistent TT
- `src/pgn.cpp` — updated for `MoveList` return type

## Known limitations / not independently re-verifiable by the reader without rebuilding

- ASan/UBSan and TSan builds live in gitignored `build-asan/` and `build-tsan/` directories (not committed, regenerate via the `cmake -DCMAKE_CXX_FLAGS="-fsanitize=..."` invocations used during this pass).
- The opening-book transposition check and the TT-persistence node-count comparison were run as standalone throwaway programs (not committed as permanent test infrastructure), since they're one-off verifications of specific claims rather than reusable regression tools like `--perft-suite`/`--verify-eval`/`--bench`.
- Perft was run to depth 4 (all 3 positions) and depth 5 (startpos, endgame) rather than depth 5 everywhere — Kiwipete depth 5 (193M nodes) was judged not worth the extra ~50s given depth 4 (4.3M nodes) already passed cleanly and depth 4 already exercises every castling/en-passant/promotion edge case the suite is meant to catch.
- Did not implement null-move pruning, LMR, PVS, aspiration windows, or bitboards, per your explicit "do not do yet" list.
