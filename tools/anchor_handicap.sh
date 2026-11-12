#!/usr/bin/env bash
# Anchor against FULL-strength Stockfish with a time handicap: the engine plays 20+0.2 (BASE seconds, BASE/100
# increment), Stockfish gets 1/R of that. UCI_Elo anchoring (tools/anchor.sh) saturates near 3190; this keeps
# working up to parity. Both on 1 thread, Hash=64, book off, 8moves_v3. The Elo fastchess prints is the engine's
# edge at that handicap; the R where it reaches 0 is the measure (compare builds at the same R).
#
#   tools/anchor_handicap.sh <stockfish-binary> <engine-binary> <out-dir> [R ...]     (default R: 8 16 32)
#
# GAMES (default 300), CONCURRENCY (default 20) and BASE (default 20) can be set in the environment.
set -euo pipefail
SF=${1:?stockfish binary}; ENGINE=${2:?engine binary}; OUT=${3:?output dir}; shift 3
RATIOS=${*:-8 16 32}
REPO=$(cd "$(dirname "$0")/.." && pwd)
GAMES=${GAMES:-300}; CONCURRENCY=${CONCURRENCY:-20}; BASE=${BASE:-20}
mkdir -p "$OUT"
for R in $RATIOS; do
    SF_TC=$(awk -v b="$BASE" -v r="$R" 'BEGIN { printf "%.4f+%.5f", b / r, b / 100 / r }')
    "$REPO/tools/fastchess-src/fastchess" \
        -engine cmd="$ENGINE" args="--uci" name=chess option.Threads=1 option.Hash=64 option.OwnBook=false tc="$BASE+$(awk -v b="$BASE" 'BEGIN { printf "%.3f", b / 100 }')" \
        -engine cmd="$SF" name="sf1_$R" option.Threads=1 option.Hash=64 tc="$SF_TC" \
        -openings file="$REPO/tools/openings/8moves_v3.pgn" format=pgn order=random \
        -rounds $((GAMES / 2)) -games 2 -repeat -concurrency "$CONCURRENCY" -recover \
        -pgnout file="$OUT/handicap-$R.pgn" -config outname="$OUT/config.json" -log level=warn \
        2>"$OUT/handicap-$R.err" | grep -E '^(Elo|Games|Results of)' | tee "$OUT/handicap-$R.txt" | sed "s/^/[1\/$R] /"
done
