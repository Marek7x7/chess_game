#include "gui.hpp"

#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <vector>

#include "ai.hpp"
#include "board.hpp"
#include "match.hpp"
#include "move.hpp"
#include "piece.hpp"

namespace {

constexpr int kSquare = 80;
constexpr int kBoardPx = kSquare * 8;
constexpr int kStatusH = 110;
constexpr int kWindowW = kBoardPx;
constexpr int kWindowH = kBoardPx + kStatusH;

// Bot-vs-bot match setup: how the game-count buttons step, and the range.
constexpr int kGameCountSteps[4] = {-10, -1, 1, 10};
constexpr int kMaxMatchGames = 99;

// Layout for Screen::CustomSetup: a shrunk, editable board flanked by two
// piece palettes (one per side, same width), all inside the normal
// kWindowW-wide canvas -- 80 + 480 + 80 == 640 with no leftover slack.
constexpr int kSetupSquare = 60;
constexpr int kSetupBoardPx = kSetupSquare * 8;
constexpr int kPaletteSlot = 80;
constexpr int kSetupBoardX = kPaletteSlot;
constexpr int kSetupBoardY = 56;

constexpr PieceType kPaletteOrder[6] = {
    PieceType::Pawn, PieceType::Knight, PieceType::Bishop,
    PieceType::Rook, PieceType::Queen, PieceType::King,
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
        case '9': return {{{0.1f, 0.95f}, {0.4f, 1}, {0.65f, 0.85f}, {0.8f, 0.5f}, {0.8f, 0.2f}, {0.6f, 0},
                            {0.25f, 0}, {0, 0.2f}, {0, 0.35f}, {0.25f, 0.5f}, {0.6f, 0.5f}, {0.8f, 0.35f}}};
        case '0': return {{{0.45f, 0}, {0.15f, 0.12f}, {0.05f, 0.5f}, {0.15f, 0.88f}, {0.45f, 1}, {0.75f, 0.88f},
                            {0.85f, 0.5f}, {0.75f, 0.12f}, {0.45f, 0}}};
        case '-': return {{{0.15f, 0.5f}, {0.85f, 0.5f}}};
        case '+': return {{{0.5f, 0.2f}, {0.5f, 0.8f}}, {{0.2f, 0.5f}, {0.8f, 0.5f}}};
        case '/': return {{{0.8f, 0}, {0.1f, 1}}};
        case '_': return {{{0, 1}, {0.85f, 1}}};
        case '(': return {{{0.6f, 0}, {0.35f, 0.25f}, {0.3f, 0.5f}, {0.35f, 0.75f}, {0.6f, 1}}};
        case ')': return {{{0.25f, 0}, {0.5f, 0.25f}, {0.55f, 0.5f}, {0.5f, 0.75f}, {0.25f, 1}}};
        default: return {};
    }
}

