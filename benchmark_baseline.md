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

---

# Tier 2: legalMoves() Copy Elimination — Baseline (Step 0)

Recorded before touching `Board::legalMoves()`. Build: clean, zero warnings
(`ninja: no work to do` on an already-up-to-date tree, confirmed at start).

## Scope confirmation

Searched the whole repo for any remaining "copy the board per candidate move"
pattern. Found exactly one in the hot path:

- `Board::legalMoves()` (`src/board.cpp:537-544`): `Board copy = *this;
  copy.makeMove(m); if (!copy.isInCheck(color)) ...` for every pseudo-legal
  move, called at the top of every `negamax`/`quiescence` node. **This is the
  target of this pass.**

Other `Board copy`/`Board local`/`Board threadBoard` sites found are all
one-time, non-per-candidate copies, out of scope:
- `ai.cpp` (`benchSearch`, `findBestMove`): one copy per benchmark call / per
  search thread at startup, not per move.
- `main.cpp` (`perft()`, `runPerftSuite()`): the copy-based `perft()` is
  deliberately kept as the independent cross-check against `perftUnmake()` —
  changing it would remove that verification method, not exercise it.

## Perft suite (safety net)

```
startpos   perft(1..4) = 20, 400, 8902, 197281       — ALL OK (unmake cross-check also OK)
kiwipete   perft(1..4) = 48, 2039, 97862, 4085603     — ALL OK
endgame    perft(1..4) = 14, 191, 2812, 43238         — ALL OK
PERFT SUITE: ALL PASS
```

## Eval verification

```
startpos   depth=4 nodesChecked=197281   mismatches=0 OK
kiwipete   depth=4 nodesChecked=4085603  mismatches=0 OK
endgame    depth=4 nodesChecked=43238    mismatches=0 OK
EVAL VERIFICATION: ALL PASS
```

## Search benchmark — depth 6, single-threaded (`benchSearch`), average of 2 clean runs

| Position | Nodes | Nodes/sec | Score | Move |
|---|---|---|---|---|
| startpos | 100,732 | ~274,000 | 0 | g1f3 |
| middlegame (Kiwipete) | 1,256,733 | ~346,000 | 19 | e2a6 |
| endgame | 41,856 | ~235,400 | 15 | b4f4 |
| tactical (mate-in-1) | 39 | (noise, TT-alloc dominated) | 999999 | d1d8 |

These numbers, node counts, and moves are what every step below is compared against.

---

## Step 1 — Result: legalMoves() converted to make/unmake, but this alone was a REGRESSION, not a win

Converted `Board::legalMoves()` from `Board copy = *this; copy.makeMove(m); if (!copy.isInCheck(color)) ...`
to in-place `makeMove(m, undo)` / `isInCheck(color)` / `unmakeMove(m, undo)`, matching
the same pattern used for `negamax`/`quiescence` in the prior pass. Since
`legalMoves()` is `const` but needs to mutate `*this` transiently, this uses
`const_cast<Board&>(*this)` — sound specifically because a repo-wide search
confirmed no `Board` anywhere in this codebase is ever declared with genuine
`const` storage (every `const Board` occurrence is a `const Board&`
*reference* to a non-const object); mutating a truly-const object, even
temporarily, would be undefined behavior. This is a real, if currently only
theoretical, constraint on future code and is called out in a comment at the
call site.

**Correctness verification:**
- Perft suite: byte-identical at every depth already tested (both the
  copy-based and make/unmake-walked cross-check).
