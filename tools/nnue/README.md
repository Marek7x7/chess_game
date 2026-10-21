# Neural-network evaluation: data, training, use

The engine can evaluate with a small neural network instead of the hand-crafted terms (`include/nnue_arch.hpp`
describes the shape: 768 one-hot inputs per side, 128 hidden units, SCReLU, eight output weight sets chosen by the
piece count). Nothing here needs Python or a GPU: the trainer is C++ and trains about 2 million positions per second
per three cores.

```sh
cmake -S . -B build-tools -DCMAKE_BUILD_TYPE=Release
cmake --build build-tools --target chess nnue_data nnue_train -j
```

## 1. Training data

Records are 32 bytes (`include/nnue_record.hpp`): the position, the search score and depth of the move chosen
there, and the game's result. Two sources:

```sh
# From PGN files that carry the engine's score in each move comment, as fastchess writes them
# ("{+0.95/12 0.318s}" = score from the mover's point of view, depth, time; "{book}" for book moves):
build-tools/nnue_data data.bin .sprt-work/games*.pgn --min-depth 8

# From self-play at a fixed number of nodes, started from random openings (no match may be running: it uses
# every core it is given). Both sides use the network if one is given: the next generation of data.
tools/nnue/datagen.sh build-tools/chess data-dir 22 500 5000 [network.nnue]
cat data-dir/data-*.bin > data.bin
```

Positions in check, moves that capture or promote, mate scores and huge scores are left out.

## 2. Training

```sh
build-tools/nnue_train train data.bin --out net.nnue --epochs 20 --lambda 0.7
```

The target is `lambda * sigmoid(score / K) + (1 - lambda) * result` with `K = 173.7`, the scale of the hand-crafted
evaluation's Texel fit, so the network's centipawns mean about the same. 5% of the data (blocks of 4096 consecutive
records, which keeps games together) is held out and its loss reported after each epoch. At the end the exported
integer network is compared with the float one it came from.

## 3. Is it better than the hand-crafted evaluation?

```sh
build-tools/nnue_train eval data.bin --net net.nnue --valid-only
build-tools/nnue_train eval data.bin --hce --valid-only
```

Both print the loss against the search score and against the game result on the held-out blocks. Then play it:

```sh
NEW_OPTIONS="option.EvalFile=$PWD/net.nnue" BASE_OPTIONS="option.EvalFile=none" \
    tools/sprt.sh HEAD HEAD 5+0.05 0 10
```

## 4. Using a network

`setoption name EvalFile value <path>` in UCI (`none` = hand-crafted evaluation, `<default>` = the built-in
network). To ship one in the executable, copy it to `nnue/default.nnue` and rebuild (`-DCHESS_NNUE=OFF` leaves it
out); `--verify-eval` then checks the incrementally updated accumulators against a recompute, and
`tools/bench_signature.txt` has to be updated, since the search is no longer the same.

## 5. Unattended: `auto.sh`

```sh
tools/nnue/auto.sh --work ~/nnue-run --hours 12 [--start-net best.nnue] [--seed-data old.bin] [--max-rounds N]
```

Loops over: self-play with the best network so far (the hand-crafted evaluation in round 1, unless `--start-net`),
training a challenger on the data pool plus the new data, and an SPRT of the challenger against the best network
(same binary, only `EvalFile` differs; 5+0.05, bounds -3 1, 20 games at once). If H1 is accepted the challenger
becomes the best network (`<work>/best/round-N.nnue`); if not, one retry with twice the epochs and another lambda,
then the run ends. The steps never overlap. Options: `--procs 22 --games 8000 --nodes 5000 --epochs 30 --lambda 0.7`,
`--keep-rounds 6` (rounds of data kept), `--min-free-gb 20`, `--sprt-bounds "-3 1"`, `--sprt-rounds`, `--sprt-tc`.

- Everything is under `--work`, which must be on a real disk (it refuses a RAM disk). It checks the free space before
  each step, and the engine is built from the checked-out sources, so commit them first (matches build `HEAD`).
- `<work>/state` is written after every step: after a crash or `Ctrl-C`, run the same command again to resume
  (a half-finished data run is redone from its start). `touch <work>/STOP` stops after the current step. The
  `--hours` budget is checked between steps, so the last step can run past it (a data run takes about an hour
  at the defaults).
- A status line is printed every 20 minutes; `<work>/log.txt` has the exact SPRT numbers (Elo, games, LLR) of every
  test, which are what the `STRENGTH_LOG.md` row is written from by hand. At the end it prints how to ship the best
  network (it never touches `nnue/default.nnue`, merges or pushes).