void drawChar(SDL_Renderer* r, float x, float y, float size, SDL_Color color, char c) {
    char uc = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (uc == '.' || uc == ':') {
        filledCircle(r, x + size * 0.5f, y + size * 0.85f, size * 0.08f, color);
        if (uc == ':') filledCircle(r, x + size * 0.5f, y + size * 0.35f, size * 0.08f, color);
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

// Same idea as toScreenX/Y/toBoardX/Y above, but for the smaller, inset
// board used by Screen::CustomSetup. Callers must confirm a point falls
// inside the setup board rect (pointInRect against kSetupBoardX/Y sized
// kSetupBoardPx) before converting it with the BoardX/Y direction -- the
// subtraction below can go negative for an off-board point (e.g. a click in
// a palette), and integer division truncates toward zero rather than
// flooring, so an out-of-bounds point can silently alias into a
// valid-looking column instead of an obviously invalid one.
int setupToScreenX(int x, bool flipped) { return kSetupBoardX + (flipped ? (7 - x) : x) * kSetupSquare; }
int setupToScreenY(int y, bool flipped) { return kSetupBoardY + (flipped ? y : (7 - y)) * kSetupSquare; }
int setupToBoardX(int screenX, bool flipped) { int c = (screenX - kSetupBoardX) / kSetupSquare; return flipped ? 7 - c : c; }
int setupToBoardY(int screenY, bool flipped) { int rw = (screenY - kSetupBoardY) / kSetupSquare; return flipped ? rw : 7 - rw; }

// ---------------------------------------------------------------------
// UI widgets
// ---------------------------------------------------------------------

struct Button {
    SDL_Rect rect;
    std::string label;
};

bool pointInRect(int x, int y, const SDL_Rect& r) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

// Finds which CustomSetup palette slot (if any) contains (px, py), and
// reports the piece type/color that slot stamps. Piece order top-to-bottom
// is defined once via kPaletteOrder and shared with rendering.
bool paletteSlotAt(int px, int py, const SDL_Rect (&black)[6], const SDL_Rect (&white)[6], PieceType& outType, Color& outColor) {
    for (int i = 0; i < 6; i++) {
        if (pointInRect(px, py, black[i])) { outType = kPaletteOrder[i]; outColor = Color::Black; return true; }
        if (pointInRect(px, py, white[i])) { outType = kPaletteOrder[i]; outColor = Color::White; return true; }
    }
    return false;
}

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

enum class Screen { MenuMain, MenuColor, MenuDifficulty, CustomSetup, BotSetup, Game };
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
    std::atomic<bool> aiStop{false}; // aborts the search behind aiFuture (quitting, leaving a bot match)

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

    // Staging state for the "Custom" board-setup flow (Screen::CustomSetup).
    bool customMode = false;
    Piece customGrid[8][8]; // Piece{} = empty; Board's squares are private so this can't be `board` itself
    bool customDragActive = false;
    bool customDragFromBoard = false; // false = stamped from a palette (infinite); true = repositioning
    PieceType customDragType = PieceType::None;
    Color customDragColor = Color::None;
    int customDragFromX = -1, customDragFromY = -1; // valid only when customDragFromBoard
    int customMouseX = 0, customMouseY = 0;
    std::string customErrorMsg; // empty = no error shown

    // Bot-vs-bot match: picked on Screen::BotSetup, then played on
    // Screen::Game while `match` is set. `board` mirrors botGame.board() so
    // the normal board rendering needs no bot-specific code.
    int setupBotLevel[2] = {1, 1}; // kDifficultyPresets index for Bot A, Bot B
    int setupGames = 1;
    std::optional<BotMatch> match;
    int botRound = 0; // 1-based number of the game on the board
    GameRecord botGame;
    std::optional<TranspositionTable> botTT[2]; // one per bot, rebuilt every game
    std::chrono::steady_clock::time_point nextGameAt; // end of the pause after a finished game
    std::string lastSavedPgn;                         // empty if the last save failed

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

// Clears the custom-setup staging grid to empty. Called every time
// MenuDifficulty routes into Screen::CustomSetup, so re-entry after an
// earlier custom session is guaranteed empty, not just the very first visit.
void resetCustomSetup(App& app) {
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++)
            app.customGrid[x][y] = Piece{};
    app.customDragActive = false;
    app.customErrorMsg.clear();
    app.flipped = app.humanColor == Color::Black; // Custom mode is always vs-AI, so vsAi is implicitly true
}

// Inverse of Board::fromFEN's placement parsing: rank 8 down to rank 1,
// file a to h, digits for consecutive empty squares, '/' between ranks.
std::string customGridToFenPlacement(const Piece (&grid)[8][8]) {
    std::string placement;
    for (int y = 7; y >= 0; y--) {
        int emptyRun = 0;
        for (int x = 0; x < 8; x++) {
            const Piece& p = grid[x][y];
            if (p.isEmpty()) { emptyRun++; continue; }
            if (emptyRun > 0) { placement += static_cast<char>('0' + emptyRun); emptyRun = 0; }
            placement += p.symbol();
        }
        if (emptyRun > 0) placement += static_cast<char>('0' + emptyRun);
        if (y > 0) placement += '/';
    }
    return placement;
}

// Grid-only legality gate for a custom position, checked before any Board is
// built from it. Board::fromFEN trusts its input completely: a color with
// zero kings leaves that color's internally-tracked king square stale (still
// wherever the default-constructed standard position put it), and a color
// with 2+ kings only ever tracks the last one scanned -- either way
// isInCheck()/legalMoves() would silently check the wrong square. Pawns on
// the back ranks are rejected too since pawn-move generation assumes that
// never happens in a reachable chess position.
bool validateCustomPosition(const Piece (&grid)[8][8], std::string& reason) {
    int whiteKings = 0, blackKings = 0;
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            if (grid[x][y].type != PieceType::King) continue;
            if (grid[x][y].color == Color::White) whiteKings++; else blackKings++;
        }
    }
    if (whiteKings != 1 || blackKings != 1) { reason = "NEED EXACTLY ONE KING PER SIDE"; return false; }
    for (int x = 0; x < 8; x++) {
        if (grid[x][0].type == PieceType::Pawn || grid[x][7].type == PieceType::Pawn) {
            reason = "NO PAWNS ON THE BACK RANKS";
            return false;
        }
    }
    return true;
}

