// Turns PGN files of engine games into training records (record.hpp) for the network.
//
//   nnue_data <out.bin> <games.pgn>... [--min-depth N] [--skip-plies N] [--max-score N] [--keep-noisy]
//
// The games are the ones fastchess writes: every move of the search carries a comment "{+0.95/12 0.318s}" (the
// engine's score in pawns from the mover's point of view, the depth and the time) and the moves from the opening
// book are tagged "{book}". The position BEFORE each searched move gets that score and the game's result. Left
// out: book moves, positions in check, mate scores, searches shallower than --min-depth, scores beyond
// --max-score, the first --skip-plies plies, and (unless --keep-noisy) moves that capture or promote, because
// the static value of a position in the middle of an exchange is not what the search score measures.
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "board.hpp"
#include "pgn.hpp"
#include "record.hpp"

namespace {

struct Options {
    int minDepth = 8;
    int skipPlies = 10;
    int maxScore = 2000;
    bool keepNoisy = false;
};

struct Counters {
    long long games = 0, badGames = 0, positions = 0;
    long long book = 0, shallow = 0, mate = 0, tooBig = 0, inCheck = 0, noisy = 0, early = 0, noScore = 0;
    long long wins = 0, draws = 0, losses = 0;
};

// "+0.95/12 0.318s" -> score 95, depth 12. False for "book", mate scores ("+M5/...") and anything else.
bool parseComment(const std::string& c, int& score, int& depth, bool& mate) {
    mate = false;
    if (c.size() < 4 || (c[0] != '+' && c[0] != '-')) return false;
    if (c[1] == 'M') {
        mate = true;
        return false;
    }
    char* end = nullptr;
    const double pawns = std::strtod(c.c_str(), &end);
    if (end == c.c_str() || *end != '/') return false;
    depth = std::atoi(end + 1);
    score = static_cast<int>(std::lround(pawns * 100.0));
    return true;
}

void processGame(const std::string& headers, const std::string& movetext, const Options& opt, Counters& n,
                 std::vector<nnue::data::Record>& out) {
    int whiteResult;
    const size_t r = headers.find("[Result \"");
    if (r == std::string::npos) return;
    const std::string res = headers.substr(r + 9, 7);
    if (res.compare(0, 3, "1-0") == 0) whiteResult = 1;
    else if (res.compare(0, 3, "0-1") == 0) whiteResult = -1;
    else if (res.compare(0, 7, "1/2-1/2") == 0) whiteResult = 0;
    else return; // unfinished
    Board board;
    const size_t f = headers.find("[FEN \"");
    if (f != std::string::npos) {
        const size_t e = headers.find('"', f + 6);
        if (e == std::string::npos || !Board::tryFromFEN(headers.substr(f + 6, e - f - 6), board)) {
            n.badGames++;
            return;
        }
    }
    n.games++;

    Board before = board; // the position the last move was played from
    Move last{};
    bool haveLast = false;
    int ply = 0;
    std::vector<nnue::data::Record> game;
    auto emit = [&](const std::string& comment) {
        if (!haveLast) return;
        haveLast = false;
        int score = 0, depth = 0;
        bool mate = false;
        if (!parseComment(comment, score, depth, mate)) {
            if (mate) n.mate++;
            else if (comment.compare(0, 4, "book") == 0) n.book++;
            else n.noScore++;
            return;
        }
        if (depth < opt.minDepth) { n.shallow++; return; }
        if (std::abs(score) > opt.maxScore) { n.tooBig++; return; }
        if (ply - 1 < opt.skipPlies) { n.early++; return; }
        if (before.isInCheck(before.sideToMove())) { n.inCheck++; return; }
        if (!opt.keepNoisy && (last.captured != PieceType::None || last.isEnPassant || last.promotion != PieceType::None)) {
            n.noisy++;
            return;
        }
        const int stmResult = before.sideToMove() == Color::White ? whiteResult : -whiteResult;
        game.push_back(nnue::data::makeRecord(before, score, stmResult, depth, ply - 1));
        n.positions++;
        (stmResult > 0 ? n.wins : stmResult < 0 ? n.losses : n.draws)++;
    };

    size_t i = 0;
    const size_t len = movetext.size();
    while (i < len) {
        const char c = movetext[i];
        if (std::isspace(static_cast<unsigned char>(c))) { i++; continue; }
        if (c == '{') {
            const size_t close = movetext.find('}', i);
            const std::string comment = movetext.substr(i + 1, close == std::string::npos ? std::string::npos : close - i - 1);
            emit(comment);
            i = close == std::string::npos ? len : close + 1;
            continue;
        }
        size_t j = i;
        while (j < len && !std::isspace(static_cast<unsigned char>(movetext[j])) && movetext[j] != '{') j++;
        const std::string tok = movetext.substr(i, j - i);
        i = j;
        if (tok == "1-0" || tok == "0-1" || tok == "1/2-1/2" || tok == "*") break;
        if (std::isdigit(static_cast<unsigned char>(tok[0])) && tok.find('.') != std::string::npos) continue; // "12." / "12..."
        if (tok[0] == '$') continue;
        emit("");               // the previous move had no comment
        Move m;
        if (!resolveSanMove(board, tok, m)) { n.badGames++; return; }
        before = board;
        last = m;
        haveLast = true;
        board.makeMove(m);
        ply++;
    }
    emit("");
    out.insert(out.end(), game.begin(), game.end());
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    std::vector<std::string> inputs;
    std::string outPath;
    for (int a = 1; a < argc; a++) {
        const std::string arg = argv[a];
        auto number = [&]() { return a + 1 < argc ? std::atoi(argv[++a]) : 0; };
        if (arg == "--min-depth") opt.minDepth = number();
        else if (arg == "--skip-plies") opt.skipPlies = number();
        else if (arg == "--max-score") opt.maxScore = number();
        else if (arg == "--keep-noisy") opt.keepNoisy = true;
        else if (outPath.empty()) outPath = arg;
        else inputs.push_back(arg);
    }
    if (outPath.empty() || inputs.empty()) {
        std::fprintf(stderr, "usage: nnue_data <out.bin> <games.pgn>... [--min-depth N] [--skip-plies N] [--max-score N] [--keep-noisy]\n");
        return 2;
    }
    Counters n;
    std::FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", outPath.c_str());
        return 1;
    }
    std::vector<nnue::data::Record> buffer;
    for (const std::string& path : inputs) {
        std::ifstream in(path);
        if (!in) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            continue;
        }
        std::string line, headers, movetext;
        bool inMoves = false;
        auto flush = [&]() {
            if (!headers.empty() && !movetext.empty()) processGame(headers, movetext, opt, n, buffer);
            headers.clear();
            movetext.clear();
            inMoves = false;
            if (buffer.size() >= (1u << 16)) {
                std::fwrite(buffer.data(), sizeof(nnue::data::Record), buffer.size(), out);
                buffer.clear();
            }
        };
        while (std::getline(in, line)) {
            if (line.compare(0, 6, "[Event") == 0 && inMoves) flush();
            if (!line.empty() && line[0] == '[') {
                headers += line;
                headers += '\n';
            } else if (!line.empty()) {
                inMoves = true;
                movetext += line;
                movetext += ' ';
            }
        }
        flush();
        std::fprintf(stderr, "%s: %lld games, %lld positions so far\n", path.c_str(), n.games, n.positions);
    }
    std::fwrite(buffer.data(), sizeof(nnue::data::Record), buffer.size(), out);
    std::fclose(out);
    std::printf("games %lld (unreadable %lld), positions kept %lld  [win %lld draw %lld loss %lld, side to move]\n", n.games,
                n.badGames, n.positions, n.wins, n.draws, n.losses);
    std::printf("left out: book %lld, no score %lld, mate score %lld, shallow %lld, |score| too big %lld, early %lld, in check %lld, noisy %lld\n",
                n.book, n.noScore, n.mate, n.shallow, n.tooBig, n.early, n.inCheck, n.noisy);
    return 0;
}
