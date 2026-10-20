# Run by CTest: cmake -DCHESS=<binary> -DEXPECTED=<tools/bench_signature.txt> -P tools/check_bench.cmake
# Fails unless `chess --bench 6` prints exactly the signature stored in the repo. A change that is meant to
# alter the search (a new pruning rule, say) updates tools/bench_signature.txt in the same commit; a speedup
# or refactor must leave it alone (CLAUDE.md rule 3).
execute_process(COMMAND ${CHESS} --bench 6 OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "chess --bench 6 exited with ${rc}")
endif()
string(REGEX MATCH "BENCH SIGNATURE:[^\n]*" actual "${out}")
file(READ "${EXPECTED}" expected)
string(STRIP "${expected}" expected)
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "bench signature changed\n  expected: ${expected}\n  actual:   ${actual}")
endif()
message(STATUS "${actual}")
