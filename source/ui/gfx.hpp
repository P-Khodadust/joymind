// Drawing primitives. Rounded shapes use a pre-rendered, antialiased circle
// texture (quadrants for corners), so edges stay smooth on the GPU renderer.
#pragma once
#include <SDL.h>

class Gfx {
public:
    bool init(SDL_Renderer* r);
    void shutdown();
    SDL_Renderer* renderer() const { return r_; }

    void fill(const SDL_Rect& rc, SDL_Color c);
    void round(const SDL_Rect& rc, int radius, SDL_Color c);
    // Rounded rect with a border of `t` pixels (drawn as two nested fills).
    void roundBorder(const SDL_Rect& rc, int radius, int t, SDL_Color border, SDL_Color fill);
    void circle(int cx, int cy, int radius, SDL_Color c);
    // Original starburst logo (not an Anthropic asset), drawn from a texture
    // generated in code at startup.
    void sparkle(int cx, int cy, int size, SDL_Color c, double angle = 0);
    // Vertical gradient (used to fade text out under the top bar and composer).
    void vgradient(const SDL_Rect& r, SDL_Color top, SDL_Color bottom);

private:
    SDL_Texture* makeTexture(int size, bool sparkle);
    SDL_Renderer* r_ = nullptr;
    SDL_Texture* circle_ = nullptr;
    SDL_Texture* sparkle_ = nullptr;
};
