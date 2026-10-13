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
