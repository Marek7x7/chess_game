#!/usr/bin/env python3
"""SPSA tuning of the search constants declared with SEARCH_PARAM
(include/search_params.hpp), using the chess_spsa build and fastchess.

Each iteration perturbs every parameter by +-c in a random direction, plays
the two perturbed configurations against each other, and moves the
parameters toward the winner (gradient ascent on the score).

  cmake --build build-release --target chess_spsa
  python3 tools/spsa.py --bin build-release/chess_spsa --state spsa_state.json \
      --iterations 500 --rounds 30 --tc 3+0.03 --concurrency 20

The state file holds the current parameters and iteration, so a run can be
stopped and resumed. Parameter ranges come from the engine's own UCI options.
"""
import argparse, json, math, os, random, re, shutil, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FASTCHESS = os.path.join(REPO, "tools/fastchess-src/fastchess")
OPENINGS = os.path.join(REPO, "tools/openings/8moves_v3.pgn")


def read_options(binary):
    out = subprocess.run([binary, "--uci"], input="uci\nquit\n", capture_output=True, text=True).stdout
    params = {}
    for m in re.finditer(r"option name (sp\w+) type spin default (-?\d+) min (-?\d+) max (-?\d+)", out):
        params[m.group(1)] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
    return params


def play(binary, a, b, args):
    """Score of config a against config b in [-1, 1] (wins - losses over games)."""
    def eng(name, cfg):
        opts = [f"option.{k}={v}" for k, v in cfg.items()]
        return ["-engine", f"cmd={binary}", "args=--uci", f"name={name}", "option.Threads=1",
                "option.Hash=32", "option.OwnBook=false"] + opts
    cmd = [FASTCHESS] + eng("plus", a) + eng("minus", b) + [
        "-each", f"tc={args.tc}", "-openings", f"file={OPENINGS}", "format=pgn", "order=random",
        "-rounds", str(args.rounds), "-repeat", "-concurrency", str(args.concurrency), "-recover",
        "-config", "outname=" + os.path.join(REPO, ".sprt-work", "spsa-config.json")]  # not ./config.json
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    games = re.findall(r"Games: (\d+), Wins: (\d+), Losses: (\d+), Draws: (\d+)", out)
    if not games:
        raise RuntimeError("no result from fastchess:\n" + out[-500:])
    n, w, l, d = map(int, games[-1])
    return (w - l) / n, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--state", default="spsa_state.json")
    ap.add_argument("--iterations", type=int, default=500)
    ap.add_argument("--rounds", type=int, default=30, help="opening pairs per iteration (2 games each)")
    ap.add_argument("--tc", default="3+0.03")
    ap.add_argument("--concurrency", type=int, default=20)
    ap.add_argument("--a0", type=float, default=0.04, help="step size, in fractions of a parameter's range")
    ap.add_argument("--c0", type=float, default=0.06, help="perturbation, in fractions of a parameter's range")
    args = ap.parse_args()

    work = os.path.join(REPO, ".sprt-work")
    os.makedirs(work, exist_ok=True)
    binary = os.path.join(work, "spsa.bin")
    shutil.copy(args.bin, binary)  # a rebuild must not change the engine mid-run
    info = read_options(binary)
    if os.path.exists(args.state):
        state = json.load(open(args.state))
    else:
        state = {"k": 0, "theta": {k: float(v[0]) for k, v in info.items()}}
    A = 0.1 * args.iterations
    rng = random.Random(12345 + state["k"])

    while state["k"] < args.iterations:
        k = state["k"]
        ak = args.a0 / (A + k + 1) ** 0.602 * (A + 1) ** 0.602
        ck = args.c0 / (k + 1) ** 0.101
        plus, minus = {}, {}
        for name, (_, lo, hi) in info.items():
            rng_ = hi - lo
            step = max(1.0, ck * rng_)  # an integer parameter needs a whole-number gap
            d = rng.choice((-1, 1))
            t = state["theta"][name]
            plus[name] = int(min(hi, max(lo, round(t + d * step))))
            minus[name] = int(min(hi, max(lo, round(t - d * step))))
        r, n = play(binary, plus, minus, args)
        for name, (_, lo, hi) in info.items():
            gap = plus[name] - minus[name]  # signed: carries the perturbation direction, after rounding/clamping
            if gap == 0:
                continue
            g = r / (gap / (hi - lo))  # score per unit of normalised separation
            state["theta"][name] = min(hi, max(lo, state["theta"][name] + ak * g * (hi - lo)))
        state["k"] = k + 1
        json.dump(state, open(args.state, "w"), indent=1)
        cur = " ".join(f"{n_[2:]}={v:.0f}" for n_, v in state["theta"].items())
        print(f"iter {k+1}/{args.iterations} r={r:+.3f} ({n} games) {cur}", flush=True)


if __name__ == "__main__":
    sys.exit(main())