// Validates the staged grid, and additionally rejects a position where the
// side moving second is already in check -- nothing in this engine stops
// the side to move from legally "capturing" such a king outright (no move
// generator special-cases the king as a capture target), which would
// silently corrupt check/checkmate detection for the rest of the game. On
// success, builds the Board and switches to Screen::Game exactly like
// startGame() does for the standard-position flow.
void finishCustomSetup(App& app, Color sideToMoveFirst) {
    std::string reason;
    if (!validateCustomPosition(app.customGrid, reason)) {
        app.customErrorMsg = reason;
        return;
    }
    std::string fen = customGridToFenPlacement(app.customGrid) +
                       (sideToMoveFirst == Color::White ? " w - -" : " b - -");
    Board candidate = Board::fromFEN(fen);
    if (candidate.isInCheck(opponent(sideToMoveFirst))) {
        app.customErrorMsg = "OPPONENT KING ALREADY IN CHECK";
        return;
    }
    app.board = candidate;
    app.flipped = app.humanColor == Color::Black;
    app.dragState = DragState::None;
    app.hasLastMove = false;
    app.promotionPending = false;
    app.aiFuture.reset();
    app.customMode = false;
    app.customDragActive = false;
    app.customErrorMsg.clear();
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

// Puts game `round` of app.match on the board, with a fresh transposition
// table for each bot (see runSelfplay for why they're per bot, per game).
void startBotGame(App& app, int round) {
    app.aiFuture.reset(); // never searching here, but the tables must not be replaced under a search
    app.botRound = round;
    app.botGame = GameRecord();
    app.board = app.botGame.board();
    for (auto& tt : app.botTT) tt.emplace(kTTSize);
    app.flipped = false;
    app.dragState = DragState::None;
    app.hasLastMove = false;
    app.promotionPending = false;
    app.screen = Screen::Game;
}

// Back to the main menu from a bot match, finished or not: a search still
// running is aborted rather than waited out, and an unfinished game is
// discarded (only finished games are saved).
void leaveBotMatch(App& app) {
    app.aiStop = true;
    app.aiFuture.reset();
    app.aiStop = false;
    app.match.reset();
    for (auto& tt : app.botTT) tt.reset();
    app.screen = Screen::MenuMain;
}

// Advances a bot match by at most one step per frame: starts or collects
// the side to move's search, saves the game once it's over, and starts the
// next one after a short pause so the final position stays readable.
void driveBotMatch(App& app) {
    BotMatch& match = *app.match;
    if (match.gamesPlayed == app.botRound) { // this game is over and already saved
        if (!match.finished() && std::chrono::steady_clock::now() >= app.nextGameAt) {
            startBotGame(app, app.botRound + 1);
        }
        return;
    }
    if (app.botGame.outcome().over) {
        app.lastSavedPgn = match.recordFinishedGame(app.botGame);
        if (!app.lastSavedPgn.empty()) std::printf("saved %s\n", app.lastSavedPgn.c_str());
        app.nextGameAt = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        return;
    }
    if (!app.aiFuture) {
        Color side = app.botGame.board().sideToMove();
        int bot = match.botToMove(app.botRound, side);
        const DifficultyPreset& preset = kDifficultyPresets[match.level[bot]];
        SearchOptions options;
        options.stopFlag = &app.aiStop;
        options.gameHistory = app.botGame.positionHashes();
        app.aiFuture = std::async(std::launch::async,
                                  [board = app.botGame.board(), side, preset, options, &tt = *app.botTT[bot]] {
                                      return findBestMove(board, side, preset.depthCap, preset.timeBudgetMs, tt,
                                                          options);
                                  });
    } else if (app.aiFuture->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Move m = app.aiFuture->get();
        app.aiFuture.reset();
        app.botGame.play(m);
        app.board = app.botGame.board();
        app.hasLastMove = true;
        app.lastMove = m;
    }
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
const SDL_Color kErrorColor{235, 90, 90, 255};
const SDL_Color kButtonSelectedBg{70, 110, 170, 255};
const SDL_Color kHintColor{160, 160, 170, 255};

void renderMenuMain(App& app, Button& twoPlayers, Button& vsComputer, Button& custom, Button& botVsBot) {
    SDL_Renderer* r = app.renderer;
    SDL_SetRenderDrawColor(r, kBg.r, kBg.g, kBg.b, kBg.a);
    SDL_RenderClear(r);
    drawTextCentered(r, kWindowW / 2.0f, 90, 48, kTitleColor, "CHESS");
    drawButton(r, twoPlayers, kButtonBg, kButtonFg, 20);
    drawButton(r, vsComputer, kButtonBg, kButtonFg, 20);
    drawButton(r, custom, kButtonBg, kButtonFg, 20);
    drawButton(r, botVsBot, kButtonBg, kButtonFg, 20);
}

void renderBotSetup(App& app, Button (&levelBtns)[2][4], Button (&gameCountBtns)[4], Button& startBtn,
                    Button& backBtn) {
    SDL_Renderer* r = app.renderer;
    SDL_SetRenderDrawColor(r, kBg.r, kBg.g, kBg.b, kBg.a);
    SDL_RenderClear(r);
    drawTextCentered(r, kWindowW / 2.0f, 40, 32, kTitleColor, "BOT VS BOT");
    drawTextCentered(r, kWindowW / 2.0f, 115, 16, kTitleColor, "BOT A (WHITE IN GAME 1)");
    drawTextCentered(r, kWindowW / 2.0f, 225, 16, kTitleColor, "BOT B");
    for (int bot = 0; bot < 2; bot++) {
        for (int i = 0; i < 4; i++) {
            drawButton(r, levelBtns[bot][i], i == app.setupBotLevel[bot] ? kButtonSelectedBg : kButtonBg, kButtonFg,
                       16);
        }
    }
    drawTextCentered(r, kWindowW / 2.0f, 335, 16, kTitleColor, "NUMBER OF GAMES");
    for (const Button& b : gameCountBtns) drawButton(r, b, kButtonBg, kButtonFg, 16);
    drawTextCentered(r, kWindowW / 2.0f, 376, 28, kTitleColor, std::to_string(app.setupGames));
    drawTextCentered(r, kWindowW / 2.0f, 440, 14, kHintColor, "COLORS SWAP EVERY GAME");
    drawButton(r, startBtn, kButtonBg, kButtonFg, 20);
    drawButton(r, backBtn, kButtonBg, kButtonFg, 14);
}

// "2.5" / "3" from a count of half points.
std::string pointsText(int halfPoints) { return std::to_string(halfPoints / 2) + (halfPoints % 2 ? ".5" : ""); }

// Bot-match status bar: what's happening on the board, the game number and
// running score, and the last saved PGN. Sized to stay left of the MENU and
// QUIT buttons in the right-hand column.
void drawBotMatchStatus(App& app) {
    SDL_Renderer* r = app.renderer;
    const BotMatch& match = *app.match;
    int half[2] = {2 * match.wins[0] + match.draws, 2 * match.wins[1] + match.draws};

    std::string headline;
    GameOutcome outcome = app.botGame.outcome();
    if (match.finished()) {
        int leader = half[0] >= half[1] ? 0 : 1;
        headline = "MATCH OVER - " +
                   (half[0] == half[1] ? std::string("TIED ")
                                       : std::string(leader == 0 ? "BOT A" : "BOT B") + " WINS ") +
                   pointsText(half[leader]) + "-" + pointsText(half[1 - leader]);
    } else if (outcome.over && outcome.reason == "checkmate") {
        Color winner = opponent(app.board.sideToMove());
        headline = "CHECKMATE - " + match.botName(match.botToMove(app.botRound, winner)) + " WINS";
    } else if (outcome.over) {
        headline = "DRAW - " + outcome.reason;
    } else {
        Color turn = app.board.sideToMove();
        headline = colorName(turn) + " - " + match.botName(match.botToMove(app.botRound, turn)) + " THINKING";
    }
    drawText(r, 16, static_cast<float>(kBoardPx + 14), 16, kTitleColor, headline);

    std::string score = "GAME " + std::to_string(app.botRound) + "/" + std::to_string(match.totalGames) +
                        " - BOT A " + pointsText(half[0]) + "  BOT B " + pointsText(half[1]);
    drawText(r, 16, static_cast<float>(kBoardPx + 42), 14, kTitleColor, score);

    if (match.gamesPlayed > 0) {
        std::filesystem::path saved(app.lastSavedPgn);
        std::string savedText = app.lastSavedPgn.empty()
                                    ? "COULD NOT SAVE THE PGN - SEE TERMINAL"
                                    : "SAVED " + (saved.parent_path().filename() / saved.filename()).generic_string();
        drawText(r, 16, static_cast<float>(kBoardPx + 70), 11,
                 app.lastSavedPgn.empty() ? kErrorColor : kHintColor, savedText);
    }
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

void renderCustomSetup(App& app, const SDL_Rect (&blackPalette)[6], const SDL_Rect (&whitePalette)[6],
                        Button& blackStartBtn, Button& whiteStartBtn, Button& backBtn) {
    SDL_Renderer* r = app.renderer;
    SDL_SetRenderDrawColor(r, kBg.r, kBg.g, kBg.b, kBg.a);
    SDL_RenderClear(r);

    drawTextCentered(r, kWindowW / 2.0f, 20, 24, kTitleColor, "CUSTOM SETUP");

    // Board squares.
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            SDL_Rect rect{setupToScreenX(x, app.flipped), setupToScreenY(y, app.flipped), kSetupSquare, kSetupSquare};
            SDL_Color base = ((x + y) % 2 == 0) ? kDarkSquare : kLightSquare;
            SDL_SetRenderDrawColor(r, base.r, base.g, base.b, base.a);
            SDL_RenderFillRect(r, &rect);
        }
    }

    // Palette slots: a filled/outlined square plus a centered piece icon.
    // Palettes never deplete -- they're just a fixed reference of what's
    // available to stamp, not inventory being consumed.
    for (int i = 0; i < 6; i++) {
        for (const SDL_Rect* slot : {&blackPalette[i], &whitePalette[i]}) {
            SDL_SetRenderDrawColor(r, kButtonBg.r, kButtonBg.g, kButtonBg.b, kButtonBg.a);
            SDL_RenderFillRect(r, slot);
            SDL_SetRenderDrawColor(r, kButtonFg.r, kButtonFg.g, kButtonFg.b, kButtonFg.a);
            SDL_RenderDrawRect(r, slot);
        }
        float bcx = blackPalette[i].x + blackPalette[i].w / 2.0f, bcy = blackPalette[i].y + blackPalette[i].h / 2.0f;
        float wcx = whitePalette[i].x + whitePalette[i].w / 2.0f, wcy = whitePalette[i].y + whitePalette[i].h / 2.0f;
        drawPieceIcon(r, kPaletteOrder[i], Color::Black, bcx, bcy, static_cast<float>(kPaletteSlot));
        drawPieceIcon(r, kPaletteOrder[i], Color::White, wcx, wcy, static_cast<float>(kPaletteSlot));
    }

    // Board pieces. A piece mid-drag from the board was already cleared out
    // of customGrid at pickup time, so there's nothing to skip here -- it's
    // simply absent until it's dropped somewhere.
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            Piece p = app.customGrid[x][y];
            if (p.isEmpty()) continue;
            int cx = setupToScreenX(x, app.flipped) + kSetupSquare / 2;
            int cy = setupToScreenY(y, app.flipped) + kSetupSquare / 2;
            drawPieceIcon(r, p.type, p.color, static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(kSetupSquare));
        }
    }
    if (app.customDragActive) {
        drawPieceIcon(r, app.customDragType, app.customDragColor, static_cast<float>(app.customMouseX),
                      static_cast<float>(app.customMouseY), static_cast<float>(kSetupSquare));
    }

    drawButton(r, blackStartBtn, kButtonBg, kButtonFg, 18);
    drawButton(r, whiteStartBtn, kButtonBg, kButtonFg, 18);
    drawButton(r, backBtn, kButtonBg, kButtonFg, 14);

    if (!app.customErrorMsg.empty()) {
        drawTextCentered(r, kWindowW / 2.0f, 624, 16, kErrorColor, app.customErrorMsg);
    }
}

