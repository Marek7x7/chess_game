#include "gui.hpp"

#include <SDL2/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <optional>
#include <string>
#include <vector>

#include "ai.hpp"
#include "board.hpp"
#include "move.hpp"
#include "piece.hpp"

namespace {

constexpr int kSquare = 80;
constexpr int kBoardPx = kSquare * 8;
constexpr int kStatusH = 110;
constexpr int kWindowW = kBoardPx;
constexpr int kWindowH = kBoardPx + kStatusH;

struct DifficultyPreset {
    const char* label;
    int depthCap;
    int timeBudgetMs;
};

constexpr DifficultyPreset kDifficultyPresets[4] = {
    {"EASY", 3, 300},
    {"MEDIUM", 6, 1200},
    {"HARD", 10, 3000},
    {"EXPERT", 20, 6000},
};

// ---------------------------------------------------------------------
// Low-level drawing primitives (plain SDL2, no extra libraries)
// ---------------------------------------------------------------------

struct Point {
    float x, y;
};

void filledCircle(SDL_Renderer* r, float cx, float cy, float radius, SDL_Color color) {
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    int ri = static_cast<int>(radius);
    for (int dy = -ri; dy <= ri; dy++) {
        float dx = std::sqrt(std::max(0.0f, radius * radius - static_cast<float>(dy * dy)));
        SDL_RenderDrawLine(r, static_cast<int>(cx - dx), static_cast<int>(cy) + dy,
                            static_cast<int>(cx + dx), static_cast<int>(cy) + dy);
    }
}

void filledPolygon(SDL_Renderer* r, const std::vector<Point>& pts, SDL_Color color) {
    if (pts.size() < 3) return;
    float ymin = pts[0].y, ymax = pts[0].y;
    for (const auto& p : pts) {
        ymin = std::min(ymin, p.y);
        ymax = std::max(ymax, p.y);
    }
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    size_t n = pts.size();
    int y0 = static_cast<int>(std::floor(ymin));
    int y1 = static_cast<int>(std::ceil(ymax));
    for (int y = y0; y <= y1; y++) {
        std::vector<float> xs;
        for (size_t i = 0; i < n; i++) {
            const Point& a = pts[i];
            const Point& b = pts[(i + 1) % n];
            if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) {
                float t = (static_cast<float>(y) - a.y) / (b.y - a.y);
                xs.push_back(a.x + t * (b.x - a.x));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2) {
            SDL_RenderDrawLine(r, static_cast<int>(xs[i]), y, static_cast<int>(xs[i + 1]), y);
        }
    }
}

void outlinePolygon(SDL_Renderer* r, const std::vector<Point>& pts, SDL_Color color) {
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    size_t n = pts.size();
    for (size_t i = 0; i < n; i++) {
        const Point& a = pts[i];
        const Point& b = pts[(i + 1) % n];
        SDL_RenderDrawLine(r, static_cast<int>(a.x), static_cast<int>(a.y), static_cast<int>(b.x),
                            static_cast<int>(b.y));
    }
}

Point xf(Point p, float cx, float cy, float scale) { return {cx + p.x * scale, cy + p.y * scale}; }

std::vector<Point> xfAll(const std::vector<Point>& pts, float cx, float cy, float scale) {
    std::vector<Point> out;
    out.reserve(pts.size());
    for (const auto& p : pts) out.push_back(xf(p, cx, cy, scale));
    return out;
}

void filledOutlinedPolygon(SDL_Renderer* r, const std::vector<Point>& pts, SDL_Color fill, SDL_Color outline) {
    filledPolygon(r, pts, fill);
    outlinePolygon(r, pts, outline);
}

// ---------------------------------------------------------------------
// Tiny built-in vector font (stroke polylines on a 0..1 unit square).
// Only the characters the UI actually uses are defined; anything else
// (besides space) is silently skipped.
// ---------------------------------------------------------------------

using Stroke = std::vector<Point>;
using Glyph = std::vector<Stroke>;

Glyph glyphFor(char c) {
    switch (c) {
        case 'A': return {{{0, 1}, {0.5f, 0}, {1, 1}}, {{0.2f, 0.6f}, {0.8f, 0.6f}}};
        case 'B': return {{{0, 1}, {0, 0}, {0.7f, 0}, {0.85f, 0.25f}, {0.7f, 0.5f}, {0, 0.5f},
                            {0.75f, 0.5f}, {0.9f, 0.75f}, {0.75f, 1}, {0, 1}}};
        case 'C': return {{{0.85f, 0.15f}, {0.6f, 0}, {0.2f, 0}, {0, 0.25f}, {0, 0.75f}, {0.2f, 1},
                            {0.6f, 1}, {0.85f, 0.85f}}};
        case 'D': return {{{0, 1}, {0, 0}, {0.55f, 0}, {0.85f, 0.3f}, {0.85f, 0.7f}, {0.55f, 1}, {0, 1}}};
        case 'E': return {{{1, 0}, {0, 0}, {0, 1}, {1, 1}}, {{0, 0.5f}, {0.7f, 0.5f}}};
        case 'F': return {{{0, 1}, {0, 0}, {1, 0}}, {{0, 0.5f}, {0.7f, 0.5f}}};
        case 'G': return {{{0.85f, 0.15f}, {0.6f, 0}, {0.2f, 0}, {0, 0.25f}, {0, 0.75f}, {0.2f, 1},
                            {0.6f, 1}, {0.85f, 0.85f}, {0.85f, 0.55f}, {0.5f, 0.55f}}};
        case 'H': return {{{0, 0}, {0, 1}}, {{1, 0}, {1, 1}}, {{0, 0.5f}, {1, 0.5f}}};
        case 'I': return {{{0.25f, 0}, {0.75f, 0}}, {{0.5f, 0}, {0.5f, 1}}, {{0.25f, 1}, {0.75f, 1}}};
        case 'J': return {{{0.75f, 0}, {0.75f, 0.8f}, {0.55f, 1}, {0.25f, 0.9f}}};
        case 'K': return {{{0, 0}, {0, 1}}, {{0, 0.5f}, {0.9f, 0}}, {{0, 0.5f}, {0.9f, 1}}};
        case 'L': return {{{0, 0}, {0, 1}, {0.8f, 1}}};
        case 'M': return {{{0, 1}, {0, 0}, {0.5f, 0.55f}, {1, 0}, {1, 1}}};
        case 'N': return {{{0, 1}, {0, 0}, {1, 1}, {1, 0}}};
        case 'O': return {{{0.5f, 0}, {0.15f, 0.15f}, {0, 0.5f}, {0.15f, 0.85f}, {0.5f, 1}, {0.85f, 0.85f},
                            {1, 0.5f}, {0.85f, 0.15f}, {0.5f, 0}}};
        case 'P': return {{{0, 1}, {0, 0}, {0.6f, 0}, {0.85f, 0.2f}, {0.85f, 0.35f}, {0.6f, 0.5f}, {0, 0.5f}}};
        case 'Q': return {{{0.5f, 0}, {0.15f, 0.15f}, {0, 0.5f}, {0.15f, 0.85f}, {0.5f, 1}, {0.85f, 0.85f},
                            {1, 0.5f}, {0.85f, 0.15f}, {0.5f, 0}}, {{0.55f, 0.7f}, {1, 1}}};
        case 'R': return {{{0, 1}, {0, 0}, {0.6f, 0}, {0.85f, 0.2f}, {0.85f, 0.35f}, {0.6f, 0.5f}, {0, 0.5f}},
                           {{0, 0.5f}, {0.9f, 1}}};
        case 'S': return {{{0.85f, 0.15f}, {0.6f, 0}, {0.2f, 0}, {0, 0.2f}, {0, 0.35f}, {0.3f, 0.5f},
                            {0.7f, 0.5f}, {1, 0.65f}, {1, 0.8f}, {0.8f, 1}, {0.4f, 1}, {0.15f, 0.85f}}};
        case 'T': return {{{0, 0}, {1, 0}}, {{0.5f, 0}, {0.5f, 1}}};
        case 'U': return {{{0, 0}, {0, 0.75f}, {0.15f, 0.95f}, {0.5f, 1}, {0.85f, 0.95f}, {1, 0.75f}, {1, 0}}};
        case 'V': return {{{0, 0}, {0.5f, 1}, {1, 0}}};
        case 'W': return {{{0, 0}, {0.25f, 1}, {0.5f, 0.4f}, {0.75f, 1}, {1, 0}}};
        case 'X': return {{{0, 0}, {1, 1}}, {{1, 0}, {0, 1}}};
        case 'Y': return {{{0, 0}, {0.5f, 0.5f}, {1, 0}}, {{0.5f, 0.5f}, {0.5f, 1}}};
        case 'Z': return {{{0, 0}, {1, 0}, {0, 1}, {1, 1}}};
        case '1': return {{{0.3f, 0.2f}, {0.5f, 0}, {0.5f, 1}}, {{0.25f, 1}, {0.75f, 1}}};
        case '2': return {{{0, 0.25f}, {0.15f, 0.05f}, {0.5f, 0}, {0.75f, 0.15f}, {0.75f, 0.35f}, {0, 1}, {0.8f, 1}}};
        case '3': return {{{0, 0.15f}, {0.2f, 0}, {0.6f, 0}, {0.8f, 0.2f}, {0.8f, 0.35f}, {0.5f, 0.5f},
                            {0.8f, 0.65f}, {0.8f, 0.8f}, {0.6f, 1}, {0.2f, 1}, {0, 0.85f}}};
        case '4': return {{{0.65f, 1}, {0.65f, 0}, {0, 0.7f}, {0.9f, 0.7f}}};
        case '5': return {{{0.8f, 0}, {0, 0}, {0, 0.5f}, {0.5f, 0.5f}, {0.8f, 0.7f}, {0.8f, 0.85f},
                            {0.55f, 1}, {0.15f, 1}, {0, 0.85f}}};
        case '6': return {{{0.7f, 0.05f}, {0.4f, 0}, {0.15f, 0.15f}, {0, 0.5f}, {0, 0.8f}, {0.2f, 1},
                            {0.55f, 1}, {0.8f, 0.8f}, {0.8f, 0.65f}, {0.55f, 0.5f}, {0.2f, 0.5f}, {0, 0.65f}}};
        case '7': return {{{0, 0}, {1, 0}, {0.35f, 1}}};
        case '8': return {{{0.3f, 0.5f}, {0.15f, 0.35f}, {0.15f, 0.15f}, {0.3f, 0}, {0.65f, 0}, {0.8f, 0.15f},
                            {0.8f, 0.35f}, {0.65f, 0.5f}, {0.3f, 0.5f}},
                           {{0.3f, 0.5f}, {0.1f, 0.65f}, {0.1f, 0.85f}, {0.3f, 1}, {0.65f, 1}, {0.85f, 0.85f},
                            {0.85f, 0.65f}, {0.65f, 0.5f}, {0.3f, 0.5f}}};
        case '-': return {{{0.15f, 0.5f}, {0.85f, 0.5f}}};
        default: return {};
    }
}

void drawChar(SDL_Renderer* r, float x, float y, float size, SDL_Color color, char c) {
    char uc = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (uc == '.') {
        filledCircle(r, x + size * 0.5f, y + size * 0.85f, size * 0.08f, color);
        return;
    }
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    for (const Stroke& stroke : glyphFor(uc)) {
        for (size_t i = 0; i + 1 < stroke.size(); i++) {
            SDL_RenderDrawLine(r, static_cast<int>(x + stroke[i].x * size), static_cast<int>(y + stroke[i].y * size),
                                static_cast<int>(x + stroke[i + 1].x * size),
                                static_cast<int>(y + stroke[i + 1].y * size));
        }
    }
}

float charAdvance(float size) { return size * 0.85f; }
float textWidth(const std::string& text, float size) { return static_cast<float>(text.size()) * charAdvance(size); }

void drawText(SDL_Renderer* r, float x, float y, float size, SDL_Color color, const std::string& text) {
    float cx = x;
    for (char c : text) {
        if (c != ' ') drawChar(r, cx, y, size, color, c);
        cx += charAdvance(size);
    }
}

void drawTextCentered(SDL_Renderer* r, float centerX, float y, float size, SDL_Color color, const std::string& text) {
    drawText(r, centerX - textWidth(text, size) / 2.0f, y, size, color, text);
}

// ---------------------------------------------------------------------
// Vector piece icons
// ---------------------------------------------------------------------

SDL_Color pieceFillColor(Color c) { return c == Color::White ? SDL_Color{245, 245, 245, 255} : SDL_Color{40, 40, 40, 255}; }
SDL_Color pieceOutlineColor(Color c) { return c == Color::White ? SDL_Color{30, 30, 30, 255} : SDL_Color{225, 225, 225, 255}; }

void drawPawn(SDL_Renderer* r, float cx, float cy, float s, SDL_Color fill, SDL_Color outline) {
    filledCircle(r, cx, cy - 0.16f * s, 0.12f * s, fill);
    filledOutlinedPolygon(r, xfAll({{-0.12f, 0}, {0.12f, 0}, {0.22f, 0.3f}, {-0.22f, 0.3f}}, cx, cy, s), fill, outline);
    filledOutlinedPolygon(r, xfAll({{-0.28f, 0.3f}, {0.28f, 0.3f}, {0.28f, 0.38f}, {-0.28f, 0.38f}}, cx, cy, s), fill, outline);
}

void drawRook(SDL_Renderer* r, float cx, float cy, float s, SDL_Color fill, SDL_Color outline) {
    filledOutlinedPolygon(r, xfAll({{-0.22f, -0.05f}, {0.22f, -0.05f}, {0.22f, 0.3f}, {-0.22f, 0.3f}}, cx, cy, s), fill, outline);
    filledOutlinedPolygon(r, xfAll({{-0.22f, -0.32f}, {-0.1f, -0.32f}, {-0.1f, -0.05f}, {-0.22f, -0.05f}}, cx, cy, s), fill, outline);
    filledOutlinedPolygon(r, xfAll({{-0.06f, -0.32f}, {0.06f, -0.32f}, {0.06f, -0.05f}, {-0.06f, -0.05f}}, cx, cy, s), fill, outline);
    filledOutlinedPolygon(r, xfAll({{0.1f, -0.32f}, {0.22f, -0.32f}, {0.22f, -0.05f}, {0.1f, -0.05f}}, cx, cy, s), fill, outline);
    filledOutlinedPolygon(r, xfAll({{-0.3f, 0.3f}, {0.3f, 0.3f}, {0.3f, 0.4f}, {-0.3f, 0.4f}}, cx, cy, s), fill, outline);
}

void drawBishop(SDL_Renderer* r, float cx, float cy, float s, SDL_Color fill, SDL_Color outline) {
    filledOutlinedPolygon(r, xfAll({{0, -0.2f}, {0.18f, 0.28f}, {-0.18f, 0.28f}}, cx, cy, s), fill, outline);
    filledCircle(r, cx, cy - 0.3f * s, 0.09f * s, fill);
    filledOutlinedPolygon(r, xfAll({{-0.28f, 0.28f}, {0.28f, 0.28f}, {0.28f, 0.36f}, {-0.28f, 0.36f}}, cx, cy, s), fill, outline);
    Point a = xf({-0.05f, -0.05f}, cx, cy, s);
    Point b = xf({0.05f, 0.08f}, cx, cy, s);
    SDL_SetRenderDrawColor(r, outline.r, outline.g, outline.b, outline.a);
    SDL_RenderDrawLine(r, static_cast<int>(a.x), static_cast<int>(a.y), static_cast<int>(b.x), static_cast<int>(b.y));
}

void drawKnight(SDL_Renderer* r, float cx, float cy, float s, SDL_Color fill, SDL_Color outline) {
    filledOutlinedPolygon(r, xfAll({{0.15f, 0.35f}, {-0.25f, 0.35f}, {-0.25f, 0.15f}, {-0.1f, 0.05f},
                                     {-0.22f, -0.05f}, {-0.22f, -0.2f}, {-0.05f, -0.3f}, {0.1f, -0.28f},
                                     {0.2f, -0.15f}, {0.12f, -0.05f}, {0.22f, 0.05f}}, cx, cy, s), fill, outline);
    filledOutlinedPolygon(r, xfAll({{-0.3f, 0.35f}, {0.3f, 0.35f}, {0.3f, 0.42f}, {-0.3f, 0.42f}}, cx, cy, s), fill, outline);
}

void drawQueen(SDL_Renderer* r, float cx, float cy, float s, SDL_Color fill, SDL_Color outline) {
    filledOutlinedPolygon(r, xfAll({{-0.12f, -0.05f}, {0.12f, -0.05f}, {0.2f, 0.3f}, {-0.2f, 0.3f}}, cx, cy, s), fill, outline);
    for (float x : {-0.2f, -0.1f, 0.0f, 0.1f, 0.2f}) filledCircle(r, cx + x * s, cy - 0.28f * s, 0.045f * s, fill);
    filledOutlinedPolygon(r, xfAll({{-0.28f, 0.3f}, {0.28f, 0.3f}, {0.28f, 0.38f}, {-0.28f, 0.38f}}, cx, cy, s), fill, outline);
}

void drawKing(SDL_Renderer* r, float cx, float cy, float s, SDL_Color fill, SDL_Color outline) {
    filledOutlinedPolygon(r, xfAll({{-0.1f, -0.1f}, {0.1f, -0.1f}, {0.16f, 0.3f}, {-0.16f, 0.3f}}, cx, cy, s), fill, outline);
    filledOutlinedPolygon(r, xfAll({{-0.18f, -0.12f}, {0.18f, -0.12f}, {0.18f, -0.04f}, {-0.18f, -0.04f}}, cx, cy, s), fill, outline);
    filledOutlinedPolygon(r, xfAll({{-0.025f, -0.34f}, {0.025f, -0.34f}, {0.025f, -0.12f}, {-0.025f, -0.12f}}, cx, cy, s), fill, outline);
    filledOutlinedPolygon(r, xfAll({{-0.09f, -0.25f}, {0.09f, -0.25f}, {0.09f, -0.19f}, {-0.09f, -0.19f}}, cx, cy, s), fill, outline);
    filledOutlinedPolygon(r, xfAll({{-0.28f, 0.3f}, {0.28f, 0.3f}, {0.28f, 0.38f}, {-0.28f, 0.38f}}, cx, cy, s), fill, outline);
}

void drawPieceIcon(SDL_Renderer* r, PieceType type, Color color, float cx, float cy, float squareSize) {
    SDL_Color fill = pieceFillColor(color);
    SDL_Color outline = pieceOutlineColor(color);
    switch (type) {
        case PieceType::Pawn: drawPawn(r, cx, cy, squareSize, fill, outline); break;
        case PieceType::Knight: drawKnight(r, cx, cy, squareSize, fill, outline); break;
        case PieceType::Bishop: drawBishop(r, cx, cy, squareSize, fill, outline); break;
        case PieceType::Rook: drawRook(r, cx, cy, squareSize, fill, outline); break;
        case PieceType::Queen: drawQueen(r, cx, cy, squareSize, fill, outline); break;
        case PieceType::King: drawKing(r, cx, cy, squareSize, fill, outline); break;
        default: break;
    }
}

// ---------------------------------------------------------------------
// Board <-> screen coordinate mapping
// ---------------------------------------------------------------------

int toScreenX(int x, bool flipped) { return (flipped ? (7 - x) : x) * kSquare; }
int toScreenY(int y, bool flipped) { return (flipped ? y : (7 - y)) * kSquare; }
int toBoardX(int screenX, bool flipped) { int c = screenX / kSquare; return flipped ? 7 - c : c; }
int toBoardY(int screenY, bool flipped) { int rw = screenY / kSquare; return flipped ? rw : 7 - rw; }

// ---------------------------------------------------------------------
// UI widgets
// ---------------------------------------------------------------------

struct Button {
    SDL_Rect rect;
    std::string label;
};

bool pointInRect(int x, int y, const SDL_Rect& r) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

void drawButton(SDL_Renderer* r, const Button& b, SDL_Color bg, SDL_Color fg, float textSize) {
    SDL_SetRenderDrawColor(r, bg.r, bg.g, bg.b, bg.a);
    SDL_RenderFillRect(r, &b.rect);
    SDL_SetRenderDrawColor(r, fg.r, fg.g, fg.b, fg.a);
    SDL_RenderDrawRect(r, &b.rect);
    drawTextCentered(r, b.rect.x + b.rect.w / 2.0f, b.rect.y + b.rect.h / 2.0f - textSize / 2.0f, textSize, fg, b.label);
}

std::string colorName(Color c) { return c == Color::White ? "WHITE" : "BLACK"; }

// ---------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------

enum class Screen { MenuMain, MenuColor, MenuDifficulty, Game };
enum class DragState { None, Selected, Dragging };

struct App {
    SDL_Renderer* renderer;
    Screen screen = Screen::MenuMain;
    bool vsAi = false;
    Color humanColor = Color::White;
    Board board;
    bool flipped = false;

