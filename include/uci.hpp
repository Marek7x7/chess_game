#pragma once

// Runs the engine as a UCI (Universal Chess Interface) engine: reads
// commands from stdin, writes responses to stdout, until "quit" or EOF.
int runUci();
