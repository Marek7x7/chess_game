# Turns a binary file into a C++ source that defines it as a byte array (the engine's built-in network):
#   cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DSYMBOL=<name> -P tools/embed_file.cmake
# defines `const unsigned char <SYMBOL>[]` and `const unsigned long <SYMBOL>_size`, both extern "C".
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hexLength)
math(EXPR size "${hexLength} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUTPUT}" "// Generated from ${INPUT} by tools/embed_file.cmake; do not edit.
extern \"C\" {
extern const unsigned char ${SYMBOL}[];
extern const unsigned long ${SYMBOL}_size;
const unsigned char ${SYMBOL}[] = {${bytes}};
const unsigned long ${SYMBOL}_size = ${size};
}
")