    int aiDepthCap = kDifficultyPresets[1].depthCap;
    int aiTimeBudgetMs = kDifficultyPresets[1].timeBudgetMs;
    std::optional<std::future<Move>> aiFuture;

    // Persists across the whole session (and across games, and across the
    // opponent's moves), rather than being rebuilt per move: a search often
    // benefits from what a previous search already learned about the same
    // subtrees, especially when the opponent plays a predicted response.
    TranspositionTable tt{kTTSize};

    DragState dragState = DragState::None;
    int selX = -1, selY = -1;
    int mouseX = 0, mouseY = 0;

    bool hasLastMove = false;
    Move lastMove{};

    bool promotionPending = false;
    int promoFromX = -1, promoFromY = -1, promoToX = -1, promoToY = -1;

    bool running = true;
};

void startGame(App& app) {
    app.board = Board();
    app.flipped = app.vsAi && app.humanColor == Color::Black;
    app.dragState = DragState::None;
    app.hasLastMove = false;
    app.promotionPending = false;
    app.aiFuture.reset();
    app.screen = Screen::Game;
}

// Attempts a move between two squares. If it's a promotion, opens the
// promotion picker instead of moving immediately.
void tryMove(App& app, int fromX, int fromY, int toX, int toY) {
    bool isPromotion = false;
    for (const Move& m : app.board.legalMoves(app.board.sideToMove())) {
        if (m.fromX == fromX && m.fromY == fromY && m.toX == toX && m.toY == toY && m.promotion != PieceType::None) {
            isPromotion = true;
            break;
        }
    }
    if (isPromotion) {
        app.promotionPending = true;
        app.promoFromX = fromX;
        app.promoFromY = fromY;
        app.promoToX = toX;
        app.promoToY = toY;
        return;
    }
    Move m;
    if (app.board.findLegalMove(fromX, fromY, toX, toY, PieceType::None, m)) {
        app.board.makeMove(m);
        app.hasLastMove = true;
        app.lastMove = m;
    }
}

void resolvePromotion(App& app, PieceType chosen) {
    Move m;
    if (app.board.findLegalMove(app.promoFromX, app.promoFromY, app.promoToX, app.promoToY, chosen, m)) {
        app.board.makeMove(m);
        app.hasLastMove = true;
        app.lastMove = m;
    }
    app.promotionPending = false;
}

// ---------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------

const SDL_Color kBg{30, 30, 34, 255};
const SDL_Color kTitleColor{235, 235, 235, 255};
const SDL_Color kButtonBg{60, 60, 68, 255};
const SDL_Color kButtonFg{230, 230, 230, 255};
const SDL_Color kLightSquare{238, 222, 196, 255};
const SDL_Color kDarkSquare{150, 110, 80, 255};
const SDL_Color kSelectHighlight{255, 215, 0, 110};
const SDL_Color kMoveHighlight{60, 160, 60, 130};
const SDL_Color kLastMoveHighlight{80, 130, 220, 90};
const SDL_Color kCheckHighlight{220, 40, 40, 140};

void renderMenuMain(App& app, Button& twoPlayers, Button& vsComputer) {
    SDL_Renderer* r = app.renderer;
    SDL_SetRenderDrawColor(r, kBg.r, kBg.g, kBg.b, kBg.a);
    SDL_RenderClear(r);
    drawTextCentered(r, kWindowW / 2.0f, 90, 48, kTitleColor, "CHESS");
    drawButton(r, twoPlayers, kButtonBg, kButtonFg, 20);
    drawButton(r, vsComputer, kButtonBg, kButtonFg, 20);
}

void renderMenuColor(App& app, Button& white, Button& black) {
    SDL_Renderer* r = app.renderer;
    SDL_SetRenderDrawColor(r, kBg.r, kBg.g, kBg.b, kBg.a);
    SDL_RenderClear(r);
    drawTextCentered(r, kWindowW / 2.0f, 90, 32, kTitleColor, "PLAY AS");
    drawButton(r, white, kButtonBg, kButtonFg, 20);
    drawButton(r, black, kButtonBg, kButtonFg, 20);
}

void renderMenuDifficulty(App& app, Button (&presetButtons)[4]) {
    SDL_Renderer* r = app.renderer;
    SDL_SetRenderDrawColor(r, kBg.r, kBg.g, kBg.b, kBg.a);
    SDL_RenderClear(r);
    drawTextCentered(r, kWindowW / 2.0f, 60, 32, kTitleColor, "DIFFICULTY");
    for (const Button& b : presetButtons) drawButton(r, b, kButtonBg, kButtonFg, 20);
}

void renderGame(App& app, Button& quitBtn, Button& newGameBtn) {
    SDL_Renderer* r = app.renderer;
    SDL_SetRenderDrawColor(r, kBg.r, kBg.g, kBg.b, kBg.a);
    SDL_RenderClear(r);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    Color turn = app.board.sideToMove();
    GameStatus status = app.board.status();
    bool gameOver = status == GameStatus::Checkmate || status == GameStatus::Stalemate;

    // Board squares.
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            SDL_Rect rect{toScreenX(x, app.flipped), toScreenY(y, app.flipped), kSquare, kSquare};
            SDL_Color base = ((x + y) % 2 == 0) ? kDarkSquare : kLightSquare;
            SDL_SetRenderDrawColor(r, base.r, base.g, base.b, base.a);
            SDL_RenderFillRect(r, &rect);
        }
    }

    // Last move highlight.
    if (app.hasLastMove) {
        for (auto [hx, hy] : {std::pair{app.lastMove.fromX, app.lastMove.fromY}, std::pair{app.lastMove.toX, app.lastMove.toY}}) {
            SDL_Rect rect{toScreenX(hx, app.flipped), toScreenY(hy, app.flipped), kSquare, kSquare};
            SDL_SetRenderDrawColor(r, kLastMoveHighlight.r, kLastMoveHighlight.g, kLastMoveHighlight.b, kLastMoveHighlight.a);
            SDL_RenderFillRect(r, &rect);
        }
    }

    // King-in-check highlight.
    if (status == GameStatus::Check || status == GameStatus::Checkmate) {
        for (int x = 0; x < 8; x++) {
            for (int y = 0; y < 8; y++) {
                Piece p = app.board.at(x, y);
                if (p.type == PieceType::King && p.color == turn) {
                    SDL_Rect rect{toScreenX(x, app.flipped), toScreenY(y, app.flipped), kSquare, kSquare};
                    SDL_SetRenderDrawColor(r, kCheckHighlight.r, kCheckHighlight.g, kCheckHighlight.b, kCheckHighlight.a);
                    SDL_RenderFillRect(r, &rect);
                }
            }
        }
    }

    // Selected square + legal destination markers.
    if (app.dragState != DragState::None) {
        SDL_Rect rect{toScreenX(app.selX, app.flipped), toScreenY(app.selY, app.flipped), kSquare, kSquare};
        SDL_SetRenderDrawColor(r, kSelectHighlight.r, kSelectHighlight.g, kSelectHighlight.b, kSelectHighlight.a);
        SDL_RenderFillRect(r, &rect);

        for (const Move& m : app.board.legalMoves(turn)) {
            if (m.fromX != app.selX || m.fromY != app.selY) continue;
            int cx = toScreenX(m.toX, app.flipped) + kSquare / 2;
            int cy = toScreenY(m.toY, app.flipped) + kSquare / 2;
            filledCircle(r, static_cast<float>(cx), static_cast<float>(cy), kSquare * 0.14f, kMoveHighlight);
        }
    }

    // Pieces (skip the one currently being dragged; drawn separately on top).
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            if (app.dragState == DragState::Dragging && x == app.selX && y == app.selY) continue;
            Piece p = app.board.at(x, y);
            if (p.isEmpty()) continue;
            int cx = toScreenX(x, app.flipped) + kSquare / 2;
            int cy = toScreenY(y, app.flipped) + kSquare / 2;
            drawPieceIcon(r, p.type, p.color, static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(kSquare));
        }
    }
    if (app.dragState == DragState::Dragging) {
        Piece p = app.board.at(app.selX, app.selY);
        drawPieceIcon(r, p.type, p.color, static_cast<float>(app.mouseX), static_cast<float>(app.mouseY), static_cast<float>(kSquare));
    }

    // File/rank labels drawn in the outer row/column of squares.
    SDL_Color labelLight{60, 45, 35, 255};
    SDL_Color labelDark{235, 225, 210, 255};
    for (int sx = 0; sx < 8; sx++) {
        for (int sy = 0; sy < 8; sy++) {
            bool edgeCol = sx == 0;
            bool edgeRow = sy == 7;
            if (!edgeCol && !edgeRow) continue;
            int bx = app.flipped ? 7 - sx : sx;
            int by = app.flipped ? sy : 7 - sy;
            SDL_Color labelColor = ((bx + by) % 2 == 0) ? labelDark : labelLight;
            int baseX = sx * kSquare, baseY = sy * kSquare;
            if (edgeCol) {
                std::string rank(1, static_cast<char>('1' + by));
                drawText(r, static_cast<float>(baseX + 4), static_cast<float>(baseY + 4), 12, labelColor, rank);
            }
            if (edgeRow) {
                std::string file(1, static_cast<char>('A' + bx));
                drawText(r, static_cast<float>(baseX + kSquare - 14), static_cast<float>(baseY + kSquare - 18), 12, labelColor, file);
            }
        }
    }

    // Status bar.
    SDL_Rect statusRect{0, kBoardPx, kWindowW, kStatusH};
    SDL_SetRenderDrawColor(r, kBg.r, kBg.g, kBg.b, kBg.a);
    SDL_RenderFillRect(r, &statusRect);

    std::string statusText;
    if (status == GameStatus::Checkmate) {
        statusText = "CHECKMATE - " + colorName(opponent(turn)) + " WINS";
    } else if (status == GameStatus::Stalemate) {
        statusText = "STALEMATE - DRAW";
    } else if (app.vsAi && turn != app.humanColor) {
        statusText = colorName(turn) + " (AI) IS THINKING...";
    } else if (status == GameStatus::Check) {
        statusText = colorName(turn) + " IS IN CHECK";
    } else {
        statusText = colorName(turn) + " TO MOVE";
    }
    drawText(r, 16, static_cast<float>(kBoardPx + 18), 22, kTitleColor, statusText);

    drawButton(r, quitBtn, kButtonBg, kButtonFg, 16);
    if (gameOver) drawButton(r, newGameBtn, kButtonBg, kButtonFg, 16);

    // Promotion picker modal.
    if (app.promotionPending) {
        SDL_Rect overlay{0, 0, kBoardPx, kBoardPx};
        SDL_SetRenderDrawColor(r, 0, 0, 0, 160);
        SDL_RenderFillRect(r, &overlay);

        const PieceType types[4] = {PieceType::Queen, PieceType::Rook, PieceType::Bishop, PieceType::Knight};
        const char* letters[4] = {"Q", "R", "B", "N"};
        int w = 90, gap = 20;
        int totalW = 4 * w + 3 * gap;
        int startX = (kBoardPx - totalW) / 2;
        int y = (kBoardPx - w) / 2;
        Color promotingColor = app.board.sideToMove();
        for (int i = 0; i < 4; i++) {
            SDL_Rect box{startX + i * (w + gap), y, w, w};
            SDL_SetRenderDrawColor(r, kButtonBg.r, kButtonBg.g, kButtonBg.b, 255);
            SDL_RenderFillRect(r, &box);
            SDL_SetRenderDrawColor(r, kButtonFg.r, kButtonFg.g, kButtonFg.b, 255);
            SDL_RenderDrawRect(r, &box);
            drawPieceIcon(r, types[i], promotingColor, static_cast<float>(box.x + w / 2), static_cast<float>(box.y + w / 2 - 8), static_cast<float>(w) * 0.8f);
            drawTextCentered(r, box.x + w / 2.0f, box.y + w - 20.0f, 16, kButtonFg, letters[i]);
        }
    }

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

} // namespace

