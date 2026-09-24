#!/usr/bin/env bash
# tools/sprt.sh <new-ref> <base-ref> [tc] [elo0] [elo1]
#   <new-ref>/<base-ref>: any git ref; "WORKTREE" for <new-ref> tests the
#     current uncommitted working tree instead of a committed ref.
#   [tc] default "10+0.1" (search changes). Pass "8+0.08" for quick eval
#     screening.
#   [elo0]/[elo1] default "0 5" (standard "is this better" bounds). Pass
#     "-3 1" for pure simplifications/speedups expected to be ~neutral.
#
# Requires tools/setup_fastchess.sh and tools/fetch_openings.sh to have
# been run first.
set -euo pipefail

NEW_REF="${1:?usage: sprt.sh <new-ref> <base-ref> [tc] [elo0] [elo1]}"
BASE_REF="${2:?usage: sprt.sh <new-ref> <base-ref> [tc] [elo0] [elo1]}"
TC="${3:-10+0.1}"
ELO0="${4:-0}"
ELO1="${5:-5}"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FASTCHESS="$REPO/tools/fastchess-src/fastchess"
WORK="$REPO/.sprt-work"
mkdir -p "$WORK"

if [ ! -x "$FASTCHESS" ]; then
    echo "fastchess not built -- run tools/setup_fastchess.sh first" >&2
    exit 1
fi

build_ref() { # $1=ref $2=name(new|base)
    local ref="$1" name="$2"
    if [ "$ref" = "WORKTREE" ]; then
        cmake -S "$REPO" -B "$REPO/build-sprt-$name" -DCMAKE_BUILD_TYPE=Release >/dev/null
        cmake --build "$REPO/build-sprt-$name" -j >/dev/null
        cp "$REPO/build-sprt-$name/chess" "$WORK/$name.bin"
    else
        rm -rf "$WORK/wt-$name"
        git -C "$REPO" worktree add --detach "$WORK/wt-$name" "$ref" >/dev/null
        cmake -S "$WORK/wt-$name" -B "$WORK/wt-$name/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
        cmake --build "$WORK/wt-$name/build" -j >/dev/null
        cp "$WORK/wt-$name/build/chess" "$WORK/$name.bin"
        git -C "$REPO" worktree remove --force "$WORK/wt-$name"
    fi
}

echo "Building new ($NEW_REF)..."
build_ref "$NEW_REF" new
echo "Building base ($BASE_REF)..."
build_ref "$BASE_REF" base

OPENINGS="$REPO/tools/openings/8moves_v3.pgn"
if [ ! -f "$OPENINGS" ]; then
    OPENINGS="$REPO/tools/openings/mini_suite.pgn"
    echo "WARNING: using smoke-test opening suite (tools/openings/8moves_v3.pgn not found -- run tools/fetch_openings.sh) -- this result is NOT valid for a KEPT/REVERTED verdict" >&2
fi

# Each concurrent game pair spawns two Threads=1 engine processes, so halve
# nproc (minus 1 headroom) rather than using it directly.
CONCURRENCY=$(( $(nproc) / 2 - 1 ))
[ "$CONCURRENCY" -lt 1 ] && CONCURRENCY=1

echo "Running SPRT: tc=$TC elo0=$ELO0 elo1=$ELO1 openings=$OPENINGS concurrency=$CONCURRENCY"
"$FASTCHESS" \
    -engine cmd="$WORK/new.bin"  args="--uci" name=new  option.Threads=1 option.Hash=64 option.OwnBook=false \
    -engine cmd="$WORK/base.bin" args="--uci" name=base option.Threads=1 option.Hash=64 option.OwnBook=false \
    -each tc="$TC" \
    -openings file="$OPENINGS" format=pgn order=random \
    -sprt elo0="$ELO0" elo1="$ELO1" alpha=0.05 beta=0.05 \
    -rounds 20000 -repeat -concurrency "$CONCURRENCY" -recover \
    -log file="$WORK/sprt.log" level=info \
    -pgnout file="$WORK/games.pgn"
