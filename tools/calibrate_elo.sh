#!/usr/bin/env bash
# Calibrates the UCI_Elo -> node-cap table in src/uci.cpp: plays the engine with a fixed node cap
# against Stockfish at UCI_Elo <level> and prints the score and the Elo difference. A cap that plays at
# the level scores 50%; the cap for the next try follows from the difference (about 70-110 Elo per
# doubling of nodes), so a few runs per level converge.
#
#   tools/calibrate_elo.sh <stockfish-binary> <engine-binary> <out-dir> <level> <nodes> [games]
#
# 20+0.2, both on one thread, Hash=16, book off, 8moves_v3; CONCURRENCY (default 20) in the environment.
set -euo pipefail
SF=${1:?stockfish binary}; ENGINE=${2:?engine binary}; OUT=${3:?output dir}; LEVEL=${4:?level}; NODES=${5:?nodes}
GAMES=${6:-100}; CONCURRENCY=${CONCURRENCY:-20}
REPO=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$OUT"
"$REPO/tools/fastchess-src/fastchess" \
    -engine cmd="$ENGINE" args="--uci" name=chess nodes="$NODES" option.Threads=1 option.Hash=16 option.OwnBook=false \
    -engine cmd="$SF" name="sf$LEVEL" option.Threads=1 option.Hash=16 option.UCI_LimitStrength=true option.UCI_Elo="$LEVEL" \
    -each tc=20+0.2 \
    -openings file="$REPO/tools/openings/8moves_v3.pgn" format=pgn order=random \
    -rounds $((GAMES / 2)) -games 2 -repeat -concurrency "$CONCURRENCY" -recover \
    -pgnout file="$OUT/calib-$LEVEL-$NODES.pgn" -log level=warn \
    2>"$OUT/calib-$LEVEL-$NODES.err" | grep -E '^(Elo|Games)' | tail -2
