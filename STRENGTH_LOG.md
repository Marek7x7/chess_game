# Strength Log

Every row is one SPRT (or fixed-games match) result for one committed
change. A change is only kept if its result meets its SPRT bounds (LLR
crosses the accept-H1 threshold) -- "no change is kept without a proven
SPRT/match test result." Append one row per tested change; don't edit past
rows except to fix a transcription error.

Distinct from `benchmark_baseline.md`, which is a historical node-count/
correctness log that predates this file and explicitly never claims an Elo
number. This file is specifically the Elo/SPRT ledger going forward.

**A result produced with `tools/openings/mini_suite.pgn` is never valid
grounds for a KEPT/REVERTED verdict** -- that suite exists only for
`tools/smoke_test.sh`'s harness-plumbing checks. Every real row below must
cite `tools/openings/8moves_v3.pgn` (fetch with `tools/fetch_openings.sh`).

| Date | Change | Commit | Test settings | Result | Verdict |
|---|---|---|---|---|---|
| YYYY-MM-DD | one-line description | `newhash` vs `basehash` | TC=10+0.1, Hash=64MB, Threads=1, book=off, elo0=0/elo1=5, suite=8moves_v3 | Elo +X.X ± Y.Y (N games, LLR Z.ZZ) | KEPT / REVERTED / INCONCLUSIVE |
| 2026-09-27 | Passed-pawn bonus, tapered by rank | `b548941` vs `8ec47f2` | TC=10+0.1, Hash=64MB, Threads=1, book=off, elo0=0/elo1=5, suite=8moves_v3 | Elo +34.05 ± 13.20 (1904 games, LLR 2.96) | KEPT |
| 2026-09-27 | Doubled/isolated pawn penalties alone | never committed vs `b548941` | TC=10+0.1, Hash=64MB, Threads=1, book=off, elo0=0/elo1=5, suite=8moves_v3 | Elo +12.96 ± 16.83 (1180 games, LLR 0.62) | INCONCLUSIVE (stopped early; retested with the bishop pair, next row) |
| 2026-09-28 | Doubled/isolated pawn penalties + bishop-pair bonus | `5d881f6` vs `b548941` | TC=10+0.1, Hash=64MB, Threads=1, book=off, elo0=0/elo1=5, suite=8moves_v3 | Elo +26.61 ± 11.72 (2564 games, LLR 2.97) | KEPT |
| 2026-09-28 | Piece mobility (knight/bishop/rook/queen, Fruit 2.1 weights) | `882eed1` vs `5d881f6` | TC=10+0.1, Hash=64MB, Threads=1, book=off, elo0=0/elo1=5, suite=8moves_v3 | Elo +54.18 ± 16.90 (1196 games, LLR 2.95) | KEPT |
| 2026-09-28 | Noisy-only move generation in quiescence (pure speedup, identical tree) | `df54360` vs `5fcd1af` | TC=10+0.1, Hash=64MB, Threads=1, book=off, elo0=-3/elo1=1, suite=8moves_v3 | Elo +28.90 ± 11.21 (2470 games, LLR 2.96) | KEPT |
| 2026-09-28 | Log-based late-move reductions | `77f6f5e` vs `8ef8449` | TC=10+0.1, Hash=64MB, Threads=1, book=off, elo0=0/elo1=5, suite=8moves_v3 | Elo +16.76 ± 8.78 (3922 games, LLR 2.95) | KEPT |
| 2026-09-28 | SEE pruning in quiescence alone | never committed (`7cf714c`) vs `fdd9fb6` | TC=10+0.1, Hash=64MB, Threads=1, book=off, elo0=0/elo1=5, suite=8moves_v3 | Elo +17.62 ± 14.70 (1460 games, LLR 1.14) | INCONCLUSIVE (stopped: switched to bundles; retested in bundle 1, next row) |
| 2026-09-28 | Bundle 1, search pruning: SEE + delta pruning in quiescence, root PVS, futility + reverse futility pruning | `74e0805`..`3bb8fb0` vs `02668cd` | TC=5+0.05, Hash=64MB, Threads=1, book=off, concurrency=20, elo0=0/elo1=10, suite=8moves_v3 | Elo +222.75 ± 40.59 (198 games, LLR 2.95) | KEPT |
| 2026-09-28 | Bundle 2, king safety: king shelter + king-zone attacks | `a453582`..`b934e6c` vs `3474021` | TC=5+0.05, Hash=64MB, Threads=1, book=off, concurrency=20, elo0=0/elo1=10, suite=8moves_v3 | Elo +27.99 ± 16.54 (1356 games, LLR 2.95) | KEPT |
| 2026-09-28 | Bundle 3, more eval: rooks on open files and the 7th rank, outposts, backward pawns, endgame pawn values | `c68852a`..`c660b5c` vs `782c26d` | TC=5+0.05, Hash=64MB, Threads=1, book=off, concurrency=20, elo0=0/elo1=10, suite=8moves_v3 | Elo +27.08 ± 16.00 (1414 games, LLR 2.99) | KEPT |
| 2026-09-28 | Bundle 4, hash table: lock-free packed entries, two-slot buckets with aging | `35b8de0`..`c4ea07d` vs `c0d7477` | TC=5+0.05, Hash=64MB, Threads=2, book=off, concurrency=10, elo0=-3/elo1=1, suite=8moves_v3 | Elo +4.74 ± 4.88 (13194 games, LLR 2.95) | KEPT |
| 2026-09-28 | Bundle 5, time and misc: mate distance pruning, fifty-move rule in search, PVs ending at a threefold, soft/hard time limits, thread 0's result | `27bc5db`..`d122759` vs `3973251` | TC=5+0.05, Hash=64MB, Threads=2, book=off, concurrency=10, elo0=-3/elo1=1, suite=8moves_v3 | Elo +75.31 ± 17.36 (984 games, LLR 2.96) | KEPT |
| 2026-09-28 | Speed bundle: pawn-only hash key, pawn cache in the evaluation (identical evaluation, pure speedup) | `55eb793`..`ba4462d` vs `ca2052d` | TC=5+0.05, Hash=64MB, Threads=1, book=off, concurrency=20, elo0=-3/elo1=1, suite=8moves_v3 | Elo +24.81 ± 10.33 (2806 games, LLR 2.95) | KEPT |
| 2026-09-28 | Repetition detection in search (from `8ec47f2`, never tested on its own): with it vs the same code without it | `ba4462d` vs test-only build `3ccb4b6` | TC=5+0.05, Hash=64MB, Threads=1, book=off, concurrency=20, elo0=-3/elo1=1, suite=8moves_v3 | Elo +55.29 ± 14.00 (1166 games, LLR 2.95) | KEPT (validated) |