- `--verify-eval`: zero mismatches, 4.3M positions.
- **The specific trap named in the instructions** — getting `isInCheck(color)`'s
  argument backwards (checking the side *not* just moved instead of the side
  that moved) — was verified explicitly, not assumed. Built a pinned-rook
  position (`k3r3/8/8/8/8/8/4R3/4K3 w - - 0 1`: White King e1, Rook e2 pinned
  by Black Rook e8) where the correct legal-move count is exactly 10 (4 king
  moves + 6 on-file rook moves; the 7 sideways rook moves are pseudo-legal
  but illegal, since they'd expose the king). The real implementation
  produces exactly 10, with none of the 7 illegal moves present. To confirm
  the test itself was actually discriminating (not vacuously passing), I
  deliberately flipped the argument to `isInCheck(opponent(color))` in a
  throwaway build and confirmed the test **fails hard** on that version (15
  moves, 6 of the 7 illegal moves wrongly included) — so the test is a real
  check, not a rubber stamp.
- ASan+UBSan: zero errors across the full perft suite and a depth-5 bench run.

**Performance: this step alone made things slower, not faster.** A rigorous
back-to-back A/B test in the same session (`git stash` / rebuild / bench /
`git stash pop` / rebuild / bench, 3 runs each side, no other load on the
machine) found:

| Position | Baseline (copy) | Step 1 (make/unmake) | Change |
|---|---|---|---|
| startpos | 274,094 nps | 270,346 nps | −1.4% |
| middlegame | 346,611 nps | 331,776 nps | **−4.3%** |
| endgame | 235,838 nps | 233,861 nps | −0.8% |

Node counts and chosen moves were identical in both directions — this is a
pure performance comparison, not a correctness difference.

**Why this happened, verified rather than assumed:** the premise that
`Board`'s per-candidate copy was the expensive part was wrong. `Board` is a
small, entirely stack-resident struct (an 8x8 `Piece` array plus a handful
of ints/bools/uint64_t — no heap allocation at all), so `Board copy = *this`
compiles to a cheap, cache-friendly bulk copy — likely a few nanoseconds.
`makeMove(m, undo)`, by contrast, does meaningfully more work per candidate:
Zobrist hash XOR toggling, incremental material+PST score updates (multiple
`pieceContribution()` calls with table lookups and division), and
castling-rights/en-passant hash bookkeeping — all of which `legalMoves()`
immediately throws away, since it only needs updated piece positions to run
`isInCheck()`. The regression is worst on the middlegame position because it
has the highest branching factor (most pseudo-legal moves per node), so the
per-candidate overhead is paid the most times.

## Step 2 — isInCheck() redundant-work finding, and the fix (per your direction)

`isInCheck(color)` did an O(64) linear scan of the entire board just to
*locate* `color`'s king, before ever calling `isSquareAttacked()`. But
`Board` already tracks `kingX[2]`/`kingY[2]` as incrementally-maintained
private fields (added in the prior pass for O(1) king-PST evaluation) — the
king's square was already known for free. `isSquareAttacked()` itself (pawn/
knight/king/sliding-piece ray checks) looks like the right complexity for
this architecture; true incremental attack detection would be a much larger
redesign, not "cheap and low-risk," so no further changes were made there.

Asked you directly whether to fix this given it's directly relevant to the
Step 1 regression; you said yes. Change: replaced the O(64) scan with a
direct `isSquareAttacked(kingX[ci], kingY[ci], opponent(color))` lookup.

**Verification:** perft suite (byte-identical, all depths), `--verify-eval`
(zero mismatches), the pinned-rook test re-run and still passing (confirms
castling-adjacent and check-detection call sites, which also route through
`isInCheck()`, remain correct), ASan+UBSan (zero errors, including on the
`kingX[colorIndex(color)]` array indexing this change introduced).

## Step 3 — Final benchmark and honest verdict

| Position | Baseline | Step 1 only | Step 1+2 (final) | Net vs baseline |
|---|---|---|---|---|
| startpos | 274,094 nps | 270,346 nps | 279,870 nps | **+2.1%** |
| middlegame (Kiwipete) | 346,611 nps | 331,776 nps | 357,318 nps | **+3.1%** |
| endgame | 235,838 nps | 233,861 nps | 232,088 nps | **−1.6%** |

(Each column is an average of 3 back-to-back clean runs; node counts and
moves — `g1f3`/`e2a6`/`b4f4` — are byte-identical to baseline in every
column, confirmed via `--bench 6`.)

