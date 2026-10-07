#include "ui/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

void Ui::begin(const Input& in, float scale) {
    in_ = in;
    scale_ = scale;
    uint32_t now = SDL_GetTicks();

    // Auto-repeat held directions so lists can be scrolled by holding the D-pad.
    uint32_t dirs = in.held & kDirs;
    if (dirs && dirs == repeatDirs_) {
        if (SDL_TICKS_PASSED(now, repeatAt_)) {
            in_.down |= dirs;
            repeatAt_ = now + 85;
        }
    } else {
        repeatDirs_ = dirs;
        repeatAt_ = now + 380;
    }
    if (in_.down && !showFocus_) {
        showFocus_ = true;
        focusAt_ = now;  // the ring reappears with its grow animation
    }

    topLayer_ = 0;
    for (const auto& it : prev_) topLayer_ = std::max(topLayer_, it.layer);
    moved_ = aUsed_ = false;
    unused_ = 0;
    for (uint32_t d : {BtnUp, BtnDown, BtnLeft, BtnRight})
        if ((in_.down & d) && !move(d)) unused_ |= d;

    began_ = released_ = tapped_ = false;
    dragDy_ = 0;
    if (in.touch && !prevTouch_) {
        began_ = true;
        dragging_ = false;
        startX_ = in.tx;
        startY_ = lastY_ = in.ty;
        vel_ = 0;
        showFocus_ = false;
    } else if (in.touch && prevTouch_) {
        int slop = (int)(12 * scale_);
        if (!dragging_ && (std::abs(in.tx - startX_) > slop || std::abs(in.ty - startY_) > slop)) dragging_ = true;
        if (dragging_) {
            dragDy_ = in.ty - lastY_;
            vel_ = 0.6f * vel_ + 0.4f * dragDy_;  // smoothed px/frame for the fling
        }
        lastY_ = in.ty;
    } else if (!in.touch && prevTouch_) {
        released_ = true;
        tapped_ = !dragging_;
        fling_ = dragging_ ? vel_ : 0.f;
        dragging_ = false;
    }
    prevTouch_ = in.touch;
    cur_.clear();
    layer = 0;
    clip = {0, 0, 1 << 20, 1 << 20};
}

void Ui::end() {
    int top = 0;
    for (const auto& it : cur_) top = std::min(std::max(top, it.layer), kMaxLayers - 1);
    // Opening a modal remembers the focus underneath; closing it restores that.
    if (top > prevTop_) savedFocus_[prevTop_] = focus_;
    if (top < prevTop_) focus_ = savedFocus_[top];
    prevTop_ = top;
    auto present = [&](uint32_t id) {
        for (const auto& it : cur_)
            if (it.id == id && it.layer == top) return true;
        return false;
    };
    if (!present(focus_)) {
        if (preferred_ && present(preferred_))
            focus_ = preferred_;
        else
            for (const auto& it : cur_)
                if (it.layer == top) {
                    focus_ = it.id;
                    break;
                }
    }
    if (focus_ != lastFocus_) {
        lastFocus_ = focus_;
        focusAt_ = SDL_GetTicks();
    }
    prev_.swap(cur_);
    preferred_ = 0;
}

bool Ui::item(uint32_t id, const SDL_Rect& r) {
    if (layer < topLayer_) return false;
    cur_.push_back({id, r, layer});
    bool visible = inRect(clip, startX_, startY_) && inRect(r, startX_, startY_);
    if (tapped_ && visible) {
        tapped_ = false;  // consumed
        focus_ = id;
        return true;
    }
    if (focus_ == id && (in_.down & BtnA) && !aUsed_) {
        aUsed_ = true;
        return true;
    }
    return false;
}

bool Ui::tapArea(const SDL_Rect& r) {
    if (layer < topLayer_ || !tapped_ || !inRect(r, startX_, startY_)) return false;
    tapped_ = false;
    return true;
}