## Absolute strength estimates

Not SPRT rows: fixed-length matches against Stockfish 17.1 with
`UCI_LimitStrength` at a given `UCI_Elo` (calibrated by Stockfish at 120+1
and anchored to CCRL 40/4), both engines on 1 thread, Hash=64MB, book off,
`8moves_v3.pgn` with each opening played from both sides. Performance =
the Stockfish level + the Elo difference fastchess reports. These are on
a CCRL-like scale, not chess.com's. Played at 60+0.6 -- half the
calibration time control -- which likely understates this engine a
little (it gains more from time than depth-limited Stockfish does).

| Date | Engine | Opponent | TC | Result | Performance |
|---|---|---|---|---|---|
| 2026-09-28 | Original: the tree before this session's work (`8ec47f2`) | Stockfish, UCI_Elo 2050 | 60+0.6 | 110W 36L 4D, 74.7% of 150: Elo +187.77 ± 58.16 | ≈ 2240 (2180-2295) |
| 2026-09-28 | All of the above changes (`ba4462d`) | Stockfish, UCI_Elo 2500 | 60+0.6 | 101W 41L 8D, 70.0% of 150: Elo +147.19 ± 55.82 | ≈ 2650 (2590-2705) |
| 2026-10-13 | Bundle 4, search refinements from the 50 self-play games: history malus, late-move pruning (depth <= 3), history-aware LMR | `a35ffe1`..`42a8df5` vs `cfff7e5` | TC=5+0.05, Hash=64MB, Threads=1, book=off, concurrency=20, elo0=0/elo1=10, suite=8moves_v3 | Elo -10.66 ± 12.83 (2054 games, LLR -2.97) | REVERTED (H0 accepted; split below) |
| 2026-10-13 | History malus alone (split from bundle 4) | `a35ffe1` vs `cfff7e5` | TC=5+0.05, Hash=64MB, Threads=1, book=off, concurrency=20, elo0=0/elo1=10, suite=8moves_v3 | Elo -6.37 ± 10.54 (2838 games, LLR -2.99) | REVERTED (H0 accepted); the three commits were reverted in `e244f1a`, `9f32745`, `effd370`. Late-move pruning and history-aware LMR were not tested alone. |
| 2026-10-13 | Bundle 5, endgame eval from the 50 self-play games: pin-aware mobility, rook behind a passed pawn, king distance to passers | `d46104d`..`2134d60` vs `cfff7e5` | TC=5+0.05, Hash=64MB, Threads=1, book=off, concurrency=20, elo0=0/elo1=10, suite=8moves_v3 | Elo -27.74 ± 18.80 (954 games, LLR -2.95) | REVERTED (H0 accepted; split below) |
| 2026-10-13 | Pin-aware mobility alone (split from bundle 5) | `d46104d` vs `cfff7e5` | TC=5+0.05, Hash=64MB, Threads=1, book=off, concurrency=20, elo0=0/elo1=10, suite=8moves_v3 | Elo -12.87 ± 13.60 (1756 games, LLR -2.96) | REVERTED (H0 accepted) |
| 2026-10-13 | Rook behind passed pawn + king distance to passers, without pins (split from bundle 5) | `852532a` (`5555769` + `2134d60`, pins removed) vs `cfff7e5` | TC=5+0.05, Hash=64MB, Threads=1, book=off, concurrency=20, elo0=0/elo1=10, suite=8moves_v3 | Elo +1.60 ± 8.71 (4138 games, LLR -1.05) | INCONCLUSIVE (stopped by hand: flat, the +10 target is outside the error bar); not kept. All of it restored to the base in `16cd6eb`. |
| 2026-10-13 | Bundle 6, Texel-tuned eval weights: pawn structure, mobility, king safety, rook/queen/outposts, piece-square tables + bishop pair (fitted to Stockfish depth-12/16 evals of 108k quiet self-play positions; validation loss -4.7%) | `8e33b54`..`9445cba` vs `cfff7e5` | TC=5+0.05, Hash=64MB, Threads=1, book=off, concurrency=20, elo0=0/elo1=10, suite=8moves_v3 | Elo +40.71 ± 20.15 (866 games, LLR 2.98) | KEPT |
| 2026-10-13 | Bundle 7, second Texel round: all eval weights re-fitted on 274k quiet self-play positions from the tuned engine (Stockfish depth 12/16 labels; validation loss 0.023210 -> 0.022077, pruning pass), material values excluded | `ba5b247`..`2fac06a` vs `00f55ca` | TC=5+0.05, Hash=64MB, Threads=1, book=off, concurrency=20, elo0=0/elo1=10, suite=8moves_v3 | Elo +38.50 ± 18.91 (888 games, LLR 3.00) | KEPT |
| 2026-10-13 | Bundle 8, Texel-fitted material values (bishop 330 -> 332, rook 500 -> 541, queen 900 -> 980) | `d1daa1a` vs `d262ebc` | TC=5+0.05, Hash=64MB, Threads=1, book=off, concurrency=20, elo0=0/elo1=10, suite=8moves_v3 | Elo +9.81 ± 7.83 (5316 games, LLR 2.95) | KEPT |
| 2026-10-13 | Bundle 9, speedups with identical search trees: MoveList uninitialised storage + size-aware copies, orderMoves scores each move once, filterLegal skips the make/attack-scan for moves that cannot be illegal (startpos 1.21M -> 1.71M nodes/s, kiwipete 1.12M -> 1.66M) | `c84ccbe`..`e0215d3` vs `d12b5f3` | TC=5+0.05, Hash=64MB, Threads=1, book=off, concurrency=20, elo0=-3/elo1=1, suite=8moves_v3 | Elo +112.20 ± 21.16 (724 games, LLR 2.96) | KEPT |
| 2026-10-13 | Bundle 10, speedups with identical search trees: one-byte Piece/Color enums, single-compare inBounds, cache-aligned TT, TT prefetch, bitboards in Board + bitboard isSquareAttacked, bitboard mobility/king-zone in the eval, LTO (bench-9 median 3.67M -> 5.60M nodes/s, +52%) | `142eee9`..`f2a8b69` vs `e0215d3` | TC=5+0.05, Hash=64MB, Threads=1, book=off, concurrency=20, elo0=-3/elo1=1, suite=8moves_v3 | Elo +121.73 ± 23.01 (692 games, LLR 2.96) | KEPT |

Head to head at 10+0.1 over 200 games, the same two builds: +463.16 ±
86.13 Elo for the new one (181W 7L 12D, 93.5%).

## How to add a row

1. `tools/sprt.sh <new-ref> <base-ref> [tc] [elo0] [elo1]` (requires
   `tools/setup_fastchess.sh` and `tools/fetch_openings.sh` to have been run
   first).
2. Read fastchess's PASS/FAIL verdict from `.sprt-work/sprt.log` or its
   stdout summary.
3. Append a row with the exact numbers reported -- don't round or
   paraphrase.
4. KEPT: commit stays. REVERTED: `git revert` the commit -- log the row
   anyway; a documented negative result is exactly as valuable as a
   positive one here.
