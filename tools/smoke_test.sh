#!/usr/bin/env bash
# Phase-0 harness-plumbing check -- NOT a strength test. Runs the current
# Release binary against itself for a fixed number of games (no -sprt: a
# binary can't show an Elo difference from itself) and checks fastchess's
# own summary for crashes / time losses / illegal moves. Uses
# tools/openings/mini_suite.pgn deliberately -- this run makes no strength
# claim, so the smoke-test-only opening suite is the right one here.
#
# Requires tools/setup_fastchess.sh to have been run first, and a Release
# build at build-release/chess (cmake -S . -B build-release
# -DCMAKE_BUILD_TYPE=Release && cmake --build build-release).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FASTCHESS="$REPO/tools/fastchess-src/fastchess"
BINARY="$REPO/build-release/chess"
WORK="$REPO/.sprt-work"
mkdir -p "$WORK"

if [ ! -x "$FASTCHESS" ]; then
    echo "fastchess not built -- run tools/setup_fastchess.sh first" >&2
    exit 1
fi
if [ ! -x "$BINARY" ]; then
    echo "$BINARY not found -- build it first (cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release)" >&2
    exit 1
fi

CONCURRENCY=$(( $(nproc) / 2 - 1 ))
[ "$CONCURRENCY" -lt 1 ] && CONCURRENCY=1

echo "Running 200-game self-play smoke test (tc=8+0.08, concurrency=$CONCURRENCY)..."
"$FASTCHESS" \
    -engine cmd="$BINARY" args="--uci" name=new1 option.Threads=1 option.Hash=64 option.OwnBook=false \
    -engine cmd="$BINARY" args="--uci" name=new2 option.Threads=1 option.Hash=64 option.OwnBook=false \
    -each tc=8+0.08 \
    -openings file="$REPO/tools/openings/mini_suite.pgn" format=pgn order=random \
    -rounds 100 -repeat -concurrency "$CONCURRENCY" -recover \
    -config outname="$WORK/config.json" \
    -pgnout file="$WORK/smoke.pgn"

echo "Smoke test finished. Pass bar: 200 games, zero crashes/time-losses/illegal-moves"
echo "(check the summary above and $WORK/smoke.pgn's [Termination] tags)."
