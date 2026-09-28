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
- Where things are: static eval = `Board::rawEval()` (incremental material,
  piece-square tables, king table, bishop pair) + `pawnStructure()` +
  `mobility()` in `src/ai.cpp`'s `evaluate()`. Search = `negamax()`,
  `quiescence()`, `searchRootFull()`, `findBestMove()` in `src/ai.cpp`.
  UCI = `src/uci.cpp`. Bot matches = `src/match.cpp`.
- Real games vs chess.com bots: `game1.pgn`-`game6.pgn` at the repo root
  (gitignored). Engine is White in 1/2/5/6, Black in 3/4; games 3-6 are vs
  Komodo25 (3200). Re-check the headers before relying on this mapping.

## Hard rules

1. **No change is kept without a passing SPRT.** Run
   `tools/sprt.sh <new-ref|WORKTREE> <base-ref>` (defaults: 10+0.1,
   Threads=1, Hash=64, OwnBook=false, elo0=0/elo1=5, `8moves_v3.pgn`).
   Keep a change only when fastchess reports "H1 was accepted". Use bounds
   `-3 1` for speedups and simplifications expected to be neutral.
   `tools/openings/mini_suite.pgn` is for smoke tests only and never counts.
2. **One change per commit.** Every commit builds and passes the gates. If
   two terms interact and are tested together (e.g. doubled/isolated pawns +
   bishop pair), commit them together and say why in the message.
3. **Gates after every change:** `--perft-suite 5` and `--verify-eval 4`
   print ALL PASS, and the build has zero warnings. Pure speedups/refactors
   must leave `--bench 6`'s `BENCH SIGNATURE` line byte-identical.
4. **Eval terms must be color-symmetric:** a position and its color-flipped
   mirror must score exactly opposite. Anything tracked incrementally in
   `Board` must also be recomputed in `rawEvalFromScratch()`, so
   `--verify-eval` checks it.
5. **Log every match in `STRENGTH_LOG.md`**, including REVERTED and
   INCONCLUSIVE ones, with the exact numbers fastchess prints, in a separate
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
   identical. This is the scripted equivalent of the GUI's custom placement.
   Use every engine-to-move position from move 13 on in the Komodo games
   (where Stockfish's eval is within ±5 pawns), plus a few hand-made test
   positions. Grade every move where the builds disagree with Stockfish
   (depth 18, `searchmoves` restricted). Report: moves changed, better vs
   worse, average centipawn loss, big mistakes (≥ 80 cp).
3. **Is it stronger:** the SPRT (rule 1). It takes 1-2 hours at about 23
   games/min. Run it in the background, **give the user a status update
   every 20 minutes** (games, Elo ± error, LLR, ETA), and never run two
   matches at once, since they compete for CPU and distort time-based games.
4. To test uncommitted work against an uncommitted baseline, snapshot the
   baseline tree as a dangling commit through a temporary index
   (`GIT_INDEX_FILE=... git read-tree HEAD; git add -A; git write-tree;
   git commit-tree`) and pass that hash to `tools/sprt.sh`.
5. Tools outside the repo: Stockfish is not installed. Download the
   official release (`stockfish-ubuntu-x86-64-avx2`) into a scratch
   directory, and use python-chess from a scratch venv. Never commit either.

## Roadmap (update this list as items are done)

Done, all SPRT-accepted: passed pawns (+34), doubled/isolated pawns +
bishop pair (+27), mobility (+54). The engine still rates its middlegame
positions about a pawn better than Stockfish does.

Next, in suggested order:
1. **GUI repetition fix:** the GUI's human-vs-computer search in `gui.cpp`
   doesn't set `SearchOptions::gameHistory`, so repetition detection is off
   in the mode used against chess.com (UCI and bot matches already pass it).
2. **Search (Phase 3):** log-based late-move reductions (currently a flat 1
   ply), SEE + delta pruning in `quiescence()`, captures-only move
   generation in quiescence (it currently generates all legal moves and
   filters), PVS at the root, futility pruning.
3. **King safety:** pawn shield, attackers near the king, open files toward
   the king.
4. **More eval:** rooks on open/semi-open files and the 7th rank, outposts,
   backward pawns, middlegame/endgame pawn piece-square values.
5. **Speed (Phase 4):** the from-scratch eval terms cost about 24% NPS. Do
   one board scan instead of several, and cache pawn-structure results.
   Every TT access takes a mutex, so threads scale poorly.
6. **Phase 1 leftovers:** TT replacement (a collision always overwrites,
   even a deeper entry), Lazy SMP result selection never tested against
   alternatives, repetition detection never SPRT-tested on its own, no
   50-move-rule awareness or mate distance pruning in search, PVs that
   continue past a threefold (fastchess warnings).
7. **Time management (Phase 5):** the GUI spends a fixed time per move, and
   the endgame depth bonus in `findBestMove()` does nothing at Expert
   (time runs out first).
