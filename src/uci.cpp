#include "search_params.hpp"
#include "uci.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ai.hpp"
#include "board.hpp"
#include "move.hpp"
#include "nnue.hpp"
#include "transposition_table.hpp"
#include "version.hpp"

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

// entries*kEntryBytes <= mb MiB, rounded down to a power of two
// (TranspositionTable's constructor requires one) and floored so a tiny or
// garbage Hash value can't produce a degenerate table.
size_t hashMBToEntries(int mb) {
    if (mb < 1) mb = 1;
    size_t bytes = static_cast<size_t>(mb) * 1024 * 1024;
    size_t entries = bytes / TranspositionTable::kEntryBytes;
    size_t p = size_t(1) << 16;
    while (p * 2 <= entries) p *= 2;
    return p;
}

// A whole-number option value; false for text that is not one ("setoption name Hash value lots").
bool parseInt(const std::string& s, int& out) {
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (s.empty() || end != s.c_str() + s.size() || errno == ERANGE) return false;
    out = static_cast<int>(std::clamp<long>(v, -1'000'000'000L, 1'000'000'000L));
    return true;
}

constexpr int kMaxHashMB = 4096;
constexpr long long kMaxSearchMs = 24LL * 60 * 60 * 1000; // "no limit" is a 24 hour deadline

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
#ifdef _WIN32
    std::setvbuf(stdout, nullptr, _IONBF, 0); // the Windows CRT rejects a line-buffered stream of size 0
