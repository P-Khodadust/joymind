// Immediate-mode widget core: every frame, screens call item() for each
// focusable/tappable rect. D-pad/stick navigation moves focus spatially using
// the rects registered in the previous frame; taps hit-test the current frame.
#pragma once
#include <SDL.h>

#include <cstdint>
#include <string>
#include <vector>

enum Button : uint32_t {
    BtnA = 1u << 0,
    BtnB = 1u << 1,
    BtnX = 1u << 2,
    BtnY = 1u << 3,
    BtnL = 1u << 4,
    BtnR = 1u << 5,
    BtnZL = 1u << 6,
    BtnZR = 1u << 7,
    BtnPlus = 1u << 8,
    BtnMinus = 1u << 9,
    BtnUp = 1u << 10,
    BtnDown = 1u << 11,
    BtnLeft = 1u << 12,
    BtnRight = 1u << 13,
};
constexpr uint32_t kDirs = BtnUp | BtnDown | BtnLeft | BtnRight;

struct Input {
    uint32_t down = 0, held = 0;  // buttons pressed this frame / currently held
    // physical (USB) keyboard typing this frame
    std::string text;
    int backspaces = 0;
    bool enterKey = false, shift = false;
    float scroll = 0;             // right stick or mouse wheel; positive scrolls down
    bool touch = false;           // a finger (or the left mouse button) is down
    int tx = 0, ty = 0;           // its position in pixels
};

inline bool inRect(const SDL_Rect& r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

class Ui {
public:
    void begin(const Input& in, float scale);
    void end();

    // Focusable + tappable target. Returns true when activated (A or tap).
    bool item(uint32_t id, const SDL_Rect& r);
    // Tappable background (not focusable), e.g. a scrim that closes a panel.
    bool tapArea(const SDL_Rect& r);

    bool showFocus(uint32_t id) const { return showFocus_ && focus_ == id; }
    bool hasFocus(uint32_t id) const { return focus_ == id; }
    uint32_t focusAge() const { return SDL_GetTicks() - focusAt_; }  // ms since focus moved (animations)
    bool justFocused(uint32_t id) const { return moved_ && focus_ == id; }
    void setFocus(uint32_t id) { focus_ = id; }
    void preferFocus(uint32_t id) { preferred_ = id; }  // fallback when focus is lost
    bool pressed(uint32_t btn) const { return (in_.down & btn) != 0; }
    uint32_t unusedDirs() const { return unused_; }  // directions that had nowhere to go
    const Input& input() const { return in_; }
    bool active() const { return layer >= topLayer_; }

    // Touch gesture state for scrollers.
    bool touchBegan() const { return began_; }
    int touchStartX() const { return startX_; }
    int touchStartY() const { return startY_; }
    bool dragging() const { return dragging_; }
    int dragDy() const { return dragDy_; }
    bool released() const { return released_; }
    float flingVelocity() const { return fling_; }

    int layer = 0;  // modal depth; items below the top layer are inert
    SDL_Rect clip{0, 0, 1 << 20, 1 << 20};

private:
    struct Item {
        uint32_t id;
        SDL_Rect r;
        int layer;
    };
    bool move(uint32_t dir);

    Input in_;
    std::vector<Item> prev_, cur_;
    uint32_t focus_ = 0, preferred_ = 0, unused_ = 0, lastFocus_ = 0, focusAt_ = 0;
    bool moved_ = false, showFocus_ = true, aUsed_ = false;
    static constexpr int kMaxLayers = 4;
    int topLayer_ = 0, prevTop_ = 0;
    uint32_t savedFocus_[kMaxLayers] = {};  // focus per layer, restored when a modal closes
    uint32_t repeatDirs_ = 0, repeatAt_ = 0;
    bool prevTouch_ = false, began_ = false, dragging_ = false, released_ = false, tapped_ = false;
    int startX_ = 0, startY_ = 0, lastY_ = 0, dragDy_ = 0;
    float vel_ = 0, fling_ = 0, scale_ = 1;
};

// Vertical scroll state with touch drag + inertia, right stick and L/R paging.
// Jumps (paging, focus, following new text) ease toward `goal` instead of snapping.
struct Scroller {
    float pos = 0, vel = 0, goal = 0;
    bool easing = false;
    int content = 0, view = 0;
    bool owning = false;  // this scroller owns the current touch drag
    float maxPos() const { return content > view ? (float)(content - view) : 0.f; }
    float target() const { return easing ? goal : pos; }
    void scrollTo(float y);
    void update(Ui& ui, const SDL_Rect& area, float scale, bool controller);
    void ensureVisible(int top, int h, int margin);
    void clamp();
};
