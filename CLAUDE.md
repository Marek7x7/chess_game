# Chess engine — rules for Claude

C++17 chess engine: mailbox 8x8 board, negamax/PVS + quiescence, shared
transposition table, Lazy SMP, SDL GUI, UCI mode, opening book. The ongoing
work is a phased effort to raise its playing strength. **Every change must be
proven by a match test before it is kept.**

## Build and run

- Release build: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release`
  (`-Wall -Wextra -Werror`, `-march=native`). Binary: `build-release/chess`.
- No argument opens the GUI. Headless modes: `--uci`, `--bench [d]`,
  `--perft [d]`, `--perft-suite [d]`, `--verify-eval [d]`, `--verify-book`,
  `--replay <pgn> [white|black|both]`, `--selfplay <games> [levelA] [levelB]`.
- Where things are: static eval = `evaluate()` in `src/ai.cpp`:
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
6. **Git:** work on a feature branch (currently `phase2-eval`), never commit
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
- **Done 2026-10-12..14 (autonomous session, rows in `STRENGTH_LOG.md`):**
  Texel tuning of the eval weights against Stockfish (five rounds, each
  validation-loss gain shown in the log; +40, +38, +10, +29 and a share of
  +58/+98 Elo at 5+0.05), material values; speedups with identical search
  trees (MoveList, sort, legality filter, 1-byte enums, aligned/prefetched
  TT, bitboard attacks/mobility/pins, 8-byte Move, LTO: 2.6M -> 5.6M nodes/s,
  +112 and +122 Elo); dead-draw and mop-up endgame fixes; new eval terms
  (tempo, rook behind passer, passer king distance, pawn storm, pin-aware
  mobility, threats, connected pawns, blocked passers, bad bishop, endgame
  piece-square tables); search (null move only where eval >= beta with a
  depth-scaled reduction, SEE-ordered captures, history with gravity and
  malus, history LMR, counter-moves, late-move pruning, singular extensions,
  time management by best-move stability, gradual aspiration windows).
  Re-checked at 10+0.1 (+408) and with 2 threads (+363) against the start
  of the session.
- Tried and not kept (flat or negative; do not retry blindly): contempt,
  internal iterative reduction + an 'improving' flag, a pawn-count
  adjustment for knights/rooks, a free-path bonus for passers, an
  edge-distance rewrite of `isSquareAttacked` (slower).
- Checked and left as is: the GUI keeps a fixed time per move -- with no
  clock known to the engine, using the whole budget is strongest; the
  endgame depth bonus in `findBestMove()` only matters when the depth cap
  binds (EASY/MEDIUM/HARD), which is where it's meant to.

Ideas for next time, not started:
1. Further tuning rounds give ~1-2% validation loss each now; the next
   step up needs new evaluation terms (king safety beyond safe checks,
   passed-pawn detail, endgame scaling for drawish material) -- screen a
   term by tuning only its weights and comparing the validation loss.
2. Speed: bitboard pseudo-legal move generation (keeping the move order so
   the bench signature stays identical), lazy legality checks, a static-eval
   cache; the Elo per speedup is steep at fast time controls.
3. The GUI's presets cap the depth (HARD = 10), so the engine's added
   speed does not make HARD stronger; EXPERT and clock games benefit.
4. Search ablations: the bundles above were accepted whole, so the parts
   that contribute nothing (SEE-ordered captures, history in LMR) are
   unknown.