int runGui() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("Chess", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kWindowW, kWindowH, SDL_WINDOW_SHOWN);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    App app;
    app.renderer = renderer;

    Button twoPlayersBtn{{kWindowW / 2 - 110, 220, 220, 50}, "TWO PLAYERS"};
    Button vsComputerBtn{{kWindowW / 2 - 110, 300, 220, 50}, "VS COMPUTER"};
    Button playWhiteBtn{{kWindowW / 2 - 110, 220, 220, 50}, "PLAY AS WHITE"};
    Button playBlackBtn{{kWindowW / 2 - 110, 300, 220, 50}, "PLAY AS BLACK"};
    Button quitBtn{{kWindowW - 116, kBoardPx + kStatusH - 50, 100, 34}, "QUIT"};
    Button newGameBtn{{kWindowW / 2 - 90, kBoardPx + 55, 180, 36}, "NEW GAME"};
    Button difficultyBtns[4] = {
        {{kWindowW / 2 - 110, 130, 220, 50}, kDifficultyPresets[0].label},
        {{kWindowW / 2 - 110, 200, 220, 50}, kDifficultyPresets[1].label},
        {{kWindowW / 2 - 110, 270, 220, 50}, kDifficultyPresets[2].label},
        {{kWindowW / 2 - 110, 340, 220, 50}, kDifficultyPresets[3].label},
    };

    while (app.running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                app.running = false;
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                app.dragState = DragState::None;
                app.promotionPending = false;
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) {
                app.dragState = DragState::None;
                app.promotionPending = false;
            } else if (app.screen == Screen::MenuMain && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                if (pointInRect(e.button.x, e.button.y, twoPlayersBtn.rect)) {
                    app.vsAi = false;
                    startGame(app);
                } else if (pointInRect(e.button.x, e.button.y, vsComputerBtn.rect)) {
                    app.screen = Screen::MenuColor;
                }
            } else if (app.screen == Screen::MenuColor && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                if (pointInRect(e.button.x, e.button.y, playWhiteBtn.rect)) {
                    app.vsAi = true;
                    app.humanColor = Color::White;
                    app.screen = Screen::MenuDifficulty;
                } else if (pointInRect(e.button.x, e.button.y, playBlackBtn.rect)) {
                    app.vsAi = true;
                    app.humanColor = Color::Black;
                    app.screen = Screen::MenuDifficulty;
                }
            } else if (app.screen == Screen::MenuDifficulty && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                for (int i = 0; i < 4; i++) {
                    if (pointInRect(e.button.x, e.button.y, difficultyBtns[i].rect)) {
                        app.aiDepthCap = kDifficultyPresets[i].depthCap;
                        app.aiTimeBudgetMs = kDifficultyPresets[i].timeBudgetMs;
                        startGame(app);
                        break;
                    }
                }
            } else if (app.screen == Screen::Game) {
                GameStatus status = app.board.status();
                bool gameOver = status == GameStatus::Checkmate || status == GameStatus::Stalemate;
                bool humansTurn = !app.vsAi || app.board.sideToMove() == app.humanColor;

                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    if (pointInRect(e.button.x, e.button.y, quitBtn.rect)) {
                        app.running = false;
                    } else if (gameOver && pointInRect(e.button.x, e.button.y, newGameBtn.rect)) {
                        app.screen = Screen::MenuMain;
                    } else if (app.promotionPending) {
                        const PieceType types[4] = {PieceType::Queen, PieceType::Rook, PieceType::Bishop, PieceType::Knight};
                        int w = 90, gap = 20;
                        int totalW = 4 * w + 3 * gap;
                        int startX = (kBoardPx - totalW) / 2;
                        int y = (kBoardPx - w) / 2;
                        for (int i = 0; i < 4; i++) {
                            SDL_Rect box{startX + i * (w + gap), y, w, w};
                            if (pointInRect(e.button.x, e.button.y, box)) resolvePromotion(app, types[i]);
                        }
                    } else if (!gameOver && humansTurn && e.button.y < kBoardPx) {
                        int bx = toBoardX(e.button.x, app.flipped);
                        int by = toBoardY(e.button.y, app.flipped);
                        Piece p = app.board.at(bx, by);
                        if (app.dragState == DragState::Selected) {
                            if (bx == app.selX && by == app.selY) {
                                app.dragState = DragState::None;
                            } else {
                                tryMove(app, app.selX, app.selY, bx, by);
                                app.dragState = DragState::None;
                            }
                        } else if (!p.isEmpty() && p.color == app.board.sideToMove()) {
                            app.selX = bx;
                            app.selY = by;
                            app.dragState = DragState::Dragging;
                            app.mouseX = e.button.x;
                            app.mouseY = e.button.y;
                        }
                    }
                } else if (e.type == SDL_MOUSEMOTION && app.dragState == DragState::Dragging) {
                    app.mouseX = e.motion.x;
                    app.mouseY = e.motion.y;
                } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT && app.dragState == DragState::Dragging) {
                    int bx = toBoardX(e.button.x, app.flipped);
                    int by = toBoardY(e.button.y, app.flipped);
                    if (bx == app.selX && by == app.selY) {
                        app.dragState = DragState::Selected;
                    } else {
                        tryMove(app, app.selX, app.selY, bx, by);
                        app.dragState = DragState::None;
                    }
                }
            }
        }

        if (app.screen == Screen::Game && !app.promotionPending) {
            GameStatus status = app.board.status();
            bool gameOver = status == GameStatus::Checkmate || status == GameStatus::Stalemate;
            if (!gameOver && app.vsAi && app.board.sideToMove() != app.humanColor) {
                if (!app.aiFuture) {
                    Board snapshot = app.board;
                    Color aiColor = app.board.sideToMove();
                    int depth = app.aiDepthCap, budget = app.aiTimeBudgetMs;
                    app.aiFuture = std::async(std::launch::async, [snapshot, aiColor, depth, budget, &tt = app.tt] {
                        return findBestMove(snapshot, aiColor, depth, budget, tt);
                    });
                } else if (app.aiFuture->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    Move m = app.aiFuture->get();
                    app.aiFuture.reset();
                    app.board.makeMove(m);
                    app.hasLastMove = true;
                    app.lastMove = m;
                }
            }
        }

        switch (app.screen) {
            case Screen::MenuMain: renderMenuMain(app, twoPlayersBtn, vsComputerBtn); break;
            case Screen::MenuColor: renderMenuColor(app, playWhiteBtn, playBlackBtn); break;
            case Screen::MenuDifficulty: renderMenuDifficulty(app, difficultyBtns); break;
            case Screen::Game: renderGame(app, quitBtn, newGameBtn); break;
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
