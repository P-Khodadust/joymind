#include "ui/gfx.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr int kTex = 256;  // circle / sparkle texture size
constexpr int kHalf = kTex / 2;

// Sparkle: 12 tapered rays of alternating length around a small disc.
struct Rays {
    float dx[12], dy[12];
    Rays() {
        for (int i = 0; i < 12; i++) {
            float a = 6.2831853f * i / 12.0f - 1.5707963f;
            dx[i] = std::cos(a);
            dy[i] = std::sin(a);
        }
    }
};

bool insideSparkle(float px, float py) {
    static const Rays rays;
    const float R = kHalf - 2.0f;
    if (px * px + py * py < R * R * 0.0256f) return true;  // centre disc, radius 0.16 R
    for (int i = 0; i < 12; i++) {
        float dx = rays.dx[i], dy = rays.dy[i];
        float len = (i % 2 == 0) ? R : R * 0.62f;
        float t = px * dx + py * dy;             // distance along the ray
        float perp = std::fabs(px * dy - py * dx);  // distance from the ray axis
        if (t >= 0 && t <= len && perp <= R * 0.085f * (1.0f - t / len)) return true;
    }
    return false;
}
}  // namespace

// Coverage-based antialiasing: 4x4 samples per pixel, white with alpha.
SDL_Texture* Gfx::makeTexture(int size, bool sparkle) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!s) return nullptr;
    auto* px = static_cast<Uint32*>(s->pixels);
    const float h = size / 2.0f, r2 = h * h;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float fx = x + (sx + 0.5f) / 4.0f - h, fy = y + (sy + 0.5f) / 4.0f - h;
                    hits += sparkle ? insideSparkle(fx, fy) : (fx * fx + fy * fy <= r2);
                }
            Uint8 a = (Uint8)(hits * 255 / 16);
            px[y * (s->pitch / 4) + x] = SDL_MapRGBA(s->format, 255, 255, 255, a);
        }
    }
    SDL_Texture* t = SDL_CreateTextureFromSurface(r_, s);
    SDL_FreeSurface(s);
    if (t) {
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    }
    return t;
}

bool Gfx::init(SDL_Renderer* r) {
    r_ = r;
    SDL_SetRenderDrawBlendMode(r_, SDL_BLENDMODE_BLEND);
    circle_ = makeTexture(kTex, false);
    sparkle_ = makeTexture(kTex, true);
    return circle_ && sparkle_;
}

void Gfx::shutdown() {
    if (circle_) SDL_DestroyTexture(circle_);
    if (sparkle_) SDL_DestroyTexture(sparkle_);
    circle_ = sparkle_ = nullptr;
}

void Gfx::fill(const SDL_Rect& rc, SDL_Color c) {
    if (rc.w <= 0 || rc.h <= 0) return;
    SDL_SetRenderDrawColor(r_, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(r_, &rc);
}

void Gfx::round(const SDL_Rect& rc, int rad, SDL_Color c) {
    rad = std::min({rad, rc.w / 2, rc.h / 2});
    if (rad <= 0) return fill(rc, c);
    fill({rc.x + rad, rc.y, rc.w - 2 * rad, rc.h}, c);
    fill({rc.x, rc.y + rad, rad, rc.h - 2 * rad}, c);
    fill({rc.x + rc.w - rad, rc.y + rad, rad, rc.h - 2 * rad}, c);
    SDL_SetTextureColorMod(circle_, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(circle_, c.a);
    const SDL_Rect src[4] = {{0, 0, kHalf, kHalf}, {kHalf, 0, kHalf, kHalf}, {0, kHalf, kHalf, kHalf},
                             {kHalf, kHalf, kHalf, kHalf}};
    const SDL_Rect dst[4] = {{rc.x, rc.y, rad, rad},
                             {rc.x + rc.w - rad, rc.y, rad, rad},
                             {rc.x, rc.y + rc.h - rad, rad, rad},
                             {rc.x + rc.w - rad, rc.y + rc.h - rad, rad, rad}};
    for (int i = 0; i < 4; i++) SDL_RenderCopy(r_, circle_, &src[i], &dst[i]);
}

void Gfx::roundBorder(const SDL_Rect& rc, int rad, int t, SDL_Color border, SDL_Color fillc) {
    round(rc, rad, border);
    round({rc.x + t, rc.y + t, rc.w - 2 * t, rc.h - 2 * t}, std::max(0, rad - t), fillc);
}

void Gfx::circle(int cx, int cy, int radius, SDL_Color c) {
    round({cx - radius, cy - radius, radius * 2, radius * 2}, radius, c);
}

void Gfx::sparkle(int cx, int cy, int size, SDL_Color c, double angle) {
    SDL_SetTextureColorMod(sparkle_, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(sparkle_, c.a);
    SDL_Rect dst{cx - size / 2, cy - size / 2, size, size};
    SDL_RenderCopyEx(r_, sparkle_, nullptr, &dst, angle, nullptr, SDL_FLIP_NONE);
}

void Gfx::vgradient(const SDL_Rect& r, SDL_Color top, SDL_Color bottom) {
    float x0 = (float)r.x, x1 = (float)(r.x + r.w), y0 = (float)r.y, y1 = (float)(r.y + r.h);
    SDL_Vertex v[4] = {{{x0, y0}, top, {0, 0}}, {{x1, y0}, top, {0, 0}}, {{x1, y1}, bottom, {0, 0}}, {{x0, y1}, bottom, {0, 0}}};
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(r_, nullptr, v, 4, idx, 6);
}
