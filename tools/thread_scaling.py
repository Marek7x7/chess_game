#!/usr/bin/env python3
"""How much does each extra search thread buy? (python3 stdlib only)

  python3 tools/thread_scaling.py build-release/chess [--threads 1 2 4 8 12 22] [--ms 1000] [--positions 24]

Searches the same positions for the same time with each thread count (run it on an otherwise idle machine,
nothing else may use the cores) and reports the average depth reached by thread 0, the nodes per second summed
over all threads, and how often the move differs from the 1-thread search. A run that doubles the nps but
barely adds depth is a search that wastes its threads. Positions come from tools/test_positions.csv (the
middlegames and endgames of the real games), the rest from the bench set.
"""
import argparse
import csv
import os
import queue
import re
import subprocess
import threading
import time

BENCH_FENS = [
    "startpos",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
]


def positions(limit):
    here = os.path.dirname(os.path.abspath(__file__))
    fens = list(BENCH_FENS)
    with open(os.path.join(here, "test_positions.csv"), newline="") as f:
        rows = list(csv.DictReader(f))
    step = max(1, len(rows) // max(1, limit - len(fens)))
    fens += [("fen " + r["fen"]) for r in rows[::step]]
    return [(f if f == "startpos" or f.startswith("fen ") else "fen " + f) for f in fens][:limit]


class Engine:
    def __init__(self, path):
        self.p = subprocess.Popen([path, "--uci"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.q = queue.Queue()
        threading.Thread(target=lambda: [self.q.put(l.rstrip("\n")) for l in self.p.stdout], daemon=True).start()

    def send(self, cmd):
        self.p.stdin.write(cmd + "\n")
        self.p.stdin.flush()

    def until(self, prefix, timeout):
        out, end = [], time.time() + timeout
        while time.time() < end:
            try:
                line = self.q.get(timeout=0.05)
            except queue.Empty:
                continue
            out.append(line)
            if line.startswith(prefix):
                return out
        return out

    def quit(self):
        try:
            self.send("quit")
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def run(path, threads, ms, fens, hash_mb):
    e = Engine(path)
    e.send("uci")
    e.until("uciok", 10)
    e.send(f"setoption name Threads value {threads}")
    e.send(f"setoption name Hash value {hash_mb}")
    depths, nps, moves = [], [], []
    for fen in fens:
        e.send("ucinewgame")
        e.send("position " + fen)
        e.send(f"go movetime {ms}")
        out = e.until("bestmove", ms / 1000 + 20)
        infos = [l for l in out if l.startswith("info depth")]
        m = re.search(r"depth (\d+) .* nps (\d+)", infos[-1]) if infos else None
        depths.append(int(m.group(1)) if m else 0)
        nps.append(int(m.group(2)) if m else 0)
        moves.append(out[-1].split()[1] if out and out[-1].startswith("bestmove") else "?")
    e.quit()
    return depths, nps, moves


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("engine")
    ap.add_argument("--threads", type=int, nargs="+", default=[1, 2, 4, 8, 12, 22])
    ap.add_argument("--ms", type=int, default=1000)
    ap.add_argument("--positions", type=int, default=24)
    ap.add_argument("--hash", type=int, default=256)
    args = ap.parse_args()
    fens = positions(args.positions)
    print(f"{len(fens)} positions, {args.ms} ms each, Hash {args.hash} MB")
    print(f"{'threads':>7} {'avg depth':>9} {'vs 1 thread':>11} {'Mnps (all threads)':>19} {'same move as 1 thread':>22}")
    base_depth = base_moves = None
    for t in args.threads:
        depths, nps, moves = run(args.engine, t, args.ms, fens, args.hash)
        avg = sum(depths) / len(depths)
        if base_depth is None:
            base_depth, base_moves = avg, moves
        same = sum(a == b for a, b in zip(moves, base_moves))
        print(f"{t:>7} {avg:>9.2f} {avg - base_depth:>+11.2f} {sum(nps) / len(nps) / 1e6:>19.2f} {same:>14}/{len(moves)}")


if __name__ == "__main__":
    main()
