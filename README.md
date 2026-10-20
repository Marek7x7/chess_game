# chess_game

A chess engine and an SDL2 game around it, written in C++17: mailbox 8x8 board with bitboards, negamax/PVS
with quiescence search, a lock-free shared transposition table, Lazy SMP threads, a hand-tuned evaluation,
a small opening book, a UCI mode and a mouse-driven GUI (human vs computer, bot vs bot).

Strength is measured, not claimed: every kept change has an SPRT row in [STRENGTH_LOG.md](STRENGTH_LOG.md),
and the last absolute estimate (against Stockfish `UCI_Elo`, 2026-09-28) is in the same file.

## Build

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
build-release/chess            # the GUI (needs SDL2 development files: libsdl2-dev / SDL2-devel)
```

| Option | Default | |
|---|---|---|
| `CHESS_NATIVE_ARCH` | ON | `-march=native`: fastest on this machine, may crash with SIGILL on another CPU |
| `CHESS_WERROR` | ON | warnings are errors (`-Wall -Wextra`) |
| `CHESS_LTO` | ON | link-time optimisation in Release builds |
| `CHESS_GUI` | ON | OFF builds a UCI/command-line engine that needs no SDL2 |
| `CHESS_PORTABLE` | OFF | self-contained executable: no `-march=native` (uses `CHESS_PORTABLE_ARCH`, default `x86-64-v2`; `x86-64-v3` is 10-15% faster on CPUs from 2013 on), static C++ runtime, SDL2 built in |
| `CHESS_BUNDLE_SDL2` | OFF | download SDL2 and link it statically (needs network at configure time) |
| `CHESS_BUILD_TESTS` | ON | register the correctness gates with CTest |

The release build needs GCC or Clang. MSVC is not supported (the code uses GCC/Clang builtins).

```sh
# A portable Linux binary, no SDL2 needed on the target machine:
cmake -S . -B build-portable -DCHESS_PORTABLE=ON && cmake --build build-portable -j

# A Windows .exe cross-compiled from Linux (GCC MinGW-w64 or the self-contained llvm-mingw):
cmake -S . -B build-windows -DCHESS_PORTABLE=ON -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw.cmake
cmake --build build-windows -j

# Only the engine, for a server or CI:
cmake -S . -B build-uci -DCHESS_GUI=OFF && cmake --build build-uci -j
```

For about 5% more speed (same search, identical bench signature) `tools/build_pgo.sh [build-dir]` makes a
profile-guided GCC build: instrument, train on the benchmark searches, rebuild.

Extra targets that are not built by default: `chess_spsa` (search constants exposed as UCI options, for
`tools/spsa.py`) and `chess_tune` (the Texel evaluation tuner, needs OpenMP).

## Run

| Command | |
|---|---|
| `chess` | the GUI |
| `chess --uci` | UCI engine on stdin/stdout |
| `chess --bench [depth]`, `--perft-suite [depth]`, `--verify-eval [depth]` | benchmark and correctness checks |
| `chess --replay <pgn> [white\|black\|both]` | re-search the positions of a game |
| `chess --selfplay <games> [levelA] [levelB]` | bot matches (`easy`, `medium`, `hard`, `expert`) |
| `chess --version`, `chess --help` | |

UCI options: `Hash` (MB, 1-4096), `Threads`, `OwnBook` (off by default), `Move Overhead`. Supported `go`
parameters: `depth`, `nodes`, `movetime`, `wtime`/`btime`/`winc`/`binc`/`movestogo`, `infinite`, `searchmoves`.
Not supported: `ponder`, `mate`, MultiPV, `UCI_LimitStrength`, tablebases.

## Tests

```sh
ctest --test-dir build-release --output-on-failure
```

runs perft against published node counts, the incremental-evaluation self-check, the bench signature
(`tools/bench_signature.txt`) and `tools/uci_conformance.py`, a suite of the ways a GUI or tournament manager
can hurt an engine. A change meant to alter the search updates `tools/bench_signature.txt` in the same commit; a
speedup or refactor must leave it alone.

Strength changes go through an SPRT with [fastchess](https://github.com/Disservin/fastchess):

```sh
tools/setup_fastchess.sh && tools/fetch_openings.sh        # once
CONCURRENCY=20 tools/sprt.sh <new-ref|WORKTREE> <base-ref> 5+0.05 0 10
```

## Layout

| | |
|---|---|
| `src/ai.cpp` | evaluation and search (`evaluate`, `negamax`, `quiescence`, `findBestMove`) |
| `src/board.cpp`, `src/bitboard.cpp` | board, move generation, incremental evaluation and hashing |
| `src/transposition_table.cpp` | the lock-free hash table |
| `src/uci.cpp`, `src/gui.cpp`, `src/match.cpp` | UCI driver, SDL2 GUI, bot matches |
| `include/tune.hpp`, `include/search_params.hpp` | tunable evaluation weights and search constants |
| `tools/` | SPRT and SPSA drivers, the Texel tuner, Stockfish analysis scripts, the UCI suite |
