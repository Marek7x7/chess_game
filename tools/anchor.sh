#!/usr/bin/env bash
# Anchor the engine against Stockfish at fixed UCI_Elo levels (UCI_LimitStrength, calibrated by
# Stockfish at 120+1 and anchored to CCRL 40/4), the same method as the 2026-09-28 rows of
# STRENGTH_LOG.md: both engines on 1 thread, Hash=64, book off, 60+0.6, 8moves_v3.
#
#   tools/anchor.sh <stockfish-binary> <engine-binary> <out-dir> [level ...]
#
# Default levels: 2800 3000 3190 (Stockfish's maximum). GAMES (default 150) and CONCURRENCY (default 20)
# can be set in the environment. The implied rating of the engine at each level is
# level + the Elo fastchess reports; trust only levels where the score lies between 30% and 70%.
set -euo pipefail
SF=${1:?stockfish binary}; ENGINE=${2:?engine binary}; OUT=${3:?output dir}; shift 3
LEVELS=${*:-2800 3000 3190}
REPO=$(cd "$(dirname "$0")/.." && pwd)
GAMES=${GAMES:-150}; CONCURRENCY=${CONCURRENCY:-20}
mkdir -p "$OUT"
for L in $LEVELS; do
    "$REPO/tools/fastchess-src/fastchess" \
        -engine cmd="$ENGINE" args="--uci" name=chess option.Threads=1 option.Hash=64 option.OwnBook=false \
        -engine cmd="$SF" name="sf$L" option.Threads=1 option.Hash=64 option.UCI_LimitStrength=true option.UCI_Elo="$L" \
        -each tc=60+0.6 \
        -openings file="$REPO/tools/openings/8moves_v3.pgn" format=pgn order=random \
        -rounds $((GAMES / 2)) -games 2 -repeat -concurrency "$CONCURRENCY" -recover \
        -pgnout file="$OUT/anchor-$L.pgn" -log level=warn \
        2>"$OUT/anchor-$L.err" | grep -E '^(Elo|Games|Results of)' | tee "$OUT/anchor-$L.txt" | sed "s/^/[$L] /"
done
