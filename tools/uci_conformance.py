#!/usr/bin/env python3
"""UCI conformance and robustness checks for the engine (python3 stdlib only).

  python3 tools/uci_conformance.py build-release/chess

Every case is a way a GUI or tournament manager can hurt an engine: a position
with no legal move, a malformed FEN, absurd option values, a clock that has run
out, stdin closing in the middle of a search. The engine must answer, stay
alive and exit cleanly. Exit status 0 only if every case passes.

Set CHESS_TEST_BIG_HASH=1 to also try `Hash 100000000` (clamped to 4096 MB, so
it needs about 2 GB of memory).
"""
import os
import queue
import re
import subprocess
import sys
import threading
import time

BESTMOVE = re.compile(r"^bestmove (?:[a-h][1-8][a-h][1-8][qrbn]?|0000)(?: ponder [a-h][1-8][a-h][1-8][qrbn]?)?$")
MATE = "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"        # black to move, stalemated
BAD_FENS = ["garbage", "", "9999999/8/8/8/8/8/8/8 w - - 0 1", "8/8/8/8/8/8/8/8 w - - 0 1",
            "8/8/8/8/8/8/8/4K3 w - - 0 1", "P3k3/8/8/8/8/8/8/4K3 w - - 0 1",
            "4k3/8/8/8/8/8/8/4R1K1 w - - 0 1",
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQxq - 0 1"]


class Engine:
    def __init__(self, path):
        self.p = subprocess.Popen([path, "--uci"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.q = queue.Queue()
        self.log = []  # every line the engine wrote, for the sanitizer check in close()
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.p.stdout:
            self.log.append(line.rstrip("\n"))
            self.q.put(line.rstrip("\n"))

    def send(self, cmd):
        self.p.stdin.write(cmd + "\n")
        self.p.stdin.flush()

    def read_until(self, prefix, timeout=20):
        """Lines up to and including the first one starting with prefix (None as the last entry on timeout)."""
        out, end = [], time.time() + timeout
        while time.time() < end:
            try:
                line = self.q.get(timeout=0.05)
            except queue.Empty:
                continue
            out.append(line)
            if line.startswith(prefix):
                return out
        out.append(None)
        return out

    def drain(self, wait=0.0):
        time.sleep(wait)
        out = []
        while not self.q.empty():
            out.append(self.q.get())
        return out

    def alive(self):
        return self.p.poll() is None

    def ready(self):
        self.send("isready")
        return self.read_until("readyok", 10)[-1] == "readyok"

    def close(self):
        try:
            self.send("quit")
        except Exception:
            pass
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.p.kill()
        time.sleep(0.05)
        # An engine built with -fsanitize=... reports on stderr, which is merged into the stream.
        bad = [l for l in self.log if "Sanitizer" in l or "runtime error:" in l]
        assert not bad, "sanitizer report: " + bad[0]


def start(path, options=()):
    e = Engine(path)
    e.send("uci")
    e.read_until("uciok")
    for o in options:
        e.send("setoption name " + o)
    return e


def bestmove(lines):
    last = lines[-1] if lines else None
    return last if last and BESTMOVE.match(last) else None


def last_depth_info(lines):
    infos = [l for l in lines if l and l.startswith("info depth")]
    return infos[-1] if infos else ""


CASES = []


def case(fn):
    CASES.append(fn)
    return fn


@case
def handshake(path):
    e = Engine(path)
    e.send("uci")
    out = e.read_until("uciok")
    e.close()
    text = "\n".join(l for l in out if l)
    assert out[-1] == "uciok", "no uciok"
    for opt in ("Hash", "Threads", "OwnBook", "Move Overhead", "Ponder", "MultiPV", "UCI_LimitStrength", "UCI_Elo",
                "SyzygyPath", "SyzygyProbeDepth"):
        assert f"option name {opt} " in text, f"option {opt} not advertised"


@case
def search_returns_a_move(path):
    e = start(path, ["Threads value 1"])
    e.send("position startpos moves e2e4 e7e5")
    e.send("go depth 4")
    out = e.read_until("bestmove")
    e.close()
    assert bestmove(out) and bestmove(out) != "bestmove 0000", f"bad answer {out[-1]!r}"
    info = last_depth_info(out)
    for field in (" time ", " nodes ", " nps ", " hashfull ", " pv "):
        assert field in info, f"info line lacks{field!r}: {info!r}"


@case
def no_legal_move_positions(path):
    e = start(path, ["Threads value 1"])
    for fen in (MATE,):
        e.send(f"position fen {fen}")
        e.send("go depth 3")
        assert bestmove(e.read_until("bestmove")) == "bestmove 0000", "stalemate: no bestmove 0000"
    e.send("position startpos moves f2f3 e7e5 g2g4 d8h4")  # fool's mate
    e.send("go movetime 100")
    assert bestmove(e.read_until("bestmove")) == "bestmove 0000", "checkmate: no bestmove 0000"
    assert e.alive() and e.ready(), "engine died or stopped answering"
    e.close()


@case
def bad_fens_are_survived(path):
    e = start(path, ["Threads value 1"])
    for fen in BAD_FENS:
        e.send(f"position fen {fen}")
        e.send("go depth 2")
        out = e.read_until("bestmove")
        assert e.alive(), f"engine died on FEN {fen!r}"
        assert bestmove(out) == "bestmove 0000", f"FEN {fen!r} was accepted: {out[-1]!r}"
    e.send("position startpos")  # a valid position recovers
    e.send("go depth 2")
    assert bestmove(e.read_until("bestmove")) not in (None, "bestmove 0000"), "no recovery after bad FENs"
    e.send("position fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -")  # four fields only
    e.send("go depth 2")
    assert bestmove(e.read_until("bestmove")) not in (None, "bestmove 0000"), "four-field FEN rejected"
    e.close()


@case
def absurd_options(path):
    e = start(path, ["Threads value 1"])
    values = ["Hash value lots", "Hash value -5", "Hash value 0", "Threads value xyz", "Threads value 99999999",
              "Threads value -3", "Move Overhead value -1", "No Such Option value 1"]
    if os.environ.get("CHESS_TEST_BIG_HASH"):
        values.append("Hash value 100000000")
    for v in values:
        e.send("setoption name " + v)
        assert e.alive() and e.ready(), f"engine died or hung on setoption {v!r}"
    e.send("setoption name Threads value 1")
    e.send("position startpos")
    e.send("go depth 3")
    assert bestmove(e.read_until("bestmove", 60)), "no move after the option abuse"
    e.close()


@case
def exhausted_clock(path):
    for go in ("go wtime -100 btime 1000", "go wtime 0 btime 0", "go movetime 0", "go btime 5000 wtime 50 movestogo 1"):
        e = start(path, ["Threads value 1"])
        e.send("position startpos")
        t0 = time.time()
        e.send(go)
        out = e.read_until("bestmove", 10)
        e.close()
        assert bestmove(out), f"{go!r}: no bestmove"
        assert time.time() - t0 < 3, f"{go!r}: took {time.time() - t0:.1f} s"


@case
def node_limit(path):
    e = start(path, ["Threads value 1"])
    e.send("position startpos")
    t0 = time.time()
    e.send("go nodes 50000")
    out = e.read_until("bestmove", 30)
    e.close()
    assert bestmove(out), "no bestmove"
    assert time.time() - t0 < 5, "go nodes ran far too long"
    nodes = [int(m.group(1)) for l in out if l for m in [re.search(r" nodes (\d+) ", l)] if m]
    assert nodes and max(nodes) <= 50000, f"searched past the limit: {nodes}"


@case
def searchmoves(path):
    e = start(path, ["Threads value 1"])
    e.send("position startpos")
    e.send("go depth 5 searchmoves a2a3")
    assert bestmove(e.read_until("bestmove")) == "bestmove a2a3", "searchmoves a2a3 ignored"
    e.send("go depth 5 searchmoves a2a3 b1c3")
    assert bestmove(e.read_until("bestmove")) in ("bestmove a2a3", "bestmove b1c3"), "searchmoves pair ignored"
    e.close()


@case
def go_depth_is_exact(path):
    e = start(path, ["Threads value 1"])
    e.send("position fen 8/8/4k3/8/8/4K3/8/8 w - - 0 1")
    e.send("go depth 6")
    out = e.read_until("bestmove")
    e.close()
    depths = [int(m.group(1)) for l in out if l for m in [re.match(r"info depth (\d+) ", l)] if m]
    assert depths and max(depths) == 6, f"go depth 6 reached depth {max(depths) if depths else None}"


@case
def infinite_waits_for_stop(path):
    e = start(path, ["Threads value 1"])
    e.send("position fen 6k1/5ppp/8/8/8/8/8/R3K3 w - - 0 1")
    e.send("go infinite depth 4")  # a search that is over in milliseconds
    early = e.drain(1.5)
    assert not any(l.startswith("bestmove") for l in early), "bestmove before stop"
    e.send("stop")
    assert bestmove(e.read_until("bestmove", 5)), "no bestmove after stop"
    e.close()


@case
def eof_during_search(path):
    e = start(path, ["Threads value 2"])
    e.send("position startpos")
    e.send("go infinite")
    time.sleep(0.3)
    e.p.stdin.close()
    try:
        rc = e.p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        e.p.kill()
        raise AssertionError("still running 10 s after stdin closed")
    assert rc == 0, f"exit status {rc}"


@case
def go_while_searching(path):
    e = start(path, ["Threads value 1"])
    e.send("position startpos")
    e.send("go infinite")
    time.sleep(0.3)
    e.send("go depth 3")  # a GUI that forgot "stop"
    first = e.read_until("bestmove", 10)
    second = e.read_until("bestmove", 10)
    e.close()
    assert bestmove(first) and bestmove(second), "a second go while searching hung or lost a bestmove"


@case
def multithreaded_search(path):
    e = start(path, ["Threads value 4", "Hash value 32"])
    for fen in ("startpos", "fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"):
        e.send("ucinewgame")
        e.send("position " + fen)
        e.send("go movetime 800")
        assert bestmove(e.read_until("bestmove", 20)), "no bestmove with 4 threads"
        e.send("go movetime 500")  # the second search starts from the table and heuristics of the first
        assert bestmove(e.read_until("bestmove", 20)), "no bestmove from the warm table"
    e.send("go infinite")
    time.sleep(0.5)
    e.send("stop")
    assert bestmove(e.read_until("bestmove", 10)), "no bestmove after stop with 4 threads"
    e.close()


@case
def new_game_and_moves(path):
    e = start(path, ["Threads value 1"])
    for _ in range(2):
        e.send("ucinewgame")
        e.send("position startpos moves e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 e2e9")  # the last move is illegal
        e.send("go depth 3")
        assert bestmove(e.read_until("bestmove")), "no move after ucinewgame/position"
    e.close()


@case
def ponder_waits_for_ponderhit_then_answers(path):
    e = start(path, ["Threads value 2", "Ponder value true"])
    e.send("position startpos moves e2e4 e7e5")
    e.send("go ponder wtime 30000 btime 30000")
    early = e.read_until("bestmove", 1.5)
    assert early[-1] is None, "answered before ponderhit"
    t0 = time.time()
    e.send("ponderhit")
    out = e.read_until("bestmove", 6)
    took = time.time() - t0
    e.close()
    assert bestmove(out), f"no bestmove after ponderhit: {out[-3:]}"
    assert " ponder " in out[-1], f"no ponder move in {out[-1]}"
    assert took < 3.5, f"took {took:.1f}s after ponderhit with 30 s on the clock"


@case
def ponder_stop_and_depth_limit(path):
    e = start(path, ["Threads value 1", "Ponder value true"])
    e.send("position startpos")
    e.send("go ponder wtime 30000 btime 30000")
    e.drain(0.5)
    e.send("stop")
    assert bestmove(e.read_until("bestmove", 3)), "stop did not end the ponder search"
    e.send("go ponder depth 3")  # finishes at once, but must wait for ponderhit
    assert e.read_until("bestmove", 1.0)[-1] is None, "depth-limited ponder answered before ponderhit"
    e.send("ponderhit")
    out = e.read_until("bestmove", 3)
    e.close()
    assert bestmove(out), "no bestmove after ponderhit"


@case
def no_ponder_move_unless_enabled(path):
    e = start(path, ["Threads value 1"])
    e.send("position startpos moves e2e4 e7e5")
    e.send("go depth 8")
    out = e.read_until("bestmove", 20)
    e.close()
    assert bestmove(out) and "ponder" not in out[-1], f"unexpected: {out[-1]}"


def multipv_lines(out, depth=None):
    """{multipv index: (depth, first pv move)} from the info lines (the last depth's, if depth is None)."""
    rows = []
    for l in out:
        m = re.match(r"^info multipv (\d+) depth (\d+) .* pv ([a-h][1-8][a-h][1-8][qrbn]?)", l or "")
        if m:
            rows.append((int(m.group(2)), int(m.group(1)), m.group(3)))
    if not rows:
        return {}
    d = depth if depth is not None else max(r[0] for r in rows)
    return {k: (dd, mv) for dd, k, mv in rows if dd == d}


@case
def multipv_reports_distinct_lines(path):
    e = start(path, ["Threads value 1", "MultiPV value 3"])
    e.send("position startpos moves e2e4 e7e5")
    e.send("go depth 6")
    out = e.read_until("bestmove", 30)
    e.close()
    assert bestmove(out), "no bestmove"
    lines = multipv_lines(out)
    assert sorted(lines) == [1, 2, 3], f"expected lines 1-3 at the last depth, got {sorted(lines)}"
    assert lines[1][0] == 6, f"deepest lines are at depth {lines[1][0]}, not 6"
    firsts = [lines[k][1] for k in (1, 2, 3)]
    assert len(set(firsts)) == 3, f"first moves are not distinct: {firsts}"
    assert out[-1].split()[1] == firsts[0], f"bestmove {out[-1]} is not line 1 ({firsts[0]})"


@case
def multipv_with_fewer_legal_moves_than_lines(path):
    e = start(path, ["Threads value 1", "MultiPV value 5"])
    e.send("position fen 7k/8/8/2b5/8/8/8/7K w - - 0 1")  # two legal moves: Kg2 and Kh2 (the bishop covers g1)
    e.send("go depth 4")
    out = e.read_until("bestmove", 10)
    assert bestmove(out), f"no bestmove: {out[-3:]}"
    assert sorted(multipv_lines(out)) == [1, 2], f"expected two lines, got {sorted(multipv_lines(out))}"
    e.send("position fen 7k/8/8/8/8/8/6q1/7K w - - 0 1")  # one legal move: Kxg2
    e.send("go depth 4")
    out = e.read_until("bestmove", 10)
    e.close()
    assert bestmove(out) and out[-1].split()[1] == "h1g2", f"expected h1g2, got {out[-1]}"


@case
def multipv_stop_and_movetime(path):
    e = start(path, ["Threads value 1", "MultiPV value 4"])
    e.send("position startpos")
    t0 = time.time()
    e.send("go movetime 600")
    out = e.read_until("bestmove", 5)
    took = time.time() - t0
    assert bestmove(out), "no bestmove for movetime"
    assert took < 2.0, f"movetime 600 took {took:.1f}s"
    e.send("go infinite")
    e.drain(0.6)
    e.send("stop")
    assert bestmove(e.read_until("bestmove", 3)), "stop did not end the MultiPV search"
    e.close()


@case
def syzygy_options_survive_bad_input(path):
    e = start(path, ["Threads value 1"])
    e.send("setoption name SyzygyPath value /no/such/directory")
    e.send("setoption name SyzygyProbeDepth value 99999")
    e.send("setoption name SyzygyPath value <empty>")
    e.send("position startpos moves e2e4 e7e5")
    e.send("go depth 5")
    assert bestmove(e.read_until("bestmove", 20)), "no bestmove after the Syzygy options"
    e.close()


@case
def limited_strength_plays_and_is_cheaper(path):
    e = start(path, ["Threads value 1", "UCI_LimitStrength value true", "UCI_Elo value 1800"])
    e.send("position startpos moves e2e4 e7e5")
    t0 = time.time()
    e.send("go movetime 2000")
    out = e.read_until("bestmove", 6)
    took = time.time() - t0
    e.close()
    assert bestmove(out), f"no bestmove: {out[-2:]}"
    assert took < 1.0, f"a 1800-rated search used {took:.1f}s of a 2 s budget (it should be node-capped)"


@case
def own_book_answers_known_positions_at_once(path):
    e = start(path, ["Threads value 1", "OwnBook value true"])
    for moves in ("", "e2e4 c7c5", "d2d4 g8f6 c2c4 e7e6"):
        e.send("position startpos" + (" moves " + moves if moves else ""))
        t0 = time.time()
        e.send("go movetime 2000")
        out = e.read_until("bestmove", 5)
        took = time.time() - t0
        assert bestmove(out), f"no bestmove after '{moves}': {out[-2:]}"
        assert took < 0.5, f"after '{moves}' the book position took {took:.1f}s (a search, not a book move)"
    e.close()


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = os.path.abspath(sys.argv[1])
    failed = 0
    for fn in CASES:
        t0 = time.time()
        try:
            fn(path)
            print(f"PASS {fn.__name__} ({time.time() - t0:.1f}s)")
        except AssertionError as err:
            failed += 1
            print(f"FAIL {fn.__name__}: {err}")
        except Exception as err:  # a crashed pipe etc.
            failed += 1
            print(f"FAIL {fn.__name__}: {type(err).__name__}: {err}")
    print("ALL PASS" if not failed else f"{failed} of {len(CASES)} FAILED")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
