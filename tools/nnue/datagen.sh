#!/usr/bin/env bash
# Generates network training data with many single-threaded `chess --datagen` processes.
#
#   tools/nnue/datagen.sh <chess-binary> <out-dir> <processes> <games-per-process> [nodes] [network.nnue]
#
# Process i uses seed i and appends to <out-dir>/data-i.bin (so a rerun with the same seeds repeats games: pass
# a different SEED_BASE to get new ones). A game is about 100 searches of `nodes` nodes (default 5000), a few
# hundred milliseconds. With a network file both sides use it: the second generation of data, labelled by the
# first network. Run it with no match running: it uses every core it is given.
set -euo pipefail
BIN="$(realpath "${1:?usage: datagen.sh <chess-binary> <out-dir> <processes> <games-per-process> [nodes] [network]}")"
OUT="${2:?out-dir}"
PROCS="${3:?processes}"
GAMES="${4:?games per process}"
NODES="${5:-5000}"
NET="${6:-}"
SEED_BASE="${SEED_BASE:-0}"
mkdir -p "$OUT"
pids=()
for i in $(seq 1 "$PROCS"); do
    seed=$((SEED_BASE + i))
    args=(--datagen --out "$OUT/data-$seed.bin" --games "$GAMES" --nodes "$NODES" --seed "$seed")
    [ -n "$NET" ] && args+=(--net "$NET")
    nice -n 10 "$BIN" "${args[@]}" > "$OUT/log-$seed.txt" 2>&1 &
    pids+=($!)
done
for p in "${pids[@]}"; do wait "$p"; done
cat "$OUT"/log-*.txt | grep -h '^games' | awk '{g+=$2; p+=$4; w+=$6; d+=$8; l+=$10} END {printf "total: %d games, %d positions (W %d D %d L %d)\n", g, p, w, d, l}' || true