#else
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif

    Board board;
    bool positionValid = true; // false after a "position fen" that could not be parsed, until the next valid one
    std::vector<uint64_t> gameHistory; // hashKey() of every position from "position", for repetition detection
    int hashMB = 224; // UCI "Hash" default; the GUI and --selfplay use kTTSize (ai.hpp) instead
    std::unique_ptr<TranspositionTable> tt;
    bool ownBook = false; // UCI default is OFF, unlike every other caller of findBestMove
    int hwThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 2);
    const int maxThreads = std::max(1, hwThreads * 4);
    int threadsOption = hwThreads;
    int moveOverheadMs = 50;
    bool ponderOption = false; // UCI "Ponder": GUIs only ponder when it is on, so "bestmove" names a ponder move only then

    // (Re)creates the table for the current Hash size. The old one goes first, so a big table is
    // never alive twice at once; if the memory is not there, the size is halved until it is.
    auto rebuildTT = [&]() {
        tt.reset();
        for (int mb = hashMB;; mb /= 2) {
            try {
                tt = std::make_unique<TranspositionTable>(hashMBToEntries(mb));
                break;
            } catch (const std::bad_alloc&) {
                if (mb <= 1) throw;
            }
            std::printf("info string cannot allocate %d MB of hash, trying %d MB\n", mb, mb / 2);
            std::fflush(stdout);
        }
    };
    rebuildTT();

    std::atomic<bool> stopFlag{false};
    // "go ponder" searches the position after the move the engine expects, with no time limit, until
    // "ponderhit" (the opponent played it: the search is kept and a timer ends it after the usual time
    // for a move) or "stop"/a new "position" (it did not: the search is thrown away).
    std::atomic<bool> pondering{false};
    std::atomic<bool> timerCancel{false};
    long long ponderOptimumMs = 0;
    std::thread ponderTimer;
    std::future<void> searchTask;
    auto joinSearch = [&]() {
        if (searchTask.valid()) searchTask.get();
    };
    // Ends a search that is still running. A GUI sends "stop" first, but one that does not must not
    // leave us waiting for an infinite search before the next command can be handled.
    auto haltSearch = [&]() {
        stopFlag.store(true);
        timerCancel.store(true);
        if (ponderTimer.joinable()) ponderTimer.join();
        joinSearch();
        pondering.store(false);
        stopFlag.store(false);
        timerCancel.store(false);
    };

    std::string line;
    while (std::getline(std::cin, line)) {
        std::vector<std::string> tok = splitTokens(line);
        if (tok.empty()) continue;
        const std::string& cmd = tok[0];

        try {
            if (cmd == "uci") {
                std::printf("id name ChessGame %s\n", CHESS_VERSION);
                std::printf("id author Marek\n");
                std::printf("option name Hash type spin default %d min 1 max %d\n", hashMB, kMaxHashMB);
                std::printf("option name Threads type spin default %d min 1 max %d\n", threadsOption, maxThreads);
                std::printf("option name OwnBook type check default false\n");
                std::printf("option name Move Overhead type spin default %d min 0 max 5000\n", moveOverheadMs);
                std::printf("option name Ponder type check default false\n");
                std::printf("option name EvalFile type string default <default>\n");
    #ifdef SPSA
                for (const SearchParam& sp : searchParamRegistry())
                    std::printf("option name %s type spin default %d min %d max %d\n", sp.name, sp.def, sp.lo, sp.hi);
    #endif
                std::printf("uciok\n");
                std::fflush(stdout);
            } else if (cmd == "isready") {
                // Must answer even mid-search -- the one command that does NOT
                // join the search thread first.
                std::printf("readyok\n");
                std::fflush(stdout);
            } else if (cmd == "setoption") {
                haltSearch();
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
                int number = 0;
                if (name == "Hash") {
                    if (parseInt(value, number)) {
                        hashMB = std::clamp(number, 1, kMaxHashMB);
                        rebuildTT();
                    }
                } else if (name == "Threads") {
                    if (parseInt(value, number)) threadsOption = std::clamp(number, 1, maxThreads);
                } else if (name == "OwnBook") {
                    ownBook = (value == "true");
                } else if (name == "Ponder") {
                    ponderOption = (value == "true");
                } else if (name == "Move Overhead") {
                    if (parseInt(value, number)) moveOverheadMs = std::clamp(number, 0, 5000);
                } else if (name == "EvalFile") {
                    // "<default>" or empty: the network built into this binary if there is one, else the
                    // hand-crafted evaluation; "none": the hand-crafted evaluation; otherwise a network file.
                    std::string error;
                    if (value.empty() || value == "<default>") {
                        nnue::unload();
                        nnue::loadBuiltin();
                    } else if (value == "none") {
                        nnue::unload();
                    } else if (!nnue::loadFile(value, error)) {
                        std::printf("info string EvalFile: %s\n", error.c_str());
                    }
                    board.refreshNnue();
                    rebuildTT(); // its entries (and their cached static evals) came from the other evaluation
                    std::printf("info string evaluation: %s\n", nnue::active() ? nnue::netName().c_str() : "hand-crafted");
                    std::fflush(stdout);
                }
    #ifdef SPSA
                for (const SearchParam& sp : searchParamRegistry())
                    if (name == sp.name) *sp.value = std::clamp(std::atoi(value.c_str()), sp.lo, sp.hi);
    #endif
                // Unrecognized option names are silently ignored, per UCI convention.
            } else if (cmd == "ucinewgame") {
                haltSearch();
                rebuildTT();
            } else if (cmd == "position") {
                haltSearch();
                size_t i = 1;
                if (i < tok.size() && tok[i] == "startpos") {
                    board = Board();
                    positionValid = true;
                    i++;
                } else if (i < tok.size() && tok[i] == "fen") {
                    i++;
                    std::string fen;
                    while (i < tok.size() && tok[i] != "moves") {
                        if (!fen.empty()) fen += ' ';
                        fen += tok[i];
                        i++;
                    }
                    // A malformed or illegal FEN must not crash the engine or leave the old position in
                    // place under the new moves: no position is set until a valid one arrives.
                    if (!Board::tryFromFEN(fen, board)) {
                        positionValid = false;
                        std::printf("info string invalid FEN, position ignored: %s\n", fen.c_str());
                        std::fflush(stdout);
                        continue;
                    }
                    positionValid = true;
                }
                gameHistory.assign(1, board.hashKey());
                if (i < tok.size() && tok[i] == "moves") {
                    i++;
                    for (; i < tok.size(); i++) {
                        Move m;
                        if (!parseUciMove(board, tok[i], m)) break; // malformed input: stop applying
                        board.makeMove(m);
                        gameHistory.push_back(board.hashKey());
                    }
                }
            } else if (cmd == "go") {
                haltSearch();

                if (!positionValid) {
                    std::printf("info string no valid position\nbestmove 0000\n");
                    std::fflush(stdout);
                    continue;
                }
                const Color aiColor = board.sideToMove();
                if (board.legalMoves(aiColor).empty()) { // checkmate or stalemate: there is nothing to play
                    std::printf("bestmove 0000\n");
                    std::fflush(stdout);
                    continue;
                }
                int maxDepth = 64;
                long long timeBudgetMs = -1;
                long long softTimeMs = 0;
                long long movetime = -1, wtime = 0, btime = 0, winc = 0, binc = 0;
                bool haveWtime = false, haveBtime = false, infinite = false, ponder = false;
                int movestogo = 0;
                long long nodeLimit = 0;
                std::vector<Move> searchMoves;
                for (size_t i = 1; i < tok.size(); i++) {
                    // Reads the number after the current token, if there is one.
                    auto next = [&](long long& dst) {
                        if (i + 1 >= tok.size()) return false;
                        dst = std::atoll(tok[++i].c_str());
                        return true;
                    };
                    if (tok[i] == "depth") {
                        long long v = 0;
                        if (next(v)) maxDepth = static_cast<int>(std::clamp<long long>(v, 1, 64));
                    } else if (tok[i] == "movetime") {
                        next(movetime);
                    } else if (tok[i] == "wtime") {
                        haveWtime = next(wtime);
                    } else if (tok[i] == "btime") {
                        haveBtime = next(btime);
                    } else if (tok[i] == "winc") {
                        next(winc);
                    } else if (tok[i] == "binc") {
                        next(binc);
                    } else if (tok[i] == "movestogo") {
                        long long v = 0;
                        if (next(v)) movestogo = static_cast<int>(std::clamp<long long>(v, 0, 1000));
                    } else if (tok[i] == "infinite") {
                        infinite = true;
                    } else if (tok[i] == "ponder") {
                        ponder = true;
                    } else if (tok[i] == "nodes") {
                        next(nodeLimit);
                    } else if (tok[i] == "searchmoves") {
                        // The moves after it, up to the first token that is not a legal move here.
                        Move m;
                        while (i + 1 < tok.size() && parseUciMove(board, tok[i + 1], m)) {
                            searchMoves.push_back(m);
                            i++;
                        }
                    }
                    // Any other token: no explicit handling needed -- the fallback 24h deadline below
                    // already means "don't stop on your own, rely on depth or an explicit stop."
                }

                // A clock that is given but negative (a GUI whose player has flagged) is a clock with no time left.
                bool haveMyTime = (aiColor == Color::White) ? haveWtime : haveBtime;
                if (movetime >= 0) {
                    timeBudgetMs = movetime - moveOverheadMs;
                } else if (haveMyTime) {
                    long long myTime = std::max<long long>(0, (aiColor == Color::White) ? wtime : btime);
                    long long myInc = (aiColor == Color::White) ? winc : binc;
                    int divisor = movestogo > 0 ? movestogo : 30;
                    // What this move should take on average. No new depth is
                    // started after half of it (the soft limit), but one under
                    // way may run to twice it: an unfinished depth is thrown
                    // away, so stopping between depths wastes less of the clock
                    // than cutting one off. Never more than half the clock left.
                    long long optimum = myTime / divisor + myInc - moveOverheadMs;
                    softTimeMs = optimum / 2;
                    timeBudgetMs = std::min<long long>(optimum * 2, myTime / 2);
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
                    timeBudgetMs = (movetime >= 0 || haveMyTime) ? 1 : kMaxSearchMs;
                }
                if (infinite) { // searches until "stop", whatever clock came with it
                    timeBudgetMs = kMaxSearchMs;
                    softTimeMs = 0;
                }
                if (ponder && !infinite) { // no limit until "ponderhit"; the time it will then get is kept here
                    ponderOptimumMs = softTimeMs > 0 ? std::min<long long>(softTimeMs * 2, timeBudgetMs) : timeBudgetMs;
                    timeBudgetMs = kMaxSearchMs;
                    softTimeMs = 0;
                    pondering.store(true);
                }
                timeBudgetMs = std::clamp<long long>(timeBudgetMs, 1, kMaxSearchMs);

                Board searchBoard = board;
                SearchOptions opts;
                opts.useOpeningBook = ownBook;
                opts.threadOverride = threadsOption;
                opts.stopFlag = &stopFlag;
                opts.gameHistory = gameHistory;
                opts.softTimeMs = static_cast<int>(std::max<long long>(softTimeMs, 0));
                opts.nodeLimit = std::max<long long>(nodeLimit, 0);
                opts.searchMoves = searchMoves;
                opts.endgameDepthBonus = false; // "go depth N" searches N plies, endgame or not
                auto start = std::chrono::steady_clock::now();
                // The first two moves of the last completed depth's line: what the engine will play and
                // what it expects back (written by the search's first thread, read once it has ended).
                auto line = std::make_shared<std::pair<std::string, std::string>>();
                opts.onDepthComplete = [&tt, searchBoard, start, history = gameHistory, line](const SearchProgress& p) {
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
                    std::vector<Move> rest = extractPV(afterBest, *tt, 31, history);
                    pv.insert(pv.end(), rest.begin(), rest.end());
                    line->first = formatUciMove(pv[0]);
                    line->second = pv.size() > 1 ? formatUciMove(pv[1]) : "";
                    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                    long long nps = secs > 0 ? static_cast<long long>(p.nodes / secs) : 0;
                    std::printf("info depth %d score %s time %lld nodes %lld nps %lld hashfull %d pv %s\n", p.depth,
                                formatUciScore(p.score).c_str(), static_cast<long long>(secs * 1000), p.nodes, nps,
                                tt->hashfull(), formatUciMoveList(pv).c_str());
                    std::fflush(stdout);
                };

                int tbMs = static_cast<int>(timeBudgetMs);
                searchTask = std::async(std::launch::async, [&tt, &stopFlag, &pondering, &ponderOption, searchBoard, aiColor,
                                                              maxDepth, tbMs, opts, infinite, line]() {
                    std::string best = "0000";
                    try {
                        best = formatUciMove(findBestMove(searchBoard, aiColor, maxDepth, tbMs, *tt, opts));
                    } catch (const std::exception& e) {
                        std::printf("info string search error: %s\n", e.what());
                    }
                    // "go infinite" must not answer before "stop" (or "quit"), even when the search ran out of depth;
                    // nor "go ponder" before "ponderhit" or "stop".
                    while ((infinite || pondering.load()) && !stopFlag.load())
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    if (ponderOption && best == line->first && !line->second.empty())
                        std::printf("bestmove %s ponder %s\n", best.c_str(), line->second.c_str());
                    else
                        std::printf("bestmove %s\n", best.c_str());
                    std::fflush(stdout);
                });
            } else if (cmd == "ponderhit") {
                if (pondering.exchange(false)) {
                    if (ponderTimer.joinable()) ponderTimer.join();
                    timerCancel.store(false);
                    ponderTimer = std::thread([&, ms = ponderOptimumMs]() {
                        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
                        while (std::chrono::steady_clock::now() < end && !timerCancel.load() && !stopFlag.load())
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        if (!timerCancel.load()) stopFlag.store(true);
                    });
                }
            } else if (cmd == "stop") {
                stopFlag.store(true);
                joinSearch(); // blocks until the search thread notices and prints bestmove
                timerCancel.store(true);
                if (ponderTimer.joinable()) ponderTimer.join();
                timerCancel.store(false);
                pondering.store(false);
            } else if (cmd == "quit") {
                stopFlag.store(true);
                timerCancel.store(true);
                if (ponderTimer.joinable()) ponderTimer.join();
                joinSearch();
                break;
            }
            // Unrecognized commands are silently ignored, per UCI convention.
        } catch (const std::exception& e) {
            // A bad command must never take the engine down mid-game.
            std::printf("info string error: %s\n", e.what());
            std::fflush(stdout);
        }
    }
    // stdin closed (the GUI went away): do not leave a search running.
    stopFlag.store(true);
    timerCancel.store(true);
    if (ponderTimer.joinable()) ponderTimer.join();
    joinSearch();
    return 0;
}
