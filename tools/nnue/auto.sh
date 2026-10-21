#!/usr/bin/env bash
# Trains the evaluation network in a loop, unattended. One round:
#
#   1. datagen    self-play with the current best network (the hand-crafted evaluation in round 1 unless
#                 --start-net is given), tools/nnue/datagen.sh, a new SEED_BASE each round
#   2. train      a challenger on the data pool plus the new data (nnue_train train)
#   3. sprt       challenger against the best network, the same binary with only EvalFile different
#                 (tools/sprt.sh, 5+0.05, bounds -3 1): H1 accepted makes the challenger the best network
#   4. retry      if it was not accepted, train once more with other settings (twice the epochs, another
#                 lambda) and test again; a second failure ends the run
#
# The steps never overlap. Everything is written under --work (a real disk: never /tmp, which is a small RAM
# disk here), and a state file after every step lets a crashed or killed run continue where it stopped: start
# it again with the same --work.
#
#   tools/nnue/auto.sh --work <dir> --hours <N> [--procs 22] [--games 8000] [--nodes 5000]
#                      [--epochs 30] [--lambda 0.7] [--start-net <file>] [--max-rounds N]
#   more: [--seed-data <file>]... (training data from before, e.g. games turned into records by nnue_data)
#         [--keep-rounds 6] [--min-free-gb 20] [--sprt-bounds "-3 1"] [--sprt-rounds 20000]
#         [--sprt-concurrency 20] [--sprt-tc 5+0.05] [--allow-dirty]
#
# <work>/best/round-N.nnue   every network that passed          <work>/log.txt  the numbers, one entry per test
# <work>/run.log             everything this script printed     <work>/state    where the run is
# <work>/round-N/            that round's data, logs, challengers.   touch <work>/STOP  to stop after this step.
#
# It does not touch nnue/default.nnue, merge, or push; STRENGTH_LOG.md rows are written by hand afterwards.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

die() { echo "auto.sh: $*" >&2; exit 1; }

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'; }

# ---------------------------------------------------------------- options
WORK="" HOURS="" PROCS=22 GAMES=8000 NODES=5000 EPOCHS=30 LAMBDA=0.7 START_NET="" MAX_ROUNDS=0
KEEP_ROUNDS=6 MIN_FREE_GB=20 BOUNDS="-3 1" SPRT_ROUNDS=20000 SPRT_CONC=20 SPRT_TC="5+0.05" ALLOW_DIRTY=0
SEED_DATA=()
while [ $# -gt 0 ]; do
    case "$1" in
        --work) WORK="${2:?--work needs a directory}"; shift 2 ;;
        --hours) HOURS="${2:?--hours needs a number}"; shift 2 ;;
        --procs) PROCS="${2:?}"; shift 2 ;;
        --games) GAMES="${2:?}"; shift 2 ;;
        --nodes) NODES="${2:?}"; shift 2 ;;
        --epochs) EPOCHS="${2:?}"; shift 2 ;;
        --lambda) LAMBDA="${2:?}"; shift 2 ;;
        --start-net) START_NET="${2:?}"; shift 2 ;;
        --max-rounds) MAX_ROUNDS="${2:?}"; shift 2 ;;
        --seed-data) SEED_DATA+=("${2:?}"); shift 2 ;;
        --keep-rounds) KEEP_ROUNDS="${2:?}"; shift 2 ;;
        --min-free-gb) MIN_FREE_GB="${2:?}"; shift 2 ;;
        --sprt-bounds) BOUNDS="${2:?}"; shift 2 ;;
        --sprt-rounds) SPRT_ROUNDS="${2:?}"; shift 2 ;;
        --sprt-concurrency) SPRT_CONC="${2:?}"; shift 2 ;;
        --sprt-tc) SPRT_TC="${2:?}"; shift 2 ;;
        --allow-dirty) ALLOW_DIRTY=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown option $1" ;;
    esac
