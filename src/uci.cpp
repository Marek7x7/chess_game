#include "uci.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ai.hpp"
#include "board.hpp"
#include "move.hpp"
#include "transposition_table.hpp"

namespace {

// Parses one UCI coordinate-notation move ("e2e4", "e7e8q") against the
// legal moves in `board` -- the same shape as pgn.cpp's resolveSanMove, just
// keyed by coordinates instead of SAN disambiguation. Castling resolves
// automatically this way since the legal-move list already contains the
// correctly-flagged castling Move keyed by the king's from/to squares.
bool parseUciMove(const Board& board, const std::string& token, Move& out) {
    if (token.size() < 4) return false;
    int fromX = token[0] - 'a', fromY = token[1] - '1';
    int toX = token[2] - 'a', toY = token[3] - '1';
    if (fromX < 0 || fromX > 7 || fromY < 0 || fromY > 7 || toX < 0 || toX > 7 || toY < 0 || toY > 7) return false;

    PieceType promo = PieceType::None;
    if (token.size() >= 5) {
        switch (token[4]) {
            case 'q': promo = PieceType::Queen; break;
            case 'r': promo = PieceType::Rook; break;
            case 'b': promo = PieceType::Bishop; break;
            case 'n': promo = PieceType::Knight; break;
            default: return false;
        }
    }

    for (const Move& m : board.legalMoves(board.sideToMove())) {
        if (m.fromX == fromX && m.fromY == fromY && m.toX == toX && m.toY == toY && m.promotion == promo) {
            out = m;
            return true;
        }
    }
    return false;
}

// entries*sizeof(TTEntry) <= mb MiB, rounded down to a power of two
// (TranspositionTable's constructor requires one) and floored so a tiny or
// garbage Hash value can't produce a degenerate table.
size_t hashMBToEntries(int mb) {
    if (mb < 1) mb = 1;
    size_t bytes = static_cast<size_t>(mb) * 1024 * 1024;
    size_t entries = bytes / sizeof(TTEntry);
    size_t p = size_t(1) << 16;
    while (p * 2 <= entries) p *= 2;
    return p;
}

std::vector<std::string> splitTokens(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string t;
    while (iss >> t) tokens.push_back(t);
    return tokens;
}

} // namespace

