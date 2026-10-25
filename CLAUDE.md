# Chess engine — rules for Claude

C++17 chess engine: mailbox 8x8 board, negamax/PVS + quiescence, shared
transposition table, Lazy SMP, SDL GUI, UCI mode, opening book, and an NNUE
evaluation (768 -> 256x2 -> 1, SCReLU, 8 output buckets; 512 was tested and lost) embedded from
`nnue/default.nnue`; the hand-crafted evaluation below is the fallback
(`setoption name EvalFile value none`, or `-DCHESS_NNUE=OFF`). The ongoing
work is a phased effort to raise its playing strength. **Every change must be
proven by a match test before it is kept.**

## Build and run

- Release build: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release`
  (`-Wall -Wextra -Werror`, `-march=native`). Binary: `build-release/chess`.
  CMake options (README.md has the table): `CHESS_GUI=OFF` builds a UCI-only engine without SDL2,
  `CHESS_PORTABLE=ON` a self-contained executable (Windows: `-DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw.cmake`
  or `cmake/mingw-w64.cmake`). MSVC is not supported.
- **All rule-3 gates in one command:** `ctest --test-dir build-release --output-on-failure` (perft against
  published counts, `--verify-eval 4`, the bench signature in `tools/bench_signature.txt`, and
  `tools/uci_conformance.py`). A change that is meant to alter the search updates `bench_signature.txt` in the
  same commit; a speedup or refactor must leave it alone. The signature also depends on the C++ library:
  `std::sort`'s order among equal scores differs between libstdc++ and libc++, so a clang/libc++ build prints a
  different number (different numbers, no longer current: the gcc/libstdc++ value is in `tools/bench_signature.txt`) with the same source.
- No argument opens the GUI. Headless modes: `--uci`, `--bench [d]`,
  `--perft [d]`, `--perft-suite [d]`, `--verify-eval [d]`, `--verify-book`,
  `--replay <pgn> [white|black|both]`, `--selfplay <games> [levelA] [levelB]`.
- NNUE: `include/nnue.hpp`/`src/nnue.cpp` (accumulators in `Board`, checked by `--verify-eval`),
  trainer and data generator in `tools/nnue/` (README.md there). `tools/nnue/auto.sh --work <dir> --hours N`
  retrains unattended: self-play data -> train -> SPRT vs the current net, rounds until one fails twice.
  Shipping a net = copy to `nnue/default.nnue`, rebuild, update `tools/bench_signature.txt`, `ctest`, commit.
  Self-play chain Elo overstates real strength (HCE -> shipped net: +360 direct, about +444 summed over the rounds).
- Where things are (hand-crafted fallback): static eval = `evaluate()` in `src/ai.cpp`:
  `Board::rawEval()` (incremental material and piece-square tables, with
  separate endgame tables, king table, bishop pair) + the cached pawn
  structure (`computePawnInfo()`) + `pieceTerms()` (bitboard mobility,
  pins, threats, safe checks, rooks, outposts) + `kingSafety()` + passed-pawn
  terms + mop-up + tempo; a dead draw scores 0. Every weight is declared with
  a `TUNE_` macro (`include/tune.hpp`). Search = `negamax()`,
  `quiescence()`, `searchRootFull()`, `findBestMove()` in `src/ai.cpp`.
  `Board` keeps bitboards next to `squares[][]`, written only by
  `setSquare()`; `src/bitboard.cpp` holds the attack tables.
  UCI = `src/uci.cpp`. Bot matches = `src/match.cpp`.
- Tuning: `cmake -S . -B build-tune -DCMAKE_BUILD_TYPE=Release && cmake --build
  build-tune --target chess_tune` builds `tools/tune.cpp` (Texel-style fit of
  every `TUNE_` weight to Stockfish evaluations; see its header comment for
  `--symmetry` and `--evals`). `tools/analysis/` (README.md) has the Stockfish
  scripts for labelling positions and grading moves, and
  `tools/test_positions.csv` is the fixed decision-replay set. Tuning data =
  quiet positions from fresh self-play (fastchess 3+0.03), labelled with
  Stockfish depth 12.
- Real games vs chess.com bots: `game1.pgn`-`game6.pgn` at the repo root
  (gitignored). Engine is White in 1/2/5/6, Black in 3/4; games 3-6 are vs
  Komodo25 (3200). Re-check the headers before relying on this mapping.

## Hard rules

1. **No change is kept without a passing SPRT.** Related changes are tested
   together as a bundle (e.g. all new search pruning, or all king-safety
   terms), one fast match per bundle:
   `CONCURRENCY=20 tools/sprt.sh <new-ref|WORKTREE> <base-ref> 5+0.05 0 10`
   (Threads=1, Hash=64, OwnBook=false, `8moves_v3.pgn`). Keep a bundle only
   when fastchess reports "H1 was accepted"; if it fails, split it and test
   the parts to find the culprit. Use bounds `-3 1` for speedups and
   simplifications expected to be neutral. Changes that only matter with
   several threads (hash table, thread handling) run with
   `THREADS=2 CONCURRENCY=10`. `tools/openings/mini_suite.pgn` is for smoke
   tests only and never counts. (Until 2026-09-28 every change had its own
   match at 10+0.1, bounds 0/5; the user switched to bundles to save time.)
2. **One change per commit**, also inside a bundle: every commit builds and
   passes the gates, and its message cites its bundle's match result.
3. **Gates after every change:** `--perft-suite 5` and `--verify-eval 4`
   print ALL PASS, and the build has zero warnings. Pure speedups/refactors
   must leave `--bench 6`'s `BENCH SIGNATURE` line byte-identical.
4. **Eval terms must be color-symmetric:** a position and its color-flipped
   mirror must score exactly opposite. Anything tracked incrementally in
   `Board` must also be recomputed in `rawEvalFromScratch()`, so
   `--verify-eval` checks it.
5. **Log every match (one row per bundle) in `STRENGTH_LOG.md`**, including
   REVERTED and INCONCLUSIVE ones, with the exact numbers fastchess prints, in a separate
   commit after the change (the row cites the change's hash). Keep
   `benchmark_baseline.md` separate: it's a historical log that never
   claims Elo.
6. **Git:** work on a feature branch (currently `phase7-polish`, branched off `phase6-nnue`; phases 2-5 are merged or stacked beneath it), never commit
   to `main` directly, never push. The user merges and pushes. Commit only
   when the user has asked for commits. Message: a subject line, then a body
   with why, the SPRT line (Elo ± error, games, LLR), the fixed-position
   summary and the gate results.
7. **Verify claims against the source.** Quote the actual code, and don't
   call something fixed or resolved unless it was tested.

## Testing workflow for every change

1. **Does it work:** build with zero warnings, run the gates, spot-check a
   few hand-computed eval values, and run the mirror-symmetry check over
   every position of the real games.
2. **Did its decisions change:** replay the same fixed positions with the
   old and the new build. Use FEN via UCI `position fen` after
   `ucinewgame`, at a fixed `go depth 10` with Threads=1, so reruns are
   identical (for search changes, where a fixed depth means different
   things in the two builds, a fixed `go movetime 1000` instead). This is
   the scripted equivalent of the GUI's custom placement.
   Use every engine-to-move position from move 13 on in the Komodo games
   (where Stockfish's eval is within ±5 pawns), plus a few hand-made test
   positions. Grade every move where the builds disagree with Stockfish
   (depth 18, `searchmoves` restricted). Report: moves changed, better vs
   worse, average centipawn loss, big mistakes (≥ 80 cp).
3. **Is it stronger:** the bundle's SPRT (rule 1). Run it in the
   background, **give the user a status update every 20 minutes** (games,
   Elo ± error, LLR, ETA), and never run two matches at once, since they
   compete for CPU and distort time-based games.
4. To test uncommitted work against an uncommitted baseline, snapshot the
   baseline tree as a dangling commit through a temporary index
   (`GIT_INDEX_FILE=... git read-tree HEAD; git add -A; git write-tree;
   git commit-tree`) and pass that hash to `tools/sprt.sh`.
5. Tools outside the repo: Stockfish is not installed. Download the
   official release (`stockfish-ubuntu-x86-64-avx2`) into a scratch
   directory, and use python-chess from a scratch venv. Never commit either.

## Roadmap (update this list as items are done)

Everything on the original list is done, all SPRT-accepted -- exact numbers
per change in `STRENGTH_LOG.md`:
- **Eval:** passed pawns; doubled/isolated pawns + bishop pair; mobility;
  king safety (shelter, king-zone attacks); rooks on open files and the
  7th rank; outposts; backward pawns; endgame pawn values; pawn cache.
- **Search:** captures-only quiescence moves; log-based late-move
  reductions; SEE + delta pruning in quiescence; root PVS; futility +
  reverse futility pruning; mate distance pruning; fifty-move rule;
  soft/hard time limits in clock games; thread 0's result; lock-free
  hash table with two-slot buckets and aging.
- **Other:** the GUI's human-vs-computer mode passes the game history
  (repetition detection, validated at +55); reported PVs end at a
  threefold.
- **Done 2026-10-12..15 (autonomous session, rows in `STRENGTH_LOG.md`):**
  Texel tuning of the eval weights against Stockfish (six rounds; the loop
  stopped when round 7 found no gain), including material values and the new
  terms below; speedups with identical search trees (MoveList, sort,
  legality filter, 1-byte enums, aligned/prefetched TT, bitboards for
  attacks/mobility/pins/SEE/move generation, 8-byte Move, LTO, windowed
  repetition scan) and lazy legality checks: 2.6M -> about 6M nodes/s, worth
  +112, +122 and +31 Elo at 5+0.05; endgame fixes (dead draw scores 0,
  mop-up toward the edge or the right corner); new eval terms (tempo, rook
  behind passer, passer king distance, pawn storm, pin-aware mobility, pawn-safe
  mobility, threats, safe checks, connected pawns, blocked passers, bad
  bishop, endgame piece-square tables) and endgame scaling (opposite-coloured
  bishops, and by the pawn count of the side ahead: one of the biggest single
  loss reductions, -11% validation loss); search (null move only where eval >=
  beta with a depth-scaled reduction, SEE-ordered captures -- worth 79 Elo,
  an ablation showed --, history with gravity and malus, history in LMR,
  counter-moves, late-move pruning, singular extensions, time management by
  best-move stability, gradual aspiration windows). Re-checked against the
  start of the session: +578 at 10+0.1 and +573 with 2 threads.
- **Done 2026-10-20 (phase4-robustness, autonomous session; no search change, bench signature unchanged):**
  UCI robustness (terminal positions, FEN validation, option clamping, negative clocks, EOF, `go infinite`,
  `go nodes`, `searchmoves`, total node counts in `info`, `go depth` exact), the LMR `std::clamp` UB at depth 2
  (`spLmrMinDepth` is now 3..7), six more perft positions, `ctest`, `tools/uci_conformance.py`, ASan/UBSan/TSan
  clean runs, the GitHub Actions workflow, the llvm-mingw Windows build (UCI suite passes under Wine),
  `CHESS_GUI`/`--version`/`--help`, `sprt.sh` pins `model=normalized` (the bounds are nElo, not logistic Elo).
- **Done 2026-10-20..22 (phase6-nnue / phase7-polish, rows in `STRENGTH_LOG.md`):** NNUE (own trainer and data
  generator, `auto.sh` retraining loop, round-7 net shipped as `nnue/default.nnue`: +360 vs the hand-crafted
  evaluation, direct); SPSA re-tune of the 24 search constants for the net (+10.9, kept); two same-coloured
  bishops are not mating material (+4.9, kept); White's book moves limited to those near the best weight
  (functional check, no SPRT possible). Anchor: about 3090 on the CCRL 40/4 scale vs Stockfish `UCI_Elo`
  (`tools/anchor.sh`, 1 thread, 60+0.6), against about 2650 before the net.
- **Done 2026-10-22 (phase7-polish):** search bundles on the net: razoring + correction history (by pawn key) +
  ProbCut (+40.5, kept) and history pruning + quiet-move SEE pruning + double/negative singular extensions (+18.5,
  kept); UCI Ponder, MultiPV, UCI_LimitStrength/UCI_Elo (node caps calibrated against Stockfish levels with
  `tools/calibrate_elo.sh`), Syzygy through Fathom (`third_party/fathom`, checked against python-chess with
  `tools/syzygy_check.py`); GUI: draw rules, undo, flip, FEN copy/paste, PGN save, clocks (`tools/gui_test/` drives it
  headlessly); `tools/package.sh` and `.github/workflows/release.yml` (not run on GitHub; chess.exe cross-built, not
  run: no Wine here). The ci.yml of before did not parse (a colon in a step name): validate workflows with PyYAML.
- **Also 2026-10-22:** 256-wide net (+33.3) and its auto.sh retrain round 1 (+14.2, shipped; round 2 level at
  +0.00 +/- 7.3, stopped); 512-wide lost (-11.0); a 840k-game Lichess elite book (22k positions, `book/default.book`).
  Final anchor about 3098 vs Stockfish (3090 in the morning: **no measurable gain against Stockfish although
  self-play showed about +100 at 5+0.05 and +63 +/- 28 at 60+0.6** -- trust Stockfish-style anchors over self-play
  chains; the gap is unexplained). OpenMP training needs OMP_NUM_THREADS below the CPU count (6x).
  Ideas not started: king-bucketed inputs, a second hidden layer, SPSA of the search constants for the 256-wide net
  (they were tuned for 128), an eval/search pass that targets play against other engines rather than itself.
- Tried and not kept in that session: a second SPSA round (+1.5 +/- 4.0 over 12926 games, stop at the first
  failing round); the 4-slot TT with the en-passant hash fix (inconclusive at -2.9 +/- 6.4, Threads=2); a
  Threads 12 vs 22 default (22 gives 30-40% more nps but no clear depth gain, default left at hw-2).
- **Done 2026-10-23..24 (phase8-strength, rows in `STRENGTH_LOG.md`):** speed bundle with an identical search tree, +24% nps
  (+30.3 +/- 14.9 at 5+0.05; anchor about 3116 vs 3098 before, inside the noise): AVX-512(+VNNI) network kernel and
  `CHESS_AVX512` option, `Board::givesCheck` to skip make/unmake of pruned quiet moves, `Board::hashAfter` to prefetch
  the TT bucket before the make, 2 MiB huge-page TT, accumulator stack with fused updates (no 1 KB copies).
  `--verify-eval` also checks `givesCheck` and `hashAfter`. Tried and not kept: TT cutoffs only at non-PV nodes + no qsearch
  lazy cut under the net (-7.9), `futile` mate guard (-9.3), all four together (-4.5); NNUE data experiments E1/E2:
  book-start self-play data makes nets 76-231 Elo weaker (it lacks the unbalanced positions the search visits; keep the
  8 random plies), and Stockfish-relabelled data (-7% loss against Stockfish) costs 16-18 Elo in self-play, which
  cannot show a transfer gain (an anchor resolves only about +/-17 for two nets). Profile first: a gdb-sampling script
  found the TT probe stall (16% of time) that perf would have shown.
- Tried and not kept (flat or negative; do not retry blindly): contempt;
  internal iterative reduction + an 'improving' flag; quiescence without the
  transposition table (-20); ordering quiet checking moves early (-38);
  capture history + continuation history (flat); a softer time limit (flat);
  a pawn-count adjustment for knights/rooks, a free-path bonus for passers,
  a global eval scale, a rook-ending scale, a queen-less king-attack scale
  (all screened out by the tuner); an edge-distance rewrite of
  `isSquareAttacked` (slower). (SEE pruning of captures in the main search was
  rejected at first and kept on 2026-10-16, +18.6, commit `0c80606`.) Lazy evaluation in quiescence and the history
  term in LMR tested neutral by ablation and stay.
- Checked and left as is: the GUI keeps a fixed time per move -- with no
  clock known to the engine, using the whole budget is strongest; the
  endgame depth bonus in `findBestMove()` only matters when the depth cap
  binds (EASY/MEDIUM/HARD), which is where it's meant to.

Ideas for next time, not started:
1. The evaluation is the bottleneck still (the tuner's validation loss no
   longer moves with weights alone): new terms found by screening are what
   paid off (endgame scaling, safe checks). Candidates: a better king-safety
   model, space, unstoppable passers (needs mate-free labels), more endgame
   scaling cases (rook endings, pawns on one wing).
2. Speed keeps paying at fast time controls (about 2 Elo per 1% at
   5+0.05): the remaining profile is evaluation (~30%), move ordering with
   SEE (~14%) and pseudo-legal generation (~11%).
3. The GUI's presets cap the depth (HARD = 10), so the engine's added
   speed does not make HARD stronger; EXPERT and clock games benefit.
4. The bundles above were accepted whole, so which parts of the search
   changes carry the gain is partly unknown (singular extensions, LMP,
   counter-moves, null-move conditions); ablate them with -3/1 SPRTs.
5. Scratch space: keep a long session's logs out of a small /tmp (the SPRT
   stdout is hundreds of MB; filter it as in the session's scripts).
