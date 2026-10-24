#!/usr/bin/env python3
"""Relabels training records (include/nnue_record.hpp) with Stockfish scores.

  tools/nnue/relabel_sf.py <stockfish> <in.bin> <out.bin> [--nodes 30000] [--procs 22] [--limit N]

Each position goes to one of --procs Stockfish processes (1 thread, `go nodes N`); the record keeps its game
result and ply, gets the Stockfish score (side to move's point of view) and depth = 99 as the marker. Positions
Stockfish scores as a mate are dropped. Castling rights and en passant are not in a record, so Stockfish sees
none (the same as the network).
"""
import struct, subprocess, sys
from multiprocessing import Pool

REC = struct.Struct("<Q16shbBBBH")  # occupancy, pieces, score, result, stm, depth, ply, reserved


def fen(occ, pieces, stm):
    grid = [[""] * 8 for _ in range(8)]  # [rank][file]
    n = 0
    for sq in range(64):
        if occ >> sq & 1:
            nib = pieces[n // 2] >> (4 * (n % 2)) & 15
            n += 1
            c = "pnbrqk"[(nib & 7)]
            grid[sq % 8][sq // 8] = c.upper() if nib < 8 else c
    rows = []
    for rank in range(7, -1, -1):
        row, empty = "", 0
        for c in grid[rank]:
            if c:
                row, empty = row + (str(empty) if empty else "") + c, 0
            else:
                empty += 1
        rows.append(row + (str(empty) if empty else ""))
    return "/".join(rows) + (" b" if stm else " w") + " - - 0 1"


def work(args):
    sf, nodes, chunk = args
    p = subprocess.Popen([sf], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    p.stdin.write("uci\nsetoption name Threads value 1\nsetoption name Hash value 16\nisready\n")
    p.stdin.flush()
    while p.stdout.readline().strip() != "readyok":
        pass
    out = []
    for raw in chunk:
        occ, pieces, _, result, stm, _, ply, res = REC.unpack(raw)
        p.stdin.write(f"position fen {fen(occ, pieces, stm)}\ngo nodes {nodes}\n")
        p.stdin.flush()
        score = None
        for line in iter(p.stdout.readline, ""):
            if line.startswith("info") and " score " in line:
                t = line.split()
                kind, val = t[t.index("score") + 1], int(t[t.index("score") + 2])
                score = val if kind == "cp" else None
            elif line.startswith("bestmove"):
                break
        if score is not None:
            out.append(REC.pack(occ, pieces, max(-32000, min(32000, score)), result, stm, 99, ply, res))
    p.stdin.write("quit\n")
    p.stdin.flush()
    return b"".join(out)


if __name__ == "__main__":
    sf, src, dst = sys.argv[1:4]
    opt = dict(zip(sys.argv[4::2], sys.argv[5::2]))
    nodes, procs, limit = int(opt.get("--nodes", 30000)), int(opt.get("--procs", 22)), int(opt.get("--limit", 0))
    data = open(src, "rb").read()
    recs = [data[i:i + 32] for i in range(0, len(data) - 31, 32)]
    if limit:
        recs = recs[:limit]
    size = 20000  # records per task: small enough to show progress, big enough to amortise the engine start
    tasks = [(sf, nodes, recs[i:i + size]) for i in range(0, len(recs), size)]
    done = 0
    with Pool(procs) as pool, open(dst, "wb") as f:
        for blob in pool.imap(work, tasks):
            f.write(blob)
            done += 1
            print(f"{min(done * size, len(recs))}/{len(recs)}", file=sys.stderr, flush=True)