// Spatial navigation: pick the nearest item in the pressed direction, preferring
// items that overlap the current one on the other axis.
bool Ui::move(uint32_t dir) {
    const Item* cur = nullptr;
    for (const auto& it : prev_)
        if (it.id == focus_ && it.layer == topLayer_) cur = &it;
    if (!cur) {
        for (const auto& it : prev_)
            if (it.layer == topLayer_) {
                focus_ = it.id;
                moved_ = true;
                return true;
            }
        return false;
    }
    const SDL_Rect a = cur->r;
    const Item* best = nullptr;
    float bestScore = 1e30f;
    for (const auto& it : prev_) {
        if (it.layer != topLayer_ || it.id == focus_) continue;
        const SDL_Rect b = it.r;
        float primary, gap, center;
        if (dir == BtnDown || dir == BtnUp) {
            primary = dir == BtnDown ? b.y - (a.y + a.h) : a.y - (b.y + b.h);
            if ((dir == BtnDown ? b.y + b.h / 2 - (a.y + a.h / 2) : a.y + a.h / 2 - (b.y + b.h / 2)) <= 0) continue;
            gap = (float)std::max(0, std::max(a.x, b.x) - std::min(a.x + a.w, b.x + b.w));
            center = std::fabs((b.x + b.w / 2.f) - (a.x + a.w / 2.f));
        } else {
            primary = dir == BtnRight ? b.x - (a.x + a.w) : a.x - (b.x + b.w);
            if ((dir == BtnRight ? b.x + b.w / 2 - (a.x + a.w / 2) : a.x + a.w / 2 - (b.x + b.w / 2)) <= 0) continue;
            gap = (float)std::max(0, std::max(a.y, b.y) - std::min(a.y + a.h, b.y + b.h));
            center = std::fabs((b.y + b.h / 2.f) - (a.y + a.h / 2.f));
        }
        float score = std::max(0.f, primary) + gap * 4 + center * 0.2f;
        if (score < bestScore) {
            bestScore = score;
            best = &it;
        }
    }
    if (!best) return false;
    focus_ = best->id;
    moved_ = true;
    return true;
}

void Scroller::scrollTo(float y) {
    goal = std::max(0.f, std::min(y, maxPos()));
    easing = goal != pos;
}

void Scroller::update(Ui& ui, const SDL_Rect& area, float scale, bool controller) {
    if (ui.active() && ui.touchBegan() && inRect(area, ui.touchStartX(), ui.touchStartY())) {
        owning = true;
        vel = 0;
        easing = false;
    }
    if (owning) {
        if (ui.dragging()) pos -= ui.dragDy();
        if (ui.released()) {
            vel = -ui.flingVelocity();
            owning = false;
        }
    } else if (vel != 0) {
        pos += vel;  // inertia after a fling
        vel *= 0.95f;
        if (std::fabs(vel) < 0.3f) vel = 0;
    }
    if (controller && ui.active()) {
        if (ui.input().scroll != 0) {
            pos += ui.input().scroll * 22 * scale;
            easing = false;
        }
        if (ui.pressed(BtnR)) scrollTo(target() + view * 0.85f);
        if (ui.pressed(BtnL)) scrollTo(target() - view * 0.85f);
    }
    if (easing && !owning) {
        pos += (goal - pos) * 0.22f;  // ~150 ms ease-out at 60 fps
        if (std::fabs(goal - pos) < 0.5f) {
            pos = goal;
            easing = false;
        }
    }
    clamp();
}

void Scroller::ensureVisible(int top, int h, int margin) {
    float t = target();
    if (top - margin < t) t = (float)(top - margin);
    if (top + h + margin > t + view) t = (float)(top + h + margin - view);
    scrollTo(t);
}

void Scroller::clamp() {
    pos = std::max(0.f, std::min(pos, maxPos()));
    goal = std::max(0.f, std::min(goal, maxPos()));
    if (pos == 0 || pos == maxPos()) vel = owning ? vel : 0;
}