int runUci() {
    // fastchess/cutechess talk to this process over a pipe, not a terminal:
    // printf to a pipe is fully buffered by default, so without this,
    // uciok/info/bestmove can sit in libc's buffer indefinitely and the
    // tester hangs waiting for output that's technically already "printed."
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    Board board;
    int hashMB = 224; // matches kTTSize's ~224 MiB (ai.hpp)
    auto tt = std::make_unique<TranspositionTable>(hashMBToEntries(hashMB));
    bool ownBook = false; // UCI default is OFF, unlike every other caller of findBestMove
    int hwThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 2);
    int threadsOption = hwThreads;
    int moveOverheadMs = 50;

    std::atomic<bool> stopFlag{false};
    std::future<void> searchTask;
    auto joinSearch = [&]() {
        if (searchTask.valid()) searchTask.get();
    };

    std::string line;
    while (std::getline(std::cin, line)) {
        std::vector<std::string> tok = splitTokens(line);
        if (tok.empty()) continue;
        const std::string& cmd = tok[0];

        if (cmd == "uci") {
            std::printf("id name ChessGame\n");
            std::printf("id author Marek\n");
            std::printf("option name Hash type spin default %d min 1 max 4096\n", hashMB);
            std::printf("option name Threads type spin default %d min 1 max %d\n", threadsOption,
                        std::max(1, hwThreads * 4));
            std::printf("option name OwnBook type check default false\n");
            std::printf("option name Move Overhead type spin default %d min 0 max 5000\n", moveOverheadMs);
            std::printf("uciok\n");
            std::fflush(stdout);
        } else if (cmd == "isready") {
            // Must answer even mid-search -- the one command that does NOT
            // join the search thread first.
            std::printf("readyok\n");
            std::fflush(stdout);
        } else if (cmd == "setoption") {
            joinSearch();
            stopFlag.store(false);
            // "setoption name <NAME...> value <VALUE...>" -- name (and
            // sometimes value) can contain spaces, so collect tokens
            // between "name" and "value" rather than assuming single words.
            std::string name, value;
            size_t i = 1;
            if (i < tok.size() && tok[i] == "name") {
                i++;
                while (i < tok.size() && tok[i] != "value") {
                    if (!name.empty()) name += ' ';
                    name += tok[i];
                    i++;
                }
                if (i < tok.size() && tok[i] == "value") {
                    i++;
                    while (i < tok.size()) {
                        if (!value.empty()) value += ' ';
                        value += tok[i];
                        i++;
                    }
                }
            }
            if (name == "Hash") {
                hashMB = std::atoi(value.c_str());
                tt = std::make_unique<TranspositionTable>(hashMBToEntries(hashMB));
            } else if (name == "Threads") {
                threadsOption = std::max(1, std::atoi(value.c_str()));
            } else if (name == "OwnBook") {
                ownBook = (value == "true");
            } else if (name == "Move Overhead") {
                moveOverheadMs = std::max(0, std::atoi(value.c_str()));
            }
            // Unrecognized option names are silently ignored, per UCI convention.
        } else if (cmd == "ucinewgame") {
            joinSearch();
            stopFlag.store(false);
            tt = std::make_unique<TranspositionTable>(hashMBToEntries(hashMB));
        } else if (cmd == "position") {
            joinSearch();
            stopFlag.store(false);
            size_t i = 1;
            if (i < tok.size() && tok[i] == "startpos") {
                board = Board();
                i++;
            } else if (i < tok.size() && tok[i] == "fen") {
                i++;
                std::string fen;
                while (i < tok.size() && tok[i] != "moves") {
                    if (!fen.empty()) fen += ' ';
                    fen += tok[i];
                    i++;
                }
                board = Board::fromFEN(fen);
            }
            if (i < tok.size() && tok[i] == "moves") {
                i++;
                for (; i < tok.size(); i++) {
                    Move m;
                    if (!parseUciMove(board, tok[i], m)) break; // malformed input: stop applying
                    board.makeMove(m);
                }
            }
        } else if (cmd == "go") {
            joinSearch(); // defensive: finish any stray prior search first
            stopFlag.store(false);

            int maxDepth = 64;
            long long timeBudgetMs = -1;
            long long movetime = -1, wtime = -1, btime = -1, winc = 0, binc = 0;
            int movestogo = 0;
            for (size_t i = 1; i < tok.size(); i++) {
                auto next = [&](long long& dst) {
                    if (i + 1 < tok.size()) dst = std::atoll(tok[++i].c_str());
                };
                if (tok[i] == "depth") {
                    if (i + 1 < tok.size()) maxDepth = std::atoi(tok[++i].c_str());
                } else if (tok[i] == "movetime") {
                    next(movetime);
                } else if (tok[i] == "wtime") {
                    next(wtime);
                } else if (tok[i] == "btime") {
                    next(btime);
                } else if (tok[i] == "winc") {
                    next(winc);
                } else if (tok[i] == "binc") {
                    next(binc);
                } else if (tok[i] == "movestogo") {
                    long long v = 0;
                    next(v);
                    movestogo = static_cast<int>(v);
                }
                // "infinite" and any other token: no explicit handling
                // needed -- the fallback 24h deadline below already means
                // "don't stop on your own, rely on depth or an explicit stop."
            }

            Color aiColor = board.sideToMove();
            bool haveMyTime = (aiColor == Color::White) ? (wtime >= 0) : (btime >= 0);
            if (movetime >= 0) {
                timeBudgetMs = movetime - moveOverheadMs;
            } else if (haveMyTime) {
                long long myTime = (aiColor == Color::White) ? wtime : btime;
                long long myInc = (aiColor == Color::White) ? winc : binc;
                int divisor = movestogo > 0 ? movestogo : 30;
                timeBudgetMs = myTime / divisor + myInc - moveOverheadMs;
                // Hard safety clamp regardless of the math above: must
                // never lose on time, only needs to be safe, not tuned
                // (that's later-phase scope).
                timeBudgetMs = std::min<long long>(timeBudgetMs, myTime / 2);
            }
            if (timeBudgetMs < 0) {
                // Only fall back to "search until depth/mate/stop" when no
                // time control was given at all (bare go / depth-only /
                // infinite -- timeBudgetMs is still its initial -1). If
                // movetime or wtime/btime WAS given but the computed budget
                // went negative (very little time left, or Move Overhead
                // larger than what's left), that's a real "must move
                // immediately" signal, not an absence of time control --
                // conflating the two is exactly how an engine loses on
                // time under real clock pressure.
                timeBudgetMs = (movetime >= 0 || haveMyTime) ? 1 : (24LL * 60 * 60 * 1000);
            }
            timeBudgetMs = std::max<long long>(timeBudgetMs, 1);

            Board searchBoard = board;
            SearchOptions opts;
            opts.useOpeningBook = ownBook;
            opts.threadOverride = threadsOption;
            opts.stopFlag = &stopFlag;
            auto start = std::chrono::steady_clock::now();
            opts.onDepthComplete = [&tt, searchBoard, start](const SearchProgress& p) {
                // p.bestMove is the authoritative move for this depth --
                // exactly what becomes `bestmove` if this is the last
                // completed depth. extractPV()'s own TT root-probe is NOT
                // guaranteed to agree with it: the TT persists across the
                // whole game, and TranspositionTable::store() skips
                // overwriting a same-key slot whose existing entry has
                // depth >= the new depth (see its own doc comment), so a
                // deeper, older entry for this exact position (or one
                // written by a transposition within this same depth's
                // search, via a check-extension line looping back to the
                // root) can outlive this depth's own root store. Walking
                // the TT for ply 0 can then report a different first move
                // than bestmove -- so ply 0 always comes from p.bestMove
                // (guaranteed correct by construction), and extractPV only
                // supplies the continuation from ply 1 onward.
                Board afterBest = searchBoard;
                afterBest.makeMove(p.bestMove);
                std::vector<Move> pv = {p.bestMove};
                std::vector<Move> rest = extractPV(afterBest, *tt, 31);
                pv.insert(pv.end(), rest.begin(), rest.end());
                double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                long long nps = secs > 0 ? static_cast<long long>(p.nodes / secs) : 0;
                std::printf("info depth %d score %s nodes %lld nps %lld pv %s\n", p.depth,
                            formatUciScore(p.score).c_str(), p.nodes, nps, formatUciMoveList(pv).c_str());
                std::fflush(stdout);
            };

            int tbMs = static_cast<int>(timeBudgetMs);
            searchTask = std::async(std::launch::async, [&tt, searchBoard, aiColor, maxDepth, tbMs, opts]() {
                Move best = findBestMove(searchBoard, aiColor, maxDepth, tbMs, *tt, opts);
                std::printf("bestmove %s\n", formatUciMove(best).c_str());
                std::fflush(stdout);
            });
        } else if (cmd == "stop") {
            stopFlag.store(true);
            joinSearch(); // blocks until the search thread notices and prints bestmove
        } else if (cmd == "quit") {
            stopFlag.store(true);
            joinSearch();
            break;
        }
        // Unrecognized commands are silently ignored, per UCI convention.
    }
    return 0;
}