**This is a smaller win than the framing at the start of this pass
suggested, and on the endgame position it's a net loss.** The Step 1
regression turned out to be real and was only partially offset by Step 2's
fix. The isInCheck() fix pays off in proportion to branching factor (more
candidate moves per `legalMoves()` call means more O(64)→O(1) savings), so
it recovers Step 1's loss and adds a small net gain on startpos/middlegame
(which have 20-35 legal moves per position) but can't fully offset Step 1's
fixed per-candidate overhead on the endgame position (very few pieces, low
branching factor, so `isInCheck()`'s savings are smaller relative to
`makeMove`'s fixed cost per candidate).

**Overall speedup multiplier from this pass: ~1.02-1.03x on branching-heavy
positions, ~0.98x (a regression) on sparse/endgame positions.** This is
nowhere near what "the largest remaining lever" framing implied. The honest
conclusion is that `Board`'s copy was never actually expensive here — it was
already about as fast as a POD-struct copy can be — and the real win of the
make/unmake pattern (established in the prior pass) is specific to deeply
*recursive* search nodes where the SAME position needs many sequential
mutations across a whole subtree, not a single-level fan-out like
`legalMoves()` where each candidate is immediately discarded after one
check. A faster version of this specific optimization would need a
lightweight variant of `makeMove` that skips the hash/eval bookkeeping
entirely (since `legalMoves()` needs neither), but that's a larger, separate
change from what was asked for in Step 1, and wasn't implemented here.

---

# Tier 2b: Lightweight make/unmake for legalMoves() — the fix the prior pass identified but didn't implement

Starting state confirmed to match the prior pass's final recorded state
(perft, `--verify-eval`, `--bench 6` all reproduced its numbers before any
change here).

## Step 1 — Implementation

Added `Board::makeMoveLight()` / `unmakeMoveLight()` (private, `LightUndo`
struct) that update **only** piece placement and `kingX`/`kingY` tracking —
no Zobrist hash, no incremental eval score, no castling-rights or
en-passant-target bookkeeping. `LightUndo` ended up needing just two fields
(`movedType`, `movedColor`) — everything else needed to reverse a move is
already recoverable from the `Move` object itself (`m.captured` for the
captured piece type, `opponent(movedColor)` for its color, `m.fromX`/`fromY`
for the king's square to restore on unmake, whether castling or not, since
both a plain king move and a castling move record the king's origin square
there identically).

`legalMoves()` now calls `makeMoveLight`/`unmakeMoveLight` instead of the
full `makeMove`/`unmakeMove`. No other call site does this per-candidate
pattern (confirmed again in the prior pass and unchanged since), so no other
site needed updating. `negamax`/`quiescence`/`searchRootFull` were not
touched.

En passant and castling handling were written by directly mirroring the
existing full `makeMove`/`unmakeMove`'s piece-placement logic (same branch
structure, same square arithmetic), just with the hash/eval/rights-tracking
lines removed — this minimizes the chance of introducing a *new* logic
error, since the geometry itself is copied from already-perft-verified code
rather than re-derived from scratch.

## Step 2 — Verification, with maximum suspicion

