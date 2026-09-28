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
