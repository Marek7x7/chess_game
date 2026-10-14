# Analysis scripts

Python helpers for judging engine decisions with Stockfish. They need a scratch
directory `$S` (never committed) holding the official Stockfish release at
`$S/sf/stockfish/stockfish-ubuntu-x86-64-avx2` and a venv with `python-chess` and
`pandas` (`$S/venv`). Run them as `$S/venv/bin/python tools/analysis/<script>.py`.

| Script | What it does |
|---|---|
| `grade.py $S <pgn dir> <out.csv>` | Stockfish (depth 14) grades every move of every PGN: eval before and after, centipawn loss, phase, piece. |
| `extract.py <games.pgn> <out.csv> <prefix>` | One position per move pair from self-play games (skips the book and the last plies), for tuning data. |
| `label.py $S <positions.csv> <out.csv> <depth>` | Stockfish-labels quiet positions (not in check, best move not a capture/promotion, no mate scores). Input of `build-tune/chess_tune`. |
| `mkpositions.py $S <repo> <out.csv>` | Rebuilds `tools/test_positions.csv` from the Komodo and Lichess games (`game3-7.pgn`, gitignored) and the graded self-play blunders. |
| `ab.py` (needs `S`) | A/B decision replay: each build picks a move per test position at a fixed depth or time, Stockfish grades every distinct move (cached in `$S/sfcache.json`). Reports moves changed, better/worse, average loss, big mistakes. |
| `sym.py <engine.sh> $S` | Search-based colour-symmetry check (noisy; `build-tune/chess_tune --symmetry` is the exact one). |
| `build_ref.sh <git-ref>` | Builds a ref in a temporary worktree into `$S/bins/<hash>.bin`. |

`tools/test_positions.csv`: the fixed decision-replay set -- 101 positions from the
games against Komodo 25, 29 from a Lichess game against AI level 8
(`lichess.org/Blummo9v`, lost on move 45), and 665 moves from the 200 HARD-vs-HARD
self-play games that lost >= 150 cp by Stockfish. Columns: source, game, move, fen,
played, sf_best, sf_eval, played_loss, note.

## Latest decision replay (2026-10-15)

`ab.py` on all 795 positions, original engine (`cfff7e5`) against the final head, Threads=1:

| Set | Fixed depth 10: avg loss, big mistakes (>= 80 cp) | 1 s per move: avg loss, big mistakes |
|---|---|---|
| Komodo games (101) | 29.5 -> 29.9 cp, 17 -> 16 | 30.4 -> 27.6 cp, 15 -> 13 |
| Lichess game vs AI level 8 (29) | 38.8 -> 32.0 cp, 3 -> 3 | 67.7 -> 26.8 cp, 7 -> 3 |
| Self-play blunders (665, chosen from the old build's mistakes) | 157.4 -> 84.3 cp, 421 -> 210 | 155.2 -> 73.9 cp, 420 -> 180 |

At fixed depth the two engines choose similar moves on positions not picked from the old
build's mistakes; the difference is speed and evaluation, which shows at a fixed time.
At the Lichess game's blunder (move 27) the original plays Nc1 (276 cp loss), the final
head Stockfish's own g3. A decision replay of this size cannot see an Elo change; the SPRT is
the verdict.