| Check | Result |
|---|---|
| Perft suite, all depths previously tested | **PASS** — byte-identical (both copy-based `perft()` and unmake-walked `perftUnmake()`) |
| Perft suite pushed further: Kiwipete depth 5 added (193,690,690 nodes, the standard published value for this FEN — not previously in the suite since depth 4 was judged sufficient in the prior pass; added here given this pass's "maximum suspicion" instruction) | **PASS** — 193,690,690 / 193,690,690 / 193,690,690 (actual/unmake/expected), exact match, ~37s |
| Pinned-rook test from the prior pass (`k3r3/8/8/8/8/8/4R3/4K3`, expects exactly 10 legal moves) | **PASS** — 10/10, no illegal pin-violating moves present |
| New test: forced en passant response to check | **PASS** — see below |
| New test: illegal castle (attacked destination) excluded, legal castle (elsewhere) included | **PASS** — see below |
| `--verify-eval`, depth 4, all 3 positions | **PASS** — zero mismatches, 4.3M positions. This is the check that specifically confirms the lightweight path never leaks into the incremental eval state used by the *full* `makeMove` path in real search |
| ASan + UBSan: full perft suite + depth-5 `--bench` | **PASS** — zero sanitizer errors (would catch a `LightUndo` that left stale/garbage state on an incompletely-reversed move) |

**Forced en passant test.** Position:
`k6b/8/8/5pP1/8/8/3n4/K1n5 w - f6 0 1` — White King a1, Pawn g5; Black King
a8, Bishop h8 (checking along the a1-h8 diagonal), Pawn f5 (just
double-pushed, en passant target f6, which lies exactly on that diagonal),
Knight c1 (guards a2), Knight d2 (guards b1). Every other candidate is
illegal: Ka2/Kb1 are attacked by the knights, Kb2 sits directly on the check
diagonal, and g5-g6 doesn't block the diagonal so check persists. Only
`gxf6 e.p.` lands the white pawn on f6, blocking the bishop. Result:
`legalMoves(White)` returned exactly 1 move, `g5f6` marked `isEnPassant`.
**PASS.**

**Castling test.** Position: `4k1r1/8/8/8/8/8/8/R3K2R w KQ - 0 1` — White
King e1 with both rooks and both castling rights; Black Rook g8 attacks
straight down the open g-file onto g1, the kingside castle's destination
square. Queenside is untouched. Result: kingside castle absent, queenside
castle present. **PASS.**

One honest note on this second test, found during construction rather than
assumed: the kingside exclusion is enforced by `addKingMoves()`'s existing
pre-generation `isSquareAttacked()` check (unmodified, pre-existing code,
not part of this pass) — an illegal-by-attacked-square castle is never even
added to the pseudo-legal move list, so it never reaches `legalMoves()`'s
new lightweight make/unmake path at all. I verified this by reading
`addKingMoves()` and reasoning through why a rook moving during castling
can't reveal a *new* attack on the king's path (it can't, for the geometric
reason that a corner rook's own vacated square is never on the same
rank/file/diagonal as the king's e1-to-c1-or-g1 path). The lightweight
make/unmake **is** still genuinely exercised by this test, though: the
*legal* queenside castle is still a pseudo-legal candidate that
`legalMoves()` applies and reverses via `makeMoveLight`/`unmakeMoveLight`
(moving both king and rook, then restoring both) before confirming it's
safe — that's the code path this pass actually added, and it had to get
both piece moves right for the test to pass.

## Step 3 — Benchmark and honest verdict

Same back-to-back A/B methodology as the prior pass (`git stash` / rebuild /
bench / `git stash pop` / rebuild / bench, 3 runs each side, no other load
on the machine):

| Position | Prior pass final (reported) | This session's prior (re-measured) | This pass (lightweight) | vs. this-session prior | vs. original Tier 1 baseline |
|---|---|---|---|---|---|
| startpos | 279,870 nps | 276,441 nps | 324,620 nps | **+17.4%** | **+18.4%** |
| middlegame (Kiwipete) | 357,318 nps | 346,629 nps | 450,163 nps | **+29.9%** | **+29.9%** |
| endgame | 232,088 nps | 237,796 nps | 244,649 nps | **+2.9%** | **+3.7%** |

Node counts (100,732 / 1,256,733 / 41,856 / 39) and chosen moves
(`g1f3` / `e2a6` / `b4f4` / `d1d8`) are byte-identical to every prior pass —
confirmed via `--bench 6`, not assumed.

**This closes the endgame regression from the prior pass, and reverses it
into a net gain.** The prior pass ended at −1.6% vs. the original Tier 1
baseline on the endgame position; this pass lands at +3.7% vs. that same
baseline. Startpos and middlegame — already net-positive after the prior
pass — get a much larger additional gain here (+16-30% vs. the prior pass's
own reported final numbers). This is not a marginal result: it's the
dominant win of the two Tier-2 passes combined, and directly confirms the
diagnosis from the prior pass's honest write-up — the actual overhead was
never `Board`'s copy, it was the wasted Zobrist-hash and incremental-eval
bookkeeping that a full `makeMove` does on every candidate even though
`legalMoves()` throws it all away one `isInCheck()` call later.

## Files changed

- `include/board.hpp` — `LightUndo` struct, `makeMoveLight`/`unmakeMoveLight`
  declarations (both private)
- `src/board.cpp` — `makeMoveLight`/`unmakeMoveLight` implementations;
  `legalMoves()` switched to call them instead of the full `makeMove`/`unmakeMove`
- `src/main.cpp` — added the known-correct Kiwipete depth-5 perft value
  (193,690,690) to the permanent suite, for a deeper safety net going forward

The two new hand-verified positions (forced en passant, castling
attacked-square exclusion) and the pinned-rook re-check were run as
standalone throwaway programs, consistent with how the prior pass's
transposition and TT-persistence checks were handled — one-off verifications
of specific claims, not committed as permanent test infrastructure.

## Was this worth doing?

**Yes, clearly**, unlike the prior pass's Step 1 on its own. The amount of
new hand-written logic was small (two ~30-line functions, both built by
mechanically stripping lines from already-verified code rather than writing
new logic from scratch) and the verification burden it demanded was
substantial but tractable (perft to depth 5 including Kiwipete, two new
hand-constructed edge-case positions, ASan/UBSan, `--verify-eval`) — all of
which passed cleanly on the first implementation. Weighed against a
consistent 17-30% throughput gain on branching-heavy positions and a full
reversal of the only remaining regression, the size of the win clearly
justifies the size of the change. The main residual risk is exactly what
the verification was designed to catch: hand-rolled undo logic for
en passant and castling — and that risk was retired empirically (perft
byte-identical to 193M+ nodes on the position richest in exactly those
cases), not just argued away.

---

# Tier 2 Strength Pass: PVS, Aspiration Windows, Null-Move, LMR

Unlike every prior pass, this one changes search *behavior*, not just
speed — items were required to be committed one at a time so a regression
could be pinned to a single change. Ground rules carried over: perft/
`--verify-eval`/ASan-UBSan on every item, before/after node counts via the
established within-session A/B methodology, plain regression/wash/win
statements, and no Elo claims (no controlled same-time-control match was
run — see "What wasn't done" below).

Starting state confirmed to match the repo's committed HEAD
(`a3135df`, "changed check legal moves to the same solution move and
unmove") before any item here.

## Items implemented, each its own commit

| # | Item | Commit | Node-count effect at depth 6 (cumulative) | Move/score |
|---|---|---|---|---|
| 0 | Quiescence TT probe/store (decided to include — lower risk, good process-validation step per the prompt's own framing) | `1d5a68f` | startpos 100,732→98,131; middlegame 1,256,733→1,023,972 (**-18.5%**); endgame 41,856→40,994 | Identical |
| 1 | Principal Variation Search | `487eaff` | startpos →94,503; middlegame →869,765 (**-15.1%** further); endgame →39,469 | Identical |
| 2 | Aspiration windows | `25d90a8` | startpos →94,474; middlegame →867,984 (**-0.2%**, small at depth 6 — see commit message); endgame →39,337 | Identical |
| 3 | Null-move pruning | `e0d12a9` | startpos →44,649 (**-52.7%**); middlegame →660,790 (**-23.9%**); endgame →20,575 (**-47.7%**) | Identical |
| 4 | Late Move Reductions | `71896b9` | startpos →28,057 (**-37.2%**); middlegame →398,352 (**-39.7%**); endgame →19,474 (**-5.4%**) | Identical (one depth-5-only score shift on endgame, explained in commit, reproduced identically in ASan and regular builds — not a bug) |

Mate-in-1 stayed at 39 nodes and the correct move/score (`d1d8`,
`score=999999`) through every single item — the position is too shallow for
any of these techniques to engage, which is itself a useful negative
control: nothing here is silently corrupting the trivial case.

## Cumulative result (item 4 vs. the pre-this-pass baseline)

| Position | Baseline nodes | Final nodes | Reduction |
|---|---|---|---|
| startpos | 100,732 | 28,057 | **-72.1%** |
| middlegame (Kiwipete) | 1,256,733 | 398,352 | **-68.3%** |
| endgame | 41,856 | 19,474 | **-53.5%** |
| tactical (mate-in-1) | 39 | 39 | unchanged |

Every one of the five items (0-4) was a genuine, individually-verified win
— none were a wash or a regression, unlike the very first Tier 2 attempt in
the prior pass. This is the honest result, not a rounded-up one: I looked
specifically for a case where an item made things worse (that's exactly
what happened with the original `legalMoves()` make/unmake attempt) and
didn't find one here.

## Per-item verification, all confirmed (detail in each commit message)

- **Perft suite, `--verify-eval`**: unchanged after every item (expected —
  none of these touch move generation or the evaluation function, only
  search order/pruning) and confirmed after every item regardless, not
  skipped on the assumption it would be fine.
- **ASan/UBSan**: clean after every item.
- **Tactical correctness**: mate-in-1 solved correctly after every item.
  Null-move pruning (the highest-risk item) got the most scrutiny specific
  to its own failure mode: `hasNonPawnMaterial()` unit-tested directly
  across four positions (K+P-only, king-only, the real K+R+P benchmark
  position, and a K+N position), and the guard's real-world effect was
  demonstrated empirically — 5,963 nodes with the guard active vs. 2,536
  with it artificially removed on a K+P-only position at depth 10, proving
  it actually changes behavior rather than being dead code.
- **Honest gap on LMR**: no bespoke hand-constructed "quiet move tactical
  trap" position was built beyond the mate-in-1 check and the four
  benchmark positions' move-choice stability, because hand-verifying such a
  position with real confidence turned out to need either deep personal
  calculation or an independent strong engine — neither reliably available
  here. Flagging this rather than presenting the mate-in-1 check as
  covering more than it does.

## What wasn't done

- **SEE (item 5)**: explicitly optional/lowest-priority in the brief and
  the largest remaining chunk of work; asked whether to continue past
  items 1-4 and was told to stop and report instead. Not implemented.
- **TT replacement policy two-tier scheme**: the brief's own stated trigger
  for this ("if node-count wins from items 1-4 come in lower than
  expected") didn't occur — the wins were large (53-72% cumulative
  reduction), not disappointing — so there's no evidence-based case for it
  from this pass. Left as a candidate for a future pass if a *different*
  signal (e.g. TT hit-rate telemetry) motivates it.
- **No Elo claim.** Every number in this section is a node count or a
  fixed-position move/score check, exactly per the ground rules ("No
  claimed Elo gain without a controlled comparison"). A same-time-control
  self-play match against a fixed opponent set was not run — building that
  harness (opponent selection, enough games for statistical significance,
  results tracking) is substantial infrastructure on its own and was
  outside what this pass's time budget covered. The node-count reductions
  here are evidence of *efficiency* (reaching a given depth faster), which
  is a prerequisite for strength gains at a fixed time control but is not
  itself proof of them — a pruning bug that skips a genuinely relevant line
  would also reduce node counts while making the engine weaker, which is
  exactly why the tactical sanity checks above (not the node counts) are
  the real correctness evidence in this report, and why no Elo number
  appears anywhere in it.

## Files changed

- `include/board.hpp`, `src/board.cpp` — `Board::makeNullMove`/
  `unmakeNullMove`/`hasNonPawnMaterial` (item 3 only; items 0, 1, 2, 4 are
  entirely within `src/ai.cpp`, the search layer)
- `src/ai.cpp` — quiescence TT, PVS, aspiration windows, null-move pruning
  integration, LMR
