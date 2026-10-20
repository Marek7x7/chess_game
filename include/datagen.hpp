#pragma once

// `chess --datagen --out <file> [--games N] [--nodes N] [--random-plies N] [--seed N] [--net <file>]`:
// self-play games at a fixed node count, appending training records (nnue_record.hpp) to <file>.
int runDatagen(int argc, char** argv);
