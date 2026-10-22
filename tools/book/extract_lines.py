#!/usr/bin/env python3
"""Counts how often each opening move sequence occurs in PGN games (needs python-chess).

  python3 tools/book/extract_lines.py --out lines.txt [--plies 18] [--min-count 20] games.pgn|games.zip ...

Each output line is a move sequence in UCI and how many games played it: "e2e4 e7e5 g1f3 4120". The last move
is the book move for the position reached by the others. Only sequences that occurred at least --min-count times
are written. Feed the file to `chess --build-book lines.txt book/elite.bin`, which turns the sequences into
the engine's own position hashes (so transpositions add up there).
"""
import argparse, io, sys, zipfile
from collections import Counter

import chess
import chess.pgn


def games(path):
    if path.endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            for name in z.namelist():
                with z.open(name) as raw:
                    yield from read(io.TextIOWrapper(raw, encoding="utf-8", errors="replace"))
    else:
        with open(path, encoding="utf-8", errors="replace") as f:
            yield from read(f)


def read(handle):
    while True:
        game = chess.pgn.read_game(handle)
        if game is None:
            return
        yield game


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--plies", type=int, default=18)
    ap.add_argument("--min-count", type=int, default=20)
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()

    counts = Counter()
    n = 0
    for path in a.files:
        for game in games(path):
            board = game.board()
            path_moves = []
            for i, move in enumerate(game.mainline_moves()):
                if i >= a.plies:
                    break
                path_moves.append(move.uci())
                counts[" ".join(path_moves)] += 1
            n += 1
            if n % 50000 == 0:
                print(f"{n} games, {len(counts)} sequences", file=sys.stderr)
    kept = [(seq, c) for seq, c in counts.items() if c >= a.min_count]
    kept.sort(key=lambda t: (len(t[0].split()), t[0]))
    with open(a.out, "w") as out:
        for seq, c in kept:
            out.write(f"{seq} {c}\n")
    print(f"{n} games, {len(counts)} sequences, {len(kept)} written (>= {a.min_count})", file=sys.stderr)


if __name__ == "__main__":
    main()
