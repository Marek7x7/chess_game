CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Iinclude -O2 -pthread $(shell pkg-config --cflags sdl2)
LDLIBS := $(shell pkg-config --libs sdl2)
SRC := $(wildcard src/*.cpp)
BIN := bin/chess

.PHONY: all clean run

all: $(BIN)

$(BIN): $(SRC)
	mkdir -p bin
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN) $(LDLIBS)

run: all
	./$(BIN)

clean:
	rm -rf bin
