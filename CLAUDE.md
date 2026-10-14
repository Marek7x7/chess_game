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
- Tried and not kept (flat or negative; do not retry blindly): contempt;
  internal iterative reduction + an 'improving' flag; quiescence without the
  transposition table (-20); ordering quiet checking moves early (-38);
  capture history + continuation history (flat); a softer time limit (flat);
  a pawn-count adjustment for knights/rooks, a free-path bonus for passers,
  a global eval scale, a rook-ending scale, a queen-less king-attack scale
  (all screened out by the tuner); an edge-distance rewrite of
  `isSquareAttacked` (slower); SEE pruning in the main search (more nodes at
  fixed depth, never matched). Lazy evaluation in quiescence and the history
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