void renderGame(App& app, Button& quitBtn, Button& newGameBtn, Button& botMenuBtn) {
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

    if (app.match) {
        drawBotMatchStatus(app);
        drawButton(r, botMenuBtn, kButtonBg, kButtonFg, 16);
    } else {
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
        if (gameOver) drawButton(r, newGameBtn, kButtonBg, kButtonFg, 16);
    }
    drawButton(r, quitBtn, kButtonBg, kButtonFg, 16);

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
    Button customBtn{{kWindowW / 2 - 110, 380, 220, 50}, "CUSTOM"};
    Button blackStartBtn{{110, 560, 200, 50}, "BLACK START"};
    Button whiteStartBtn{{330, 560, 200, 50}, "WHITE START"};
    Button backBtn{{240, 670, 160, 40}, "BACK TO MENU"};
    Button botVsBotBtn{{kWindowW / 2 - 110, 460, 220, 50}, "BOT VS BOT"};
    Button botLevelBtns[2][4];
    for (int bot = 0; bot < 2; bot++) {
        for (int i = 0; i < 4; i++) {
            botLevelBtns[bot][i] = {{36 + i * 146, bot == 0 ? 145 : 255, 130, 44}, kDifficultyPresets[i].label};
        }
    }
    Button gameCountBtns[4] = {
        {{150, 365, 70, 50}, "-10"},
        {{226, 365, 60, 50}, "-1"},
        {{354, 365, 60, 50}, "+1"},
        {{420, 365, 70, 50}, "+10"},
    };
    Button startMatchBtn{{kWindowW / 2 - 110, 490, 220, 50}, "START"};
    Button botBackBtn{{240, 580, 160, 40}, "BACK TO MENU"};
    Button botMenuBtn{{kWindowW - 116, kBoardPx + 12, 100, 34}, "MENU"};
    SDL_Rect setupBoardRect{kSetupBoardX, kSetupBoardY, kSetupBoardPx, kSetupBoardPx};
    SDL_Rect blackPalette[6];
    SDL_Rect whitePalette[6];
    for (int i = 0; i < 6; i++) {
        blackPalette[i] = SDL_Rect{0, kSetupBoardY + i * kPaletteSlot, kPaletteSlot, kPaletteSlot};
        whitePalette[i] = SDL_Rect{kWindowW - kPaletteSlot, kSetupBoardY + i * kPaletteSlot, kPaletteSlot, kPaletteSlot};
    }

    while (app.running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                app.running = false;
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                app.dragState = DragState::None;
                app.promotionPending = false;
                if (app.customDragActive && app.customDragFromBoard) {
                    app.customGrid[app.customDragFromX][app.customDragFromY] = Piece{app.customDragType, app.customDragColor};
                }
                app.customDragActive = false;
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) {
                app.dragState = DragState::None;
                app.promotionPending = false;
                if (app.customDragActive && app.customDragFromBoard) {
                    app.customGrid[app.customDragFromX][app.customDragFromY] = Piece{app.customDragType, app.customDragColor};
                }
                app.customDragActive = false;
            } else if (app.screen == Screen::MenuMain && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                if (pointInRect(e.button.x, e.button.y, twoPlayersBtn.rect)) {
                    app.vsAi = false;
                    app.customMode = false;
                    startGame(app);
                } else if (pointInRect(e.button.x, e.button.y, vsComputerBtn.rect)) {
                    app.customMode = false;
                    app.screen = Screen::MenuColor;
                } else if (pointInRect(e.button.x, e.button.y, customBtn.rect)) {
                    app.customMode = true;
                    app.screen = Screen::MenuColor;
                } else if (pointInRect(e.button.x, e.button.y, botVsBotBtn.rect)) {
                    app.screen = Screen::BotSetup;
                }
            } else if (app.screen == Screen::BotSetup && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                for (int bot = 0; bot < 2; bot++) {
                    for (int i = 0; i < 4; i++) {
                        if (pointInRect(e.button.x, e.button.y, botLevelBtns[bot][i].rect)) app.setupBotLevel[bot] = i;
                    }
                }
                for (int i = 0; i < 4; i++) {
                    if (pointInRect(e.button.x, e.button.y, gameCountBtns[i].rect)) {
                        app.setupGames = std::clamp(app.setupGames + kGameCountSteps[i], 1, kMaxMatchGames);
                    }
                }
                if (pointInRect(e.button.x, e.button.y, startMatchBtn.rect)) {
                    app.match.emplace(app.setupBotLevel[0], app.setupBotLevel[1], app.setupGames);
                    app.lastSavedPgn.clear();
                    startBotGame(app, 1);
                } else if (pointInRect(e.button.x, e.button.y, botBackBtn.rect)) {
                    app.screen = Screen::MenuMain;
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
                        if (app.customMode) {
                            resetCustomSetup(app);
                            app.screen = Screen::CustomSetup;
                        } else {
                            startGame(app);
                        }
                        break;
                    }
                }
            } else if (app.screen == Screen::CustomSetup) {
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    PieceType paletteType;
                    Color paletteColor;
                    if (pointInRect(e.button.x, e.button.y, backBtn.rect)) {
                        app.customMode = false;
                        app.screen = Screen::MenuMain;
                    } else if (pointInRect(e.button.x, e.button.y, blackStartBtn.rect)) {
                        finishCustomSetup(app, Color::Black);
                    } else if (pointInRect(e.button.x, e.button.y, whiteStartBtn.rect)) {
                        finishCustomSetup(app, Color::White);
                    } else if (paletteSlotAt(e.button.x, e.button.y, blackPalette, whitePalette, paletteType, paletteColor)) {
                        app.customDragActive = true;
                        app.customDragFromBoard = false;
                        app.customDragType = paletteType;
                        app.customDragColor = paletteColor;
                        app.customMouseX = e.button.x;
                        app.customMouseY = e.button.y;
                    } else if (pointInRect(e.button.x, e.button.y, setupBoardRect)) {
                        int bx = setupToBoardX(e.button.x, app.flipped);
                        int by = setupToBoardY(e.button.y, app.flipped);
                        Piece p = app.customGrid[bx][by];
                        if (!p.isEmpty()) {
                            app.customDragActive = true;
                            app.customDragFromBoard = true;
                            app.customDragType = p.type;
                            app.customDragColor = p.color;
                            app.customDragFromX = bx;
                            app.customDragFromY = by;
                            app.customGrid[bx][by] = Piece{};
                            app.customErrorMsg.clear();
                            app.customMouseX = e.button.x;
                            app.customMouseY = e.button.y;
                        }
                    }
                } else if (e.type == SDL_MOUSEMOTION && app.customDragActive) {
                    app.customMouseX = e.motion.x;
                    app.customMouseY = e.motion.y;
                } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT && app.customDragActive) {
                    if (pointInRect(e.button.x, e.button.y, setupBoardRect)) {
                        int bx = setupToBoardX(e.button.x, app.flipped);
                        int by = setupToBoardY(e.button.y, app.flipped);
                        app.customGrid[bx][by] = Piece{app.customDragType, app.customDragColor};
                        app.customErrorMsg.clear();
                    }
                    app.customDragActive = false;
                    app.customDragFromX = -1;
                    app.customDragFromY = -1;
                }
            } else if (app.screen == Screen::Game) {
                GameStatus status = app.board.status();
                bool gameOver = status == GameStatus::Checkmate || status == GameStatus::Stalemate;
                bool humansTurn = !app.vsAi || app.board.sideToMove() == app.humanColor;

                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    if (pointInRect(e.button.x, e.button.y, quitBtn.rect)) {
                        app.running = false;
                    } else if (app.match) { // bots play both sides: the board takes no input
                        if (pointInRect(e.button.x, e.button.y, botMenuBtn.rect)) leaveBotMatch(app);
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

        if (app.screen == Screen::Game && app.match) {
            driveBotMatch(app);
        } else if (app.screen == Screen::Game && !app.promotionPending) {
            GameStatus status = app.board.status();
            bool gameOver = status == GameStatus::Checkmate || status == GameStatus::Stalemate;
            if (!gameOver && app.vsAi && app.board.sideToMove() != app.humanColor) {
                if (!app.aiFuture) {
                    Board snapshot = app.board;
                    Color aiColor = app.board.sideToMove();
                    int depth = app.aiDepthCap, budget = app.aiTimeBudgetMs;
                    SearchOptions options;
                    options.stopFlag = &app.aiStop;
                    app.aiFuture = std::async(std::launch::async,
                                              [snapshot, aiColor, depth, budget, options, &tt = app.tt] {
                                                  return findBestMove(snapshot, aiColor, depth, budget, tt, options);
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
            case Screen::MenuMain: renderMenuMain(app, twoPlayersBtn, vsComputerBtn, customBtn, botVsBotBtn); break;
            case Screen::MenuColor: renderMenuColor(app, playWhiteBtn, playBlackBtn); break;
            case Screen::MenuDifficulty: renderMenuDifficulty(app, difficultyBtns); break;
            case Screen::CustomSetup: renderCustomSetup(app, blackPalette, whitePalette, blackStartBtn, whiteStartBtn, backBtn); break;
            case Screen::BotSetup: renderBotSetup(app, botLevelBtns, gameCountBtns, startMatchBtn, botBackBtn); break;
            case Screen::Game: renderGame(app, quitBtn, newGameBtn, botMenuBtn); break;
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    // Stop any in-flight search and wait for it here: `app`'s members are
    // destroyed in reverse order, so its transposition tables would
    // otherwise be freed while that search thread is still using them.
    app.aiStop = true;
    app.aiFuture.reset();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
