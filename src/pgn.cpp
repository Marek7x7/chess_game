#include "pgn.hpp"

#include <cctype>

namespace {

bool isMoveNumberToken(const std::string& tok) {
    if (tok.empty() || !std::isdigit(static_cast<unsigned char>(tok[0]))) return false;
    size_t i = 0;
    while (i < tok.size() && std::isdigit(static_cast<unsigned char>(tok[i]))) i++;
    if (i == 0 || i >= tok.size()) return false;
    for (; i < tok.size(); i++) {
        if (tok[i] != '.') return false;
    }
    return true;
}

bool isResultToken(const std::string& tok) {
    return tok == "1-0" || tok == "0-1" || tok == "1/2-1/2" || tok == "*";
}

PieceType pieceTypeFromChar(char c) {
    switch (c) {
        case 'K': return PieceType::King;
        case 'Q': return PieceType::Queen;
        case 'R': return PieceType::Rook;
        case 'B': return PieceType::Bishop;
        case 'N': return PieceType::Knight;
        default: return PieceType::None;
    }
}

bool squareFromString(const std::string& s, int& x, int& y) {
    if (s.size() != 2) return false;
    x = s[0] - 'a';
    y = s[1] - '1';
    return x >= 0 && x < 8 && y >= 0 && y < 8;
}

} // namespace

std::vector<std::string> parsePgnMainline(const std::string& pgnText) {
    // Drop header ("[Tag \"...\"]") lines, keep everything else as movetext.
    std::string movetext;
    size_t start = 0;
    while (start <= pgnText.size()) {
        size_t nl = pgnText.find('\n', start);
        std::string line = pgnText.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        size_t firstNonSpace = line.find_first_not_of(" \t\r");
        if (firstNonSpace == std::string::npos || line[firstNonSpace] != '[') {
            movetext += line;
            movetext += ' ';
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }

    // Split parens into their own tokens so RAV nesting can be tracked.
    std::string spaced;
    spaced.reserve(movetext.size() * 2);
    for (char c : movetext) {
        if (c == '(' || c == ')') {
            spaced += ' ';
            spaced += c;
            spaced += ' ';
        } else {
            spaced += c;
        }
    }

    std::vector<std::string> rawTokens;
    size_t i = 0;
    while (i < spaced.size()) {
        while (i < spaced.size() && std::isspace(static_cast<unsigned char>(spaced[i]))) i++;
        size_t j = i;
        while (j < spaced.size() && !std::isspace(static_cast<unsigned char>(spaced[j]))) j++;
        if (j > i) rawTokens.push_back(spaced.substr(i, j - i));
        i = j;
    }

    std::vector<std::string> sanMoves;
    int parenDepth = 0;
    for (const std::string& tok : rawTokens) {
        if (tok == "(") {
            parenDepth++;
            continue;
        }
        if (tok == ")") {
            if (parenDepth > 0) parenDepth--;
            continue;
        }
        if (parenDepth > 0) continue; // inside a RAV variation, not the mainline
        if (isResultToken(tok)) break;
        if (tok[0] == '$') continue; // NAG (e.g. $2, $4)
        if (isMoveNumberToken(tok)) continue;
        sanMoves.push_back(tok);
    }
    return sanMoves;
}

bool resolveSanMove(const Board& board, const std::string& sanIn, Move& out) {
    std::string san = sanIn;
    while (!san.empty() && (san.back() == '+' || san.back() == '#' || san.back() == '!' || san.back() == '?')) {
        san.pop_back();
    }
    if (san.empty()) return false;

    Color color = board.sideToMove();
    MoveList legal = board.legalMoves(color);

    if (san == "O-O-O") {
        for (const Move& m : legal) {
            if (m.isCastle && m.toX == 2) {
                out = m;
                return true;
            }
        }
        return false;
    }
    if (san == "O-O") {
        for (const Move& m : legal) {
            if (m.isCastle && m.toX == 6) {
                out = m;
                return true;
            }
        }
        return false;
    }

    size_t idx = 0;
    PieceType pieceType = PieceType::Pawn;
    if (std::isupper(static_cast<unsigned char>(san[0])) && pieceTypeFromChar(san[0]) != PieceType::None) {
        pieceType = pieceTypeFromChar(san[0]);
        idx = 1;
    }

    std::string rest = san.substr(idx);

    PieceType promotion = PieceType::None;
    size_t eq = rest.find('=');
    if (eq != std::string::npos) {
        if (eq + 1 < rest.size()) promotion = pieceTypeFromChar(rest[eq + 1]);
        rest = rest.substr(0, eq);
    }

    std::string stripped;
    for (char c : rest) {
        if (c != 'x' && c != 'X') stripped += c;
    }
    if (stripped.size() < 2) return false;

    std::string destStr = stripped.substr(stripped.size() - 2);
    std::string disambig = stripped.substr(0, stripped.size() - 2);

    int destX, destY;
    if (!squareFromString(destStr, destX, destY)) return false;

    int disambigFile = -1, disambigRank = -1;
    for (char c : disambig) {
        if (c >= 'a' && c <= 'h') disambigFile = c - 'a';
        else if (c >= '1' && c <= '8') disambigRank = c - '1';
    }

    Move match{};
    int matches = 0;
    for (const Move& m : legal) {
        if (m.isCastle) continue;
        Piece p = board.at(m.fromX, m.fromY);
        if (p.type != pieceType) continue;
        if (m.toX != destX || m.toY != destY) continue;
        if (disambigFile != -1 && m.fromX != disambigFile) continue;
        if (disambigRank != -1 && m.fromY != disambigRank) continue;
        if (promotion != PieceType::None && m.promotion != promotion) continue;
        if (promotion == PieceType::None && m.promotion != PieceType::None) continue;
        match = m;
        matches++;
    }

    if (matches == 1) {
        out = match;
        return true;
    }
    return false;
}