done
[ -n "$WORK" ] && [ -n "$HOURS" ] || { usage >&2; die "--work and --hours are required"; }
num() { [[ "$2" =~ ^[0-9]+$ ]] || die "$1 must be a whole number, got '$2'"; }
for v in PROCS GAMES NODES EPOCHS MAX_ROUNDS KEEP_ROUNDS MIN_FREE_GB SPRT_ROUNDS SPRT_CONC; do num "$v" "${!v}"; done
[[ "$HOURS" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "--hours must be a number, got '$HOURS'"
[[ "$LAMBDA" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "--lambda must be a number, got '$LAMBDA'"
[[ "$BOUNDS" =~ ^-?[0-9.]+\ +-?[0-9.]+$ ]] || die "--sprt-bounds must be two numbers, got '$BOUNDS'"
[ "$KEEP_ROUNDS" -ge 1 ] || die "--keep-rounds must be at least 1"
[ "$PROCS" -lt 1000 ] || die "--procs must be below 1000 (seeds are spaced 1000 apart per round)"
BUDGET_S="$(awk -v h="$HOURS" 'BEGIN { printf "%d", h * 3600 }')"

mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
case "$WORK" in *" "*) die "--work must not contain spaces (the match options are passed as words)" ;; esac
mkdir -p "$WORK/best" "$WORK/tmp"
[ -z "$START_NET" ] || [ -f "$START_NET" ] || die "--start-net: no such file $START_NET"
for f in ${SEED_DATA[@]+"${SEED_DATA[@]}"}; do [ -f "$f" ] || die "--seed-data: no such file $f"; done
case "$(stat -f -c %T "$WORK")" in
    tmpfs|ramfs) die "$WORK is on a RAM disk; use a directory on a real disk" ;;
esac
export TMPDIR="$WORK/tmp" # nothing of ours may land in /tmp

# One instance per work directory.
exec 9>"$WORK/lock"
flock -n 9 || die "another auto.sh is already running in $WORK"

LOG="$WORK/run.log"
RESULTS="$WORK/log.txt"
STATE="$WORK/state"
log() { echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }

# ---------------------------------------------------------------- state
# ROUND = the round being worked on, STEP = datagen|train|sprt, ATTEMPT = 0 or 1 (the retry), BEST = the best
# network's path ("" = hand-crafted evaluation), DONE = rounds finished, USED_S = seconds of the budget used.
ROUND=1 STEP=datagen ATTEMPT=0 BEST="" DONE=0 USED_S=0 LAST="(nothing tested yet)"
if [ -f "$STATE" ]; then
    # shellcheck disable=SC1090
    . "$STATE"
    log "resuming round $ROUND at step $STEP (attempt $ATTEMPT), $((USED_S / 60)) min of the budget already used"
elif [ -n "$START_NET" ]; then
    cp "$START_NET" "$WORK/best/round-0.nnue"
    BEST="$WORK/best/round-0.nnue"
fi
USED_BASE=$USED_S
T0=$(date +%s)
used() { echo $((USED_BASE + $(date +%s) - T0)); }
save_state() {
    USED_S=$(used)
    {
        printf 'ROUND=%q\nSTEP=%q\nATTEMPT=%q\nBEST=%q\nDONE=%q\nUSED_S=%q\nLAST=%q\n' \
            "$ROUND" "$STEP" "$ATTEMPT" "$BEST" "$DONE" "$USED_S" "$LAST"
    } > "$STATE.tmp"
    mv "$STATE.tmp" "$STATE"
}
hms() { printf '%dh%02dm' $(($1 / 3600)) $(($1 % 3600 / 60)); }

# ---------------------------------------------------------------- children, status line, shutdown
CHILD=""
TICKER=""
kill_tree() {
    local p="$1" c
    for c in $(ps -o pid= --ppid "$p" 2>/dev/null); do kill_tree "$c"; done
    kill "$p" 2>/dev/null || true
}
on_signal() {
    log "signal received: stopping (state is saved at the last finished step; start again with the same --work)"
    [ -n "$CHILD" ] && kill_tree "$CHILD"
    exit 130
}
on_exit() { [ -n "$TICKER" ] && kill_tree "$TICKER"; return 0; }
trap on_signal INT TERM
trap on_exit EXIT

# Runs a command in the background and waits for it by pid (never by pattern), so that a signal can end it.
run() {
    "$@" &
    CHILD=$!
    local rc=0
    wait "$CHILD" || rc=$?
    CHILD=""
    return "$rc"
}

status_line() {
    local prog=""
    if [ "$STEP" = sprt ] && [ -f "$WORK/round-$ROUND/sprt-a$ATTEMPT.txt" ]; then
        prog=" | now: $(grep -E '^(Games|LLR)' "$WORK/round-$ROUND/sprt-a$ATTEMPT.txt" | tail -2 | tr '\n' ' ')"
    fi
    echo "[status $(date '+%H:%M')] round $ROUND, step $STEP, attempt $ATTEMPT, elapsed $(hms "$(used)") of $(hms "$BUDGET_S"), rounds done $DONE | last: $LAST$prog" | tee -a "$LOG"
}
( while sleep 1200; do
      # (the state file is the source of truth: this subshell has only a copy of the variables)
      # shellcheck disable=SC1090
      . "$STATE" 2>/dev/null || true
      USED_BASE=$USED_S T0=$(date +%s) status_line
  done ) &
TICKER=$!

# ---------------------------------------------------------------- checks
free_mb() { df -Pm "$WORK" | awk 'NR == 2 { print $4 }'; }
disk_ok() {
    local mb
    mb=$(free_mb)
    if [ "$mb" -lt $((MIN_FREE_GB * 1024)) ]; then
        log "STOP: only $((mb / 1024)) GB free under $WORK (--min-free-gb $MIN_FREE_GB)"
        return 1
    fi
}
# Another heavy job would distort the timing of the matches and compete for the cores.
others_running() {
    ps -eo comm= | grep -xE 'chess|chess-dg|fastchess|nnue_train|nnue_data' || true
}

ship_hint() {
    if [ -z "$BEST" ]; then
        log "No network beat the hand-crafted evaluation, so there is nothing to ship."
        return
    fi
    cat <<EOF | tee -a "$LOG"

Best network: $BEST   ($DONE round(s) passed)
To ship it (nothing of this was done by the script):
  cp "$BEST" nnue/default.nnue
  cmake --build build-release && build-release/chess --bench 6        # a new BENCH SIGNATURE
  edit tools/bench_signature.txt to that number
  ctest --test-dir build-release --output-on-failure                  # perft, verify-eval (checks the accumulators), signature, UCI
  git add nnue/default.nnue tools/bench_signature.txt && git commit   # then a separate commit with the STRENGTH_LOG row
Numbers for the row: $RESULTS
EOF
}

finish() { # $1 = reason
    log "Run ends: $1"
    ship_hint
    exit 0
}

# ---------------------------------------------------------------- build
cd "$REPO"
[ -x tools/fastchess-src/fastchess ] || die "fastchess is not built: run tools/setup_fastchess.sh first"
[ -f tools/openings/8moves_v3.pgn ] || die "tools/openings/8moves_v3.pgn is missing: run tools/fetch_openings.sh first"
git rev-parse --verify HEAD >/dev/null 2>&1 || die "$REPO is not a git repository with a commit"
if [ "$ALLOW_DIRTY" -eq 0 ] && ! git diff --quiet HEAD -- src include CMakeLists.txt cmake tools/nnue tools/sprt.sh; then
    die "uncommitted changes in the engine sources: the matches build HEAD, the data generation the working tree. Commit them, or pass --allow-dirty"
fi
HEAD_REF="$(git rev-parse HEAD)"
others="$(others_running)"
[ -z "$others" ] || die "another heavy job is running ($(echo "$others" | sort -u | tr '\n' ' ')); wait for it, or stop it"
disk_ok || exit 1

BUILD="$WORK/build"
log "building chess and nnue_train in $BUILD (engine commit ${HEAD_REF:0:9})"
{
    cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCHESS_GUI=OFF -DCHESS_NNUE=OFF -DCHESS_BUILD_TESTS=OFF &&
        cmake --build "$BUILD" --target chess nnue_train -j
} > "$WORK/build.log" 2>&1 || { tail -20 "$WORK/build.log" >&2; die "the build failed (full output: $WORK/build.log)"; }
CHESS="$BUILD/chess"
TRAIN="$BUILD/nnue_train"
[ -x "$CHESS" ] && [ -x "$TRAIN" ] || die "the build did not produce $CHESS and $TRAIN (see $WORK/build.log)"
log "run: work=$WORK budget=$(hms "$BUDGET_S") procs=$PROCS games/proc=$GAMES nodes=$NODES epochs=$EPOCHS lambda=$LAMBDA keep=$KEEP_ROUNDS sprt=[$BOUNDS] tc=$SPRT_TC"
save_state

# ---------------------------------------------------------------- steps
training_inputs() { # the seed data and the newest KEEP_ROUNDS rounds up to and including this one
    local r f
    for f in ${SEED_DATA[@]+"${SEED_DATA[@]}"}; do echo "$f"; done
    for ((r = ROUND > KEEP_ROUNDS ? ROUND - KEEP_ROUNDS + 1 : 1; r <= ROUND; r++)); do
        if [ -f "$WORK/round-$r/data.bin" ]; then echo "$WORK/round-$r/data.bin"; fi
    done
}

step_datagen() {
    local dir="$WORK/round-$ROUND"
    rm -rf "$dir" # a half-written round cannot be continued: the same seeds would repeat their games
    mkdir -p "$dir"
    log "round $ROUND: self-play, $PROCS processes x $GAMES games at $NODES nodes with ${BEST:-the hand-crafted evaluation}"
    SEED_BASE=$((100000 + ROUND * 1000)) run tools/nnue/datagen.sh "$CHESS" "$dir" "$PROCS" "$GAMES" "$NODES" ${BEST:+"$BEST"} \
        > "$dir/datagen.log" 2>&1 || { tail -5 "$dir/datagen.log" >&2; die "data generation failed (see $dir/datagen.log)"; }
    tail -1 "$dir/datagen.log" | tee -a "$LOG"
    cat "$dir"/data-*.bin > "$dir/data.bin"
    rm -f "$dir"/data-*.bin
    [ -s "$dir/data.bin" ] || die "data generation produced no positions (see $dir/datagen.log)"
    local n=$(($(stat -c %s "$dir/data.bin") / 32))
    log "round $ROUND: $n positions"
    # keep the disk bounded: only the newest rounds' data is training data
    local r
    for ((r = 1; r <= ROUND - KEEP_ROUNDS; r++)); do rm -f "$WORK/round-$r/data.bin"; done
}

settings_for() { # prints "epochs lambda" for the attempt
    if [ "$ATTEMPT" -eq 0 ]; then echo "$EPOCHS $LAMBDA"; return; fi
    awk -v e="$EPOCHS" -v l="$LAMBDA" 'BEGIN { printf "%d %s\n", e * 2, (l > 0.6 ? "0.5" : "0.8") }'
}

step_train() {
    local dir="$WORK/round-$ROUND" epochs lambda inputs
    read -r epochs lambda <<< "$(settings_for)"
    mapfile -t inputs < <(training_inputs)
    [ "${#inputs[@]}" -gt 0 ] || die "no training data"
    log "round $ROUND attempt $ATTEMPT: training on ${#inputs[@]} file(s), $epochs epochs, lambda $lambda"
    run "$TRAIN" train "${inputs[@]}" --out "$dir/challenger-a$ATTEMPT.nnue" --epochs "$epochs" --lambda "$lambda" \
        > "$dir/train-a$ATTEMPT.log" 2>&1 || { tail -5 "$dir/train-a$ATTEMPT.log" >&2; die "training failed (see $dir/train-a$ATTEMPT.log)"; }
    [ -s "$dir/challenger-a$ATTEMPT.nnue" ] || die "training wrote no network (see $dir/train-a$ATTEMPT.log)"
    grep -E 'validation loss' "$dir/train-a$ATTEMPT.log" | tail -1 | tee -a "$LOG"
}

# Sets VERDICT to accepted | rejected | inconclusive
step_sprt() {
    local dir="$WORK/round-$ROUND" chal="$WORK/round-$ROUND/challenger-a$ATTEMPT.nnue" out="$WORK/round-$ROUND/sprt-a$ATTEMPT.txt"
    local epochs lambda base_opt
    read -r epochs lambda <<< "$(settings_for)"
    base_opt="option.EvalFile=${BEST:-none}"
    log "round $ROUND attempt $ATTEMPT: SPRT of the challenger against ${BEST:-the hand-crafted evaluation}, tc $SPRT_TC, bounds [$BOUNDS]"
    # (fastchess prints hundreds of MB; only the summary lines are kept)
    run bash -c '
        cd "$1"
        shift
        CONCURRENCY="$1" ROUNDS="$2" NEW_OPTIONS="option.EvalFile=$3" BASE_OPTIONS="$4" \
            tools/sprt.sh "$5" "$5" "$6" $7 2>&1 | grep --line-buffered -E "^(Games|LLR|Elo|nElo|SPRT|Building|Running|WARNING)|rror"
    ' _ "$REPO" "$SPRT_CONC" "$SPRT_ROUNDS" "$chal" "$base_opt" "$HEAD_REF" "$SPRT_TC" "$BOUNDS" > "$out" 2>&1 || true
    local games llr elo verdict
    games="$(grep -E '^Games:' "$out" | tail -1 || true)"
    llr="$(grep -E '^LLR:' "$out" | tail -1 || true)"
    elo="$(grep -E '^Elo:' "$out" | tail -1 || true)"
    if [ -z "$games" ]; then
        VERDICT=error
        verdict="no result (the match did not run: see $out)"
    elif grep -q "H1 was accepted" "$out"; then
        VERDICT=accepted; verdict="H1 was accepted"
    elif grep -q "H0 was accepted" "$out"; then
        VERDICT=rejected; verdict="H0 was accepted"
    else
        VERDICT=inconclusive; verdict="stopped at the --sprt-rounds cap without a verdict"
    fi
    {
        echo "[$(date '+%F %T')] round $ROUND attempt $ATTEMPT: ${chal} ($epochs epochs, lambda $lambda, engine ${HEAD_REF:0:9})"
        echo "    vs ${BEST:-hand-crafted evaluation}, tc $SPRT_TC, bounds [$BOUNDS] (normalized Elo), Hash=64, Threads=1, 8moves_v3"
        echo "    $games"
        echo "    $elo"
        echo "    $llr"
        echo "    $verdict"
    } >> "$RESULTS"
    LAST="round $ROUND a$ATTEMPT: ${elo:-no Elo line}; ${games:-no games}; $verdict"
    log "$LAST"
}

# ---------------------------------------------------------------- the loop
status_line
while :; do
    [ ! -e "$WORK/STOP" ] || finish "the STOP file exists"
    [ "$(used)" -lt "$BUDGET_S" ] || finish "the --hours budget is used up"
    [ "$MAX_ROUNDS" -eq 0 ] || [ "$DONE" -lt "$MAX_ROUNDS" ] || finish "--max-rounds $MAX_ROUNDS reached"
    disk_ok || finish "low disk space"
    others="$(others_running)"
    [ -z "$others" ] || finish "another heavy job started ($(echo "$others" | sort -u | tr '\n' ' '))"

    case "$STEP" in
        datagen) step_datagen; STEP=train ;;
        train) step_train; STEP=sprt ;;
        sprt)
            VERDICT=error
            step_sprt
            if [ "$VERDICT" = accepted ]; then
                cp "$WORK/round-$ROUND/challenger-a$ATTEMPT.nnue" "$WORK/best/round-$ROUND.nnue"
                BEST="$WORK/best/round-$ROUND.nnue"
                DONE=$((DONE + 1))
                ROUND=$((ROUND + 1)); STEP=datagen; ATTEMPT=0
                log "the challenger is the new best network: $BEST"
                # a few nets are enough: older ones go (the best one never does)
                ls -1t "$WORK"/best/round-*.nnue | tail -n +6 | while read -r old; do [ "$old" = "$BEST" ] || rm -f "$old"; done
            elif [ "$VERDICT" = error ]; then
                save_state
                finish "the match could not run (see $WORK/round-$ROUND/sprt-a$ATTEMPT.txt)"
            elif [ "$ATTEMPT" -eq 0 ]; then
                ATTEMPT=1; STEP=train
                log "not accepted: retrying with other settings"
            else
                save_state
                finish "round $ROUND: no challenger passed in two attempts (the best network stays ${BEST:-the hand-crafted evaluation})"
            fi
            ;;
        *) die "bad state file: STEP=$STEP" ;;
    esac
    save_state
done
