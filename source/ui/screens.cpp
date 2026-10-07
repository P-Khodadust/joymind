// Screens and dialogs. Everything is immediate mode: compute rects, register
// them with ui_.item() (focus + taps), draw, then act on the result.
#include <algorithm>
#include <cstdlib>
#include <ctime>

#include "app/app.hpp"
#include "app/platform.hpp"

using namespace metrics;

namespace {
SDL_Rect grow(const SDL_Rect& r, int d) { return {r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d}; }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Only the last 4 characters of a key are ever shown (and none for short keys).
std::string maskKey(const std::string& k) {
    if (k.empty()) return "not set";
    const char* dots = "••••";
    return k.size() < 12 ? dots : dots + k.substr(k.size() - 4);
}

std::string headerNames(const Headers& h) {
    std::string s;
    for (const auto& kv : h) s += (s.empty() ? "" : ", ") + kv.first;
    return s.empty() ? "none" : s;
}

// "Name: value; Other: value"
Headers parseHeaders(const std::string& s) {
    Headers h;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(';', start);
        std::string part = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t colon = part.find(':');
        if (colon != std::string::npos && !trim(part.substr(0, colon)).empty())
            h.push_back({trim(part.substr(0, colon)), trim(part.substr(colon + 1))});
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return h;
}

std::string themeLabel(const std::string& t) { return t == "light" ? "Light" : t == "dark" ? "Dark" : "Auto (system)"; }
const char* kFontSizeNames[3] = {"Small", "Medium", "Large"};
}  // namespace

// ------------------------------------------------------------------ helpers

std::string App::fit(unsigned char font, const std::string& s, int maxW) {
    std::string line = s.substr(0, s.find('\n'));
    if (text_.measure(font, line) <= maxW) return line;
    const std::string ell = "…";
    size_t lo = 0, hi = std::min(line.size(), (size_t)400);
    while (lo < hi) {  // binary search on the byte length, kept on UTF-8 boundaries
        size_t mid = (lo + hi + 1) / 2;
        while (mid > 0 && ((unsigned char)line[mid] & 0xC0) == 0x80) mid--;
        if (mid <= lo) break;
        if (text_.measure(font, line.substr(0, mid) + ell) <= maxW)
            lo = mid;
        else
            hi = mid - 1;
    }
    while (lo > 0 && ((unsigned char)line[lo] & 0xC0) == 0x80) lo--;
    return line.substr(0, lo) + ell;
}

void App::focusRing(const SDL_Rect& r, int radius, ColorRole gap) {
    float p = std::min(1.f, ui_.focusAge() / 140.f);
    int t = std::max(1, (int)(px(FocusRing) * (1.f - (1.f - p) * (1.f - p))));
    if (gap == C_COUNT) return gfx_.round(grow(r, t), radius + t, col(C_Accent));
    gfx_.round(grow(r, 2 * t), radius + 2 * t, col(C_Accent));
    gfx_.round(grow(r, t), radius + t, col(gap));
}

void App::icon(const char* glyph, const SDL_Rect& r, SDL_Color c) {
    int w = text_.measure(F_Icon, glyph);
    text_.draw(F_Icon, glyph, r.x + (r.w - w) / 2, r.y + (r.h - text_.lineHeight(F_Icon)) / 2, c);
}

void App::badge(int x, int y, const std::string& t, ColorRole fg, ColorRole bg) {
    int w = text_.measure(F_SmallBold, t) + px(14);
    gfx_.round({x, y, w, text_.lineHeight(F_SmallBold) + px(4)}, px(8), col(bg));
    text_.draw(F_SmallBold, t, x + px(7), y + px(2), col(fg));
}

bool App::button(uint32_t id, const SDL_Rect& r, const std::string& label, bool primary, bool danger) {
    bool act = ui_.item(id, r);
    int rad = r.h / 2;
    if (ui_.showFocus(id)) focusRing(r, rad, primary ? C_Bg : C_COUNT);
    if (primary)
        gfx_.round(r, rad, col(C_Accent));
    else
        gfx_.roundBorder(r, rad, std::max(1, px(1)), col(C_Border), col(ui_.showFocus(id) ? C_Hover : C_Surface));
    ColorRole fg = primary ? C_OnAccent : danger ? C_Error : C_Text;
    std::string l = fit(F_SmallBold, label, r.w - px(16));
    int w = text_.measure(F_SmallBold, l);
    text_.draw(F_SmallBold, l, r.x + (r.w - w) / 2, r.y + (r.h - text_.lineHeight(F_SmallBold)) / 2, col(fg));
    return act;
}

// Settings-style row: label on the left, value on the right.
bool App::row(uint32_t id, int x, int y, int w, const std::string& label, const std::string& value, Scroller* sc,
              int scrollTop) {
    SDL_Rect r{x, y, w, px(RowH)};
    bool act = ui_.item(id, r);
    if (sc && ui_.justFocused(id)) sc->ensureVisible(y - scrollTop, r.h, px(12));
    if (ui_.showFocus(id)) {
        focusRing(r, px(12));
        gfx_.round(r, px(12), col(C_Hover));
    }
    int ty = y + (r.h - text_.lineHeight(F_Body)) / 2;
    int lw = text_.draw(F_Body, fit(F_Body, label, w / 2 - px(20)), x + px(16), ty, col(C_Text));
    std::string v = fit(F_Body, value, w - lw - px(48));
    text_.draw(F_Body, v, x + w - px(16) - text_.measure(F_Body, v), ty, col(C_Muted));
    return act;
}

// Centered dialog panel over a scrim. Returns the content rect.
SDL_Rect App::dialog(const std::string& title, int contentH, int width) {
    float p = ease(modalAt_, 200);
    alpha_ = p;  // the whole dialog fades in (frame() resets alpha_)
    gfx_.fill({0, 0, W, H}, col(C_Scrim));
    int w = std::min(px(width), W - 2 * px(Gutter));
    int h = std::min(H - 2 * px(40), contentH + px(76));
    SDL_Rect panel{(W - w) / 2, (H - h) / 2 + (int)((1.f - p) * px(18)), w, h};
    if (ui_.tapArea({0, 0, W, panel.y}) || ui_.tapArea({0, panel.y + h, W, H}) || ui_.tapArea({0, 0, panel.x, H}) ||
        ui_.tapArea({panel.x + w, 0, W, H}))
        modal_ = Modal::None;
    gfx_.roundBorder(panel, px(Radius), std::max(1, px(1)), col(C_Border), col(C_Surface));
    text_.draw(F_H3, title, panel.x + px(24), panel.y + px(20), col(C_Text));
    return {panel.x + px(12), panel.y + px(64), w - px(24), h - px(76)};
}

// ------------------------------------------------------------------ main screen

void App::drawMain() {
    bool empty = chat_.messages.empty();
    drawTopBar(empty ? "" : chat_.title, false);
    if (empty) {
        if (!homeAt_) homeAt_ = ticks_;
        drawHome();
    } else {
        homeAt_ = 0;
        drawChat(px(TopBarH));
    }
}

void App::drawTopBar(const std::string& title, bool back) {
    SDL_Rect lb{px(14), px(10), px(40), px(40)};
    uint32_t lid = back ? ID_BACK : ID_MENU;
    if (ui_.item(lid, lb)) {
        if (back)
            screen_ = screen_ == Screen::Settings ? Screen::Main : Screen::Settings;
        else {
            modal_ = Modal::Sidebar;
            listScroll_ = Scroller();
        }
    }
    auto hover = [&](uint32_t id, const SDL_Rect& r) {
        if (ui_.showFocus(id)) {
            focusRing(r, r.w / 2);
            gfx_.circle(r.x + r.w / 2, r.y + r.h / 2, r.w / 2, col(C_Hover));
        }
    };
    hover(lid, lb);
    if (back) {
        icon("←", lb, col(C_Text));
    } else {
        for (int i = -1; i <= 1; i++)  // hamburger
            gfx_.round({lb.x + px(11), lb.y + lb.h / 2 + i * px(6) - px(1), px(18), std::max(2, px(2))}, px(1),
                       col(C_Text));
    }
    if (!title.empty()) {
        std::string t = fit(F_SmallBold, title, W - 2 * px(150));
        text_.draw(F_SmallBold, t, (W - text_.measure(F_SmallBold, t)) / 2,
                   px(TopBarH) / 2 - text_.lineHeight(F_SmallBold) / 2, col(C_Text));
    }
    if (back) return;

    SDL_Rect nb{W - px(104), px(10), px(40), px(40)}, sb{W - px(56), px(10), px(40), px(40)};
    if (ui_.item(ID_NEW, nb)) newChat();
    if (ui_.item(ID_SETTINGS, sb)) screen_ = Screen::Settings;
    hover(ID_NEW, nb);
    hover(ID_SETTINGS, sb);
    int t = std::max(2, px(2));
    gfx_.round({nb.x + px(11), nb.y + nb.h / 2 - t / 2, px(18), t}, px(1), col(C_Text));
    gfx_.round({nb.x + nb.w / 2 - t / 2, nb.y + px(11), t, px(18)}, px(1), col(C_Text));
    for (int i = -1; i <= 1; i++) gfx_.circle(sb.x + sb.w / 2 + i * px(7), sb.y + sb.h / 2, px(2), col(C_Text));
}

void App::drawHome() {
    int h = platform::localHour();
    std::string g = h >= 5 && h < 12 ? "Good morning" : h >= 12 && h < 18 ? "Good afternoon" : "Good evening";
    if (!cfg_.userName.empty()) g += ", " + cfg_.userName;
    g = fit(F_Greeting, g, W - px(160));
    int sparkS = px(44), gap = px(14);
    int gw = text_.measure(F_Greeting, g);
    float p = ease(homeAt_, 500);
    alpha_ = p;
    int gx = (W - (sparkS + gap + gw)) / 2, gy = (int)(H * 0.33f) + (int)((1.f - p) * px(14));
    gfx_.sparkle(gx + sparkS / 2, gy, sparkS, col(C_Accent));
    text_.draw(F_Greeting, g, gx + sparkS + gap, gy - text_.lineHeight(F_Greeting) / 2, col(C_Text));

    int cw = std::min(px(ComposerW), W - 2 * px(Gutter));
    SDL_Rect box{(W - cw) / 2, gy + px(56), cw, px(136) + (draftImages_.empty() ? 0 : px(66))};
    drawComposer(box, true);

    if (cfg_.profiles.empty()) {
        std::string s = "No provider yet: press ⊕ to open Settings and add one.";
        text_.draw(F_Small, s, (W - text_.measure(F_Small, s)) / 2, box.y + box.h + px(18), col(C_Muted));
    }
    std::string hint =
        "Ⓨ Type    Ⓐ Select    Ⓧ New chat    ⊖ Chats    ⊕ Settings";
    text_.draw(F_Small, hint, (W - text_.measure(F_Small, hint)) / 2, H - px(44), col(C_Muted));
    alpha_ = 1.f;
}

void App::drawChat(int top) {
    int colW = std::min(px(ColumnW), W - 2 * px(Gutter) - px(48));
    int colX = (W - colW) / 2 + px(16);  // leave room for the sparkle on the left
    int cw = std::min(px(ComposerW), W - 2 * px(Gutter));
    int pad = px(16), lh = text_.lineHeight(F_Body);
    int lines = 1;
    if (!draft_.empty()) {
        if (draft_ != draftKey_ || draftW_ != cw - 2 * pad) {
            draftLayout_ = text_.layout({{draft_, F_Body, C_Text}}, cw - 2 * pad);
            draftKey_ = draft_;
            draftW_ = cw - 2 * pad;
        }
        lines = std::min(4, (int)draftLayout_.lines.size());
    }
    int boxH = pad + lines * lh + px(10) + px(36) + px(12) + (draftImages_.empty() ? 0 : px(66));
    SDL_Rect box{(W - cw) / 2, H - boxH - px(14), cw, boxH};
    SDL_Rect area{0, top, W, box.y - px(6) - top};

    // Lay out every message (cached) to know the total height.
    size_t known = views_.size();
    views_.resize(chat_.messages.size());
    for (size_t i = known; i < views_.size(); i++) views_[i].born = skipMsgAnim_ ? 0 : ticks_;
    skipMsgAnim_ = false;
    const int gap = px(24);
    int content = px(16);
    for (size_t i = 0; i < chat_.messages.size(); i++) {
        layoutMessage(chat_.messages[i], views_[i], colW);
        layoutCalls(i, views_[i], colW);
        int h = msgHeight(views_[i]);
        content += h ? h + gap : 0;  // tool results live inside their command card
    }
    chatScroll_.content = content;
    chatScroll_.view = area.h;
    // Glide along with new tokens unless the user scrolled up to read.
    if (followBottom_) chatScroll_.scrollTo(chatScroll_.maxPos());
    if (modal_ == Modal::None) {
        if (ui_.unusedDirs() & BtnUp) chatScroll_.scrollTo(chatScroll_.target() - px(110));
        if (ui_.unusedDirs() & BtnDown) chatScroll_.scrollTo(chatScroll_.target() + px(110));
    }
    chatScroll_.update(ui_, area, S, true);
    followBottom_ = chatScroll_.target() >= chatScroll_.maxPos() - px(4);

    SDL_RenderSetClipRect(ren_, &area);
    int y = area.y + px(16) - (int)chatScroll_.pos;
    int toolAction = 0;
    ui_.clip = area;
    for (size_t i = 0; i < chat_.messages.size(); i++) {
        MsgView& v = views_[i];
        int h = msgHeight(v);
        if (!h) continue;
        float p = ease(v.born, 300);  // new messages rise and fade in
        int my = y + (int)((1.f - p) * px(16));
        alpha_ = p;
        bool visible = my + h >= area.y && my <= area.y + area.h;
        if (visible)
            drawMessage(chat_.messages[i], v, colX, my, colW, generating_ && i + 1 == chat_.messages.size(), area.y,
                        area.y + area.h);
        // cards are always registered so their buttons stay reachable by D-pad
        if (v.callsH) {
            int a = drawCalls(v, colX, my + v.textH + (v.textH ? px(12) : 0), colW);
            toolAction = a ? a : toolAction;
        }
        alpha_ = 1.f;
        y += h + gap;
    }
    // Soft edges: text fades out under the top bar and above the composer.
    SDL_Color solid = col(C_Bg), clear = solid;
    clear.a = 0;
    gfx_.vgradient({0, area.y, W, px(18)}, solid, clear);
    gfx_.vgradient({0, area.y + area.h - px(26), W, px(26)}, clear, solid);
    SDL_RenderSetClipRect(ren_, nullptr);
    ui_.clip = {0, 0, W, H};
    drawComposer(box, false);

    const ToolCall* pc = toolAction ? findCall(pendingCallId_) : nullptr;
    if (pc) {
        ToolCall c = *pc;
        if (toolAction == 2) {
            pendingCallId_.clear();
            addToolResult(c, "The user declined to run this command.", -1, true);
            nextCall();
        } else {
            autoApprove_ = autoApprove_ || toolAction == 3;
            runCall(c);
        }
    }
}

void App::drawComposer(const SDL_Rect& box, bool home) {
    int pad = px(16), rad = px(Radius);
    SDL_Rect sendR{box.x + box.w - px(12) - px(36), box.y + box.h - px(12) - px(36), px(36), px(36)};
    Profile* prof = chatProfile();
    std::string model = chatModel();
    std::string chipText = prof ? prof->name + "  ·  " + (model.empty() ? "Choose model" : model) : "No provider";
    chipText = fit(F_Small, chipText, box.w / 2);
    SDL_Rect attachR{box.x + px(10), sendR.y + px(2), px(32), px(32)};
    SDL_Rect chip{attachR.x + attachR.w + px(6), sendR.y + px(3), text_.measure(F_Small, chipText) + px(36), px(30)};
    bool insecure = prof && !prof->baseUrl.empty() && net::checkUrl(prof->baseUrl) == net::UrlCheck::Insecure;
    int mx = chip.x + chip.w + px(8) + (insecure ? text_.measure(F_SmallBold, "HTTP") + px(22) : 0);
    net::Machine* mc = chatMachine();
    std::string mText = fit(F_Small, mc ? "SSH  ·  " + mc->name : "No SSH", std::max(px(60), sendR.x - mx - px(46)));
    SDL_Rect mchip{mx, chip.y, text_.measure(F_Small, mText) + px(36), px(30)};

    bool actSend = ui_.item(ID_SEND, sendR);
    bool actAttach = ui_.item(ID_ATTACH, attachR);
    int removeShot = -1;
    const int stripH = draftImages_.empty() ? 0 : px(66), tw = px(96), th = px(54);
    for (size_t i = 0; i < draftImages_.size(); i++)  // attached screenshots: A removes one
        if (ui_.item(ID_ATTACHED + (uint32_t)i, {box.x + pad + (int)i * (tw + px(8)), box.y + px(12), tw, th}))
            removeShot = (int)i;
    bool actChip = ui_.item(ID_CHIP, chip);
    bool actMachine = ui_.item(ID_MACHINE_CHIP, mchip);
    bool actBox = ui_.item(ID_COMPOSER, box);
    ui_.preferFocus(ID_COMPOSER);

    if (ui_.showFocus(ID_COMPOSER)) focusRing(box, rad);
    gfx_.roundBorder(box, rad, std::max(1, px(1)), col(C_Border), col(C_Surface));

    for (size_t i = 0; i < draftImages_.size(); i++) {
        SDL_Rect r{box.x + pad + (int)i * (tw + px(8)), box.y + px(12), tw, th};
        if (ui_.showFocus(ID_ATTACHED + (uint32_t)i)) focusRing(r, px(6));
        if (SDL_Texture* t = thumb(draftImages_[i])) {
            SDL_SetTextureAlphaMod(t, (Uint8)(255 * alpha_));
            SDL_RenderCopy(ren_, t, nullptr, &r);
        } else {
            gfx_.round(r, px(6), col(C_CodeBg));
        }
        gfx_.circle(r.x + r.w - px(9), r.y + px(9), px(8), col(C_Text));  // remove badge
        icon("×", {r.x + r.w - px(17), r.y + px(1), px(16), px(16)}, col(C_Bg));
    }

    // Draft text (last lines) or placeholder.
    int lh = text_.lineHeight(F_Body);
    SDL_Rect ta{box.x + pad, box.y + pad - px(2) + stripH, box.w - 2 * pad, chip.y - box.y - pad - stripH};
    int caretX = ta.x, caretY = ta.y;
    if (draft_.empty()) {
        text_.draw(F_Body, home ? "How can I help you today?" : "Reply…", ta.x + (keyboardMode_ ? px(4) : 0), ta.y,
                   col(C_Muted));
    } else {
        if (draft_ != draftKey_ || draftW_ != ta.w) {
            draftLayout_ = text_.layout({{draft_, F_Body, C_Text}}, ta.w);
            draftKey_ = draft_;
            draftW_ = ta.w;
        }
        int shown = std::max(1, ta.h / lh);
        int skip = std::max(0, (int)draftLayout_.lines.size() - shown);
        SDL_RenderSetClipRect(ren_, &ta);
        text_.draw(draftLayout_, ta.x, ta.y - skip * lh, pc(), ta.y, ta.y + ta.h);
        SDL_RenderSetClipRect(ren_, nullptr);
        const TextLine& last = draftLayout_.lines.back();
        caretY = ta.y - skip * lh + last.y;
        caretX = ta.x + (last.segs.empty() ? 0 : last.segs.back().x + last.segs.back().w);
        if (draft_.back() == '\n') caretX = ta.x, caretY += lh;
    }
    if (keyboardMode_ && ticks_ / 530 % 2 == 0)  // blinking caret while typing on a USB keyboard
        gfx_.fill({caretX, std::min(caretY, ta.y + ta.h - lh), std::max(1, px(2)), lh}, col(C_Accent));

    // Attach-screenshot button.
    if (ui_.showFocus(ID_ATTACH)) focusRing(attachR, attachR.w / 2);
    gfx_.roundBorder(attachR, attachR.w / 2, std::max(1, px(1)), col(C_Border),
                     col(ui_.showFocus(ID_ATTACH) ? C_Hover : C_Surface));
    int t2 = std::max(2, px(2));
    gfx_.fill({attachR.x + attachR.w / 2 - px(6), attachR.y + attachR.h / 2 - t2 / 2, px(12), t2}, col(C_Muted));
    gfx_.fill({attachR.x + attachR.w / 2 - t2 / 2, attachR.y + attachR.h / 2 - px(6), t2, px(12)}, col(C_Muted));

    // Provider / model chip.
    if (ui_.showFocus(ID_CHIP)) focusRing(chip, chip.h / 2);
    if (ui_.showFocus(ID_CHIP)) gfx_.round(chip, chip.h / 2, col(C_Hover));
    text_.draw(F_Small, chipText, chip.x + px(12), chip.y + (chip.h - text_.lineHeight(F_Small)) / 2, col(C_Muted));
    text_.draw(F_Small, "⌄", chip.x + chip.w - px(20), chip.y + (chip.h - text_.lineHeight(F_Small)) / 2 - px(3),
               col(C_Muted));
    if (insecure) badge(chip.x + chip.w + px(8), chip.y + px(3), "HTTP", C_Warn, C_WarnBg);
    if (ui_.showFocus(ID_MACHINE_CHIP)) focusRing(mchip, mchip.h / 2);
    if (ui_.showFocus(ID_MACHINE_CHIP)) gfx_.round(mchip, mchip.h / 2, col(C_Hover));
    text_.draw(F_Small, mText, mchip.x + px(12), mchip.y + (mchip.h - text_.lineHeight(F_Small)) / 2,
               col(mc ? C_Accent : C_Muted));
    text_.draw(F_Small, "⌄", mchip.x + mchip.w - px(20), mchip.y + (mchip.h - text_.lineHeight(F_Small)) / 2 - px(3),
               col(C_Muted));

    // Send arrow, or a stop square while generating.
    bool canSend = generating_ || !trim(draft_).empty() || !draftImages_.empty();
    sendMix_ += ((canSend ? 1.f : 0.f) - sendMix_) * 0.25f;
    auto mix = [&](ColorRole a, ColorRole b) {
        SDL_Color x = col(a), y = col(b);
        auto l = [&](Uint8 u, Uint8 v) { return (Uint8)(u + (v - u) * sendMix_); };
        return SDL_Color{l(x.r, y.r), l(x.g, y.g), l(x.b, y.b), l(x.a, y.a)};
    };
    if (ui_.showFocus(ID_SEND)) focusRing(sendR, sendR.w / 2, C_Surface);
    gfx_.circle(sendR.x + sendR.w / 2, sendR.y + sendR.h / 2, sendR.w / 2, mix(C_Border, C_Accent));
    if (generating_)
        gfx_.round({sendR.x + px(12), sendR.y + px(12), px(12), px(12)}, px(2), col(C_OnAccent));
    else
        icon("↑", sendR, mix(C_Muted, C_OnAccent));

    if (actSend) generating_ ? stop() : send();
    if ((actChip || actMachine) && generating_) {
        toast("Stop the current reply first.");  // a running tool loop must keep its provider and machine
    } else if (actChip || actMachine) {
        modal_ = actChip ? Modal::ProfilePicker : Modal::MachinePicker;
        listScroll_ = Scroller();
    }
    if (actBox) editDraft();
    if (removeShot >= 0) draftImages_.erase(draftImages_.begin() + removeShot);
    if (actAttach) {
        shots_ = platform::screenshots(48);
        modal_ = Modal::Album;
        listScroll_ = Scroller();
    }
}

// ------------------------------------------------------------------ sidebar

void App::drawSidebar() {
    float p = ease(modalAt_, 220);
    int sw = std::min(px(SidebarW), W * 4 / 5);
    int ox = -(int)((1.f - p) * sw);  // slide offset
    alpha_ = p;
    gfx_.fill({0, 0, W, H}, col(C_Scrim));
    alpha_ = 1.f;
    if (ui_.tapArea({sw, 0, W - sw, H})) modal_ = Modal::None;
    gfx_.fill({ox, 0, sw, H}, col(C_Surface));
    gfx_.fill({ox + sw - 1, 0, 1, H}, col(C_Border));
    text_.draw(F_H3, "Chats", ox + px(24), px(18), col(C_Text));

    SDL_Rect nb{ox + px(12), px(64), sw - px(24), px(46)};
    bool actNew = ui_.item(ID_SIDEBAR_NEW, nb);
    if (ui_.showFocus(ID_SIDEBAR_NEW)) {
        focusRing(nb, px(12));
        gfx_.round(nb, px(12), col(C_Hover));
    }
    gfx_.circle(nb.x + px(24), nb.y + nb.h / 2, px(13), col(C_Accent));
    int t = std::max(2, px(2));
    gfx_.fill({nb.x + px(24) - px(6), nb.y + nb.h / 2 - t / 2, px(12), t}, col(C_OnAccent));
    gfx_.fill({nb.x + px(24) - t / 2, nb.y + nb.h / 2 - px(6), t, px(12)}, col(C_OnAccent));
    text_.draw(F_Body, "New chat", nb.x + px(48), nb.y + (nb.h - text_.lineHeight(F_Body)) / 2, col(C_Accent));

    SDL_Rect area{ox, px(122), sw, H - px(122)};
    int rh = px(48);
    ui_.preferFocus(ID_SIDEBAR_NEW);  // a row below overrides this with the open chat
    listScroll_.content = (int)index_.size() * rh + px(8);
    listScroll_.view = area.h;
    listScroll_.update(ui_, area, S, true);
    ui_.clip = area;
    SDL_RenderSetClipRect(ren_, &area);
    std::string open, del;
    for (size_t i = 0; i < index_.size(); i++) {
        int y = area.y + (int)i * rh - (int)listScroll_.pos;
        SDL_Rect rr{ox + px(12), y, sw - px(24) - px(44), rh - px(4)}, dr{ox + sw - px(12) - px(40), y + (rh - px(40)) / 2 - px(2), px(40), px(40)};
        uint32_t id = ID_ROW + (uint32_t)i, did = ID_ROW_DEL + (uint32_t)i;
        if (ui_.item(id, rr)) open = index_[i].id;
        if (ui_.item(did, dr)) del = index_[i].id;
        if (ui_.justFocused(id) || ui_.justFocused(did)) listScroll_.ensureVisible((int)i * rh, rh, px(8));
        if (index_[i].id == chat_.id) ui_.preferFocus(id);
        if (y + rh < area.y || y > area.y + area.h) continue;
        bool cur = index_[i].id == chat_.id;
        if (ui_.showFocus(id)) focusRing(rr, px(10));
        if (cur || ui_.showFocus(id)) gfx_.round(rr, px(10), col(C_Hover));
        text_.draw(F_Body, fit(F_Body, index_[i].title, rr.w - px(24)), rr.x + px(12),
                   rr.y + (rr.h - text_.lineHeight(F_Body)) / 2, col(C_Text));
        if (ui_.showFocus(did)) {
            focusRing(dr, dr.w / 2);
            gfx_.circle(dr.x + dr.w / 2, dr.y + dr.h / 2, dr.w / 2, col(C_Hover));
        }
        icon("×", dr, col(C_Muted));
    }
    if (index_.empty()) text_.draw(F_Small, "No chats yet", ox + px(24), area.y + px(8), col(C_Muted));
    SDL_RenderSetClipRect(ren_, nullptr);
    ui_.clip = {0, 0, W, H};

    if (actNew) newChat();
    if (!open.empty()) openChat(open);
    if (!del.empty()) {
        auto it = std::find_if(index_.begin(), index_.end(), [&](const ChatMeta& m) { return m.id == del; });
        confirmText_ = "Delete \"" + (it != index_.end() ? it->title : std::string("chat")) + "\"?";
        confirmAction_ = ConfirmAction::DeleteChat;
        confirmArg_ = del;
        modal_ = Modal::Confirm;
    }
}

// ------------------------------------------------------------------ dialogs

void App::drawModal() {
    if (modal_ == Modal::Sidebar) return drawSidebar();
    if (modal_ == Modal::Album) return drawAlbum();

    if (modal_ == Modal::TrustHost) {
        // First connection: show the server's key fingerprint; nothing runs
        // (and no password or key is sent) until the user trusts it.
        net::Machine* m = trustForEditor_ ? &editingMachine_ : chatMachine();
        SDL_Rect c = dialog("Trust this server?", px(210));
        std::string who = m ? m->user + "@" + m->host + ":" + std::to_string(m->port) : std::string("The server");
        text_.draw(F_Body, fit(F_Body, who + " identifies itself with this key:", c.w - px(24)), c.x + px(12),
                   c.y + px(4), col(C_Text));
        text_.draw(F_Mono, fit(F_Mono, trustFingerprint_, c.w - px(24)), c.x + px(12), c.y + px(44), col(C_Text));
        text_.draw(F_Small, fit(F_Small, "Compare on the server: ssh-keygen -lf /etc/ssh/ssh_host_ecdsa_key.pub", c.w - px(24)),
                   c.x + px(12), c.y + px(86), col(C_Muted));
        bool no = button(ID_CANCEL, {c.x + c.w - px(296), c.y + c.h - px(56), px(136), px(44)}, "Cancel");
        bool yes = button(ID_OK, {c.x + c.w - px(148), c.y + c.h - px(56), px(136), px(44)}, "Trust", true);
        ui_.preferFocus(ID_CANCEL);
        if (!yes && !no) return;
        modal_ = Modal::None;
        if (trustForEditor_) {
            if (yes) {
                editingMachine_.hostKey = trustFingerprint_;  // pinned when the machine is saved
                machineTestJob_ = worker_.startCommand(editingMachine_, "echo connected; uname -sr");
                machineStatus_ = "Connecting…";
            } else {
                machineStatus_ = "Not connected: the host key was not trusted.";
            }
            machineStatusError_ = no;
            return;
        }
        const ToolCall* pc = findCall(pendingCallId_);
        if (!pc) return;
        ToolCall call = *pc;
        if (yes && m) {
            m->hostKey = trustFingerprint_;
            saveConfig();
            runCall(call);
        } else {
            pendingCallId_.clear();
            addToolResult(call, "Not run: the user did not trust the server's host key.", -1, true);
            finishResponse();
        }
        return;
    }

    if (modal_ == Modal::Confirm) {
        SDL_Rect c = dialog("Are you sure?", px(130));
        text_.draw(F_Body, fit(F_Body, confirmText_, c.w - px(24)), c.x + px(12), c.y + px(4), col(C_Text));
        SDL_Rect cancel{c.x + c.w - px(296), c.y + c.h - px(56), px(136), px(44)};
        SDL_Rect ok{c.x + c.w - px(148), c.y + c.h - px(56), px(136), px(44)};
        bool no = button(ID_CANCEL, cancel, "Cancel");
        bool yes = button(ID_OK, ok, "Delete", false, true);
        ui_.preferFocus(ID_CANCEL);
        bool fromChats = confirmAction_ == ConfirmAction::DeleteChat;
        if (yes) {
            if (fromChats) {
                // newChat() first: it stops and saves a running reply, which
                // would otherwise re-create the file we are deleting.
                if (confirmArg_ == chat_.id) newChat();
                storage::deleteChat(confirmArg_, index_);
            } else if (confirmAction_ == ConfirmAction::DeleteMachine) {
                auto& ms = cfg_.machines;
                ms.erase(std::remove_if(ms.begin(), ms.end(), [&](const net::Machine& m) { return m.id == confirmArg_; }),
                         ms.end());
                if (cfg_.defaultMachine == confirmArg_) cfg_.defaultMachine.clear();
                if (chat_.machineId == confirmArg_) chat_.machineId.clear();
                saveConfig();
            } else {
                auto& ps = cfg_.profiles;
                ps.erase(std::remove_if(ps.begin(), ps.end(), [&](const Profile& p) { return p.id == confirmArg_; }),
                         ps.end());
                if (cfg_.defaultProfile == confirmArg_) cfg_.defaultProfile = ps.empty() ? "" : ps[0].id;
                saveConfig();
            }
        }
        if (yes || no) modal_ = fromChats ? Modal::Sidebar : Modal::None;
        return;
    }

    // List dialogs share one layout: optional fixed rows, then a scrolling list.
    struct Row {
        uint32_t id;
        std::string label, value;
    };
    std::vector<Row> rows;
    std::string title, status;
    bool statusError = false;
    if (modal_ == Modal::ProfilePicker) {
        title = "Provider & model";
        rows.push_back({ID_CHANGE_MODEL, "Model", chatModel().empty() ? "Choose…" : chatModel()});
        for (size_t i = 0; i < cfg_.profiles.size(); i++) {
            const Profile& p = cfg_.profiles[i];
            bool cur = chatProfile() && chatProfile()->id == p.id;
            rows.push_back({ID_ROW + (uint32_t)i, (cur ? "✓ " : "    ") + p.name, p.model});
        }
        rows.push_back({ID_MANAGE, "Manage providers…", ""});
    } else if (modal_ == Modal::ModelPicker) {
        title = "Choose model";
        rows.push_back({ID_TYPE_MODEL, "Type a model name…", ""});
        for (size_t i = 0; i < models_.size(); i++)
            rows.push_back({ID_ROW + (uint32_t)i, models_[i].id, models_[i].name == models_[i].id ? "" : models_[i].name});
        if (modelsLoading_) status = "Loading models…";
        if (!modelsError_.empty()) {
            status = modelsError_;
            statusError = true;
        }
    } else if (modal_ == Modal::MachinePicker) {
        title = "Run commands on";
        rows.push_back({ID_NO_MACHINE, std::string(chat_.machineId.empty() ? "✓ " : "    ") + "No machine (chat only)", ""});
        for (size_t i = 0; i < cfg_.machines.size(); i++) {
            const net::Machine& m = cfg_.machines[i];
            rows.push_back({ID_ROW + (uint32_t)i, (m.id == chat_.machineId ? "✓ " : "    ") + m.name, m.user + "@" + m.host});
        }
        rows.push_back({ID_MANAGE, "Manage machines…", ""});
    } else if (modal_ == Modal::PresetPicker) {
        title = "Add provider";
        auto presets = providerPresets();
        for (size_t i = 0; i < presets.size(); i++)
            rows.push_back({ID_ROW + (uint32_t)i, presets[i].name, presets[i].baseUrl});
    }

    int rh = px(RowH) + px(4);
    int statusH = status.empty() ? 0 : px(34);
    SDL_Rect c = dialog(title, (int)rows.size() * rh + statusH);
    if (!status.empty())
        text_.draw(F_Small, fit(F_Small, status, c.w - px(24)), c.x + px(12), c.y + px(4),
                   col(statusError ? C_Error : C_Muted));
    SDL_Rect area{c.x, c.y + statusH, c.w, c.h - statusH};
    listScroll_.content = (int)rows.size() * rh;
    listScroll_.view = area.h;
    listScroll_.update(ui_, area, S, true);
    ui_.clip = area;
    SDL_RenderSetClipRect(ren_, &area);
    uint32_t hit = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        int y = area.y + (int)i * rh - (int)listScroll_.pos;
        if (row(rows[i].id, area.x, y, area.w, rows[i].label, rows[i].value, &listScroll_, area.y - (int)listScroll_.pos))
            hit = rows[i].id;
    }
    SDL_RenderSetClipRect(ren_, nullptr);
    ui_.clip = {0, 0, W, H};
    if (!hit) return;

    if (modal_ == Modal::ProfilePicker) {
        if (hit == ID_CHANGE_MODEL) return openModels(false);
        modal_ = Modal::None;
        if (hit == ID_MANAGE) {
            screen_ = Screen::Settings;
        } else {
            // Switching provider mid-chat is fine: the same neutral history is sent.
            const Profile& p = cfg_.profiles[hit - ID_ROW];
            chat_.profileId = p.id;
            chat_.model = p.model;
        }
    } else if (modal_ == Modal::ModelPicker) {
        std::string chosen;
        if (hit == ID_TYPE_MODEL) {
            std::string cur = modelsForEditor_ ? editing_.model : chatModel();
            if (!platform::keyboard("Model name", cur, chosen) || trim(chosen).empty()) return;
            chosen = trim(chosen);
        } else {
            chosen = models_[hit - ID_ROW].id;
        }
        if (modelsForEditor_)
            editing_.model = chosen;
        else
            chat_.model = chosen;
        modal_ = Modal::None;
    } else if (modal_ == Modal::MachinePicker) {
        modal_ = Modal::None;
        if (hit == ID_MANAGE) {
            screen_ = Screen::Settings;
            return;
        }
        chat_.machineId = hit == ID_NO_MACHINE ? "" : cfg_.machines[hit - ID_ROW].id;
        cfg_.defaultMachine = chat_.machineId;  // new chats start with the last choice
        autoApprove_ = false;
        saveConfig();
    } else if (modal_ == Modal::PresetPicker) {
        editing_ = providerPresets()[hit - ID_ROW];
        editing_.id = storage::newId();
        editingIndex_ = -1;
        editorStatus_.clear();
        editorScroll_ = Scroller();
        screen_ = Screen::Editor;
        modal_ = Modal::None;
    }
}

void App::drawToast() {
    if (toast_.empty() || SDL_TICKS_PASSED(SDL_GetTicks(), toastUntil_)) return;
    float in = ease(toastAt_, 200), out = std::min(1.f, (toastUntil_ - SDL_GetTicks()) / 300.f);
    alpha_ = std::min(in, out);
    TextLayout tl = text_.layout({{toast_, F_Small, C_Bg}}, std::min(px(640), W - px(80)));
    SDL_Rect r{(W - tl.width) / 2 - px(18), px(TopBarH) + px(6) - (int)((1.f - in) * px(10)), tl.width + px(36),
               tl.height + px(16)};
    gfx_.round(r, px(14), col(C_Text));
    text_.draw(tl, r.x + px(18), r.y + px(8), pc(), 0, H);
    alpha_ = 1.f;
}

// ------------------------------------------------------------------ settings

void App::drawSettings() {
    drawTopBar("Settings", true);
    int top = px(TopBarH);
    SDL_Rect area{0, top, W, H - top};
    int cw = std::min(px(720), W - 2 * px(Gutter)), x = (W - cw) / 2;
    settingsScroll_.view = area.h;
    settingsScroll_.update(ui_, area, S, true);
    int origin = top + px(8) - (int)settingsScroll_.pos;
    int y = origin;
    ui_.clip = area;
    SDL_RenderSetClipRect(ren_, &area);

    auto section = [&](const char* name) {
        y += px(14);
        text_.draw(F_SmallBold, name, x + px(16), y, col(C_Muted));
        y += text_.lineHeight(F_SmallBold) + px(6);
    };
    auto ensure = [&](uint32_t id, int ry, int rh) {
        if (ui_.justFocused(id)) settingsScroll_.ensureVisible(ry - origin, rh, px(16));
    };

    section("GENERAL");
    if (row(ID_NAME, x, y, cw, "Your name", cfg_.userName.empty() ? "Not set" : cfg_.userName, &settingsScroll_, origin)) {
        std::string out;
        if (platform::keyboard("Your name", cfg_.userName, out)) {
            cfg_.userName = trim(out);
            saveConfig();
        }
    }
    y += px(RowH);
    if (row(ID_THEME, x, y, cw, "Theme", themeLabel(cfg_.theme), &settingsScroll_, origin)) {
        cfg_.theme = cfg_.theme == "auto" ? "light" : cfg_.theme == "light" ? "dark" : "auto";
        applyTheme();
        saveConfig();
    }
    y += px(RowH);
    if (row(ID_FONT, x, y, cw, "Font size", kFontSizeNames[cfg_.fontSize], &settingsScroll_, origin)) {
        cfg_.fontSize = (cfg_.fontSize + 1) % 3;
        saveConfig();  // fonts are reopened by updateScale() next frame
    }
    y += px(RowH);
    Profile* def = findProfile(cfg_.defaultProfile);
    if (row(ID_DEFAULT, x, y, cw, "Default provider", def ? def->name : "None", &settingsScroll_, origin) &&
        !cfg_.profiles.empty()) {
        size_t i = 0;
        while (i < cfg_.profiles.size() && cfg_.profiles[i].id != cfg_.defaultProfile) i++;
        cfg_.defaultProfile = cfg_.profiles[(i + 1) % cfg_.profiles.size()].id;
        saveConfig();
    }
    y += px(RowH);

    if (row(ID_GAMES, x, y, cw, "Game library for the AI", cfg_.shareGames ? "On" : "Off", &settingsScroll_, origin)) {
        cfg_.shareGames = !cfg_.shareGames;
        saveConfig();
    }
    y += px(RowH);

    section("PROVIDERS");
    int action = -1, actionIdx = -1;
    for (size_t i = 0; i < cfg_.profiles.size(); i++) {
        const Profile& p = cfg_.profiles[i];
        int ch = px(122);
        SDL_Rect card{x, y, cw, ch};
        gfx_.roundBorder(card, px(14), std::max(1, px(1)), col(C_Border), col(C_Surface));
        int nx = card.x + px(18) + text_.draw(F_Bold, fit(F_Bold, p.name, cw / 2), card.x + px(18), card.y + px(14), col(C_Text));
        if (p.id == cfg_.defaultProfile) {
            badge(nx + px(10), card.y + px(15), "Default", C_Accent, C_Hover);
            nx += px(24) + text_.measure(F_SmallBold, "Default");
        }
        if (net::checkUrl(p.baseUrl.empty() ? "https://x" : p.baseUrl) == net::UrlCheck::Insecure)
            badge(nx + px(10), card.y + px(15), "HTTP · not encrypted", C_Warn, C_WarnBg);
        std::string sub = p.type + "  ·  " + (p.model.empty() ? "no model" : p.model) + "  ·  key " + maskKey(p.apiKey);
        text_.draw(F_Small, fit(F_Small, sub, cw - px(36)), card.x + px(18), card.y + px(44), col(C_Muted));
        const char* labels[4] = {"Edit", "Duplicate", "Make default", "Delete"};
        int bx = card.x + px(18), bw = (cw - px(36) - 3 * px(10)) / 4;
        for (int k = 0; k < 4; k++) {
            SDL_Rect b{bx + k * (bw + px(10)), card.y + ch - px(52), bw, px(38)};
            uint32_t id = ID_CARD + (uint32_t)i * 8 + k;
            if (button(id, b, labels[k], false, k == 3)) {
                action = k;
                actionIdx = (int)i;
            }
            ensure(id, card.y, ch);
        }
        y += ch + px(10);
    }
    SDL_Rect add{x, y, px(200), px(44)};
    if (button(ID_ADD_PROFILE, add, "+ Add provider", true)) {
        modal_ = Modal::PresetPicker;
        listScroll_ = Scroller();
    }
    ensure(ID_ADD_PROFILE, y, px(44));
    y += px(56);

    section("MACHINES (SSH)");
    int mAction = -1, mIdx = -1;
    for (size_t i = 0; i < cfg_.machines.size(); i++) {
        const net::Machine& m = cfg_.machines[i];
        int ch = px(122);
        SDL_Rect card{x, y, cw, ch};
        gfx_.roundBorder(card, px(14), std::max(1, px(1)), col(C_Border), col(C_Surface));
        int nx = card.x + px(28) + text_.draw(F_Bold, fit(F_Bold, m.name, cw / 2), card.x + px(18), card.y + px(14), col(C_Text));
        badge(nx, card.y + px(15), m.auth == "password" ? "Password" : "Key", C_Muted, C_Hover);
        nx += text_.measure(F_SmallBold, m.auth == "password" ? "Password" : "Key") + px(24);
        if (!m.hostKey.empty()) badge(nx, card.y + px(15), "Host key pinned", C_Accent, C_Hover);
        std::string sub = m.user + "@" + m.host + ":" + std::to_string(m.port) + (m.auth == "key" ? "  ·  " + m.keyPath : "");
        text_.draw(F_Small, fit(F_Small, sub, cw - px(36)), card.x + px(18), card.y + px(44), col(C_Muted));
        const char* labels[3] = {"Edit", "Duplicate", "Delete"};
        int bw = (cw - px(36) - 2 * px(10)) / 3;
        for (int k = 0; k < 3; k++) {
            uint32_t id = ID_MCARD + (uint32_t)i * 8 + k;
            if (button(id, {card.x + px(18) + k * (bw + px(10)), card.y + ch - px(52), bw, px(38)}, labels[k], false, k == 2)) {
                mAction = k;
                mIdx = (int)i;
            }
            ensure(id, card.y, ch);
        }
        y += ch + px(10);
    }
    if (cfg_.machines.empty()) {
        text_.draw(F_Small, "Add a server so the AI can run commands on it over SSH (you approve each command).",
                   x + px(16), y, col(C_Muted));
        y += text_.lineHeight(F_Small) + px(10);
    }
    if (button(ID_ADD_MACHINE, {x, y, px(200), px(44)}, "+ Add machine", true)) mAction = 3;
    ensure(ID_ADD_MACHINE, y, px(44));
    y += px(56);

    section("ABOUT");
    text_.draw(F_Small, "joymind " APP_VERSION_STR " by pouyakh.dev. Not affiliated with Anthropic or Nintendo.",
               x + px(16), y, col(C_Muted));
    y += text_.lineHeight(F_Small) + px(4);
    text_.draw(F_Small, "A select · B back · X new chat · Y keyboard · L/R scroll · + settings · - chats",
               x + px(16), y, col(C_Muted));
    y += text_.lineHeight(F_Small) + px(14);
    if (button(ID_QUIT, {x, y, px(140), px(44)}, "Quit app", false, true)) running_ = false;
    ensure(ID_QUIT, y, px(44));
    y += px(64);

    SDL_RenderSetClipRect(ren_, nullptr);
    ui_.clip = {0, 0, W, H};
    settingsScroll_.content = y - origin;
    ui_.preferFocus(ID_NAME);

    if (mAction == 3 || mIdx >= 0) {
        net::Machine m;
        if (mIdx >= 0) m = cfg_.machines[mIdx];
        if (mAction == 0 || mAction == 3) {
            if (mAction == 3) {
                m.id = storage::newId();
                m.name = "My server";
                m.keyPath = platform::assetDir() == "romfs:/" ? "sdmc:/switch/ai-switch/keys/id_ecdsa"
                                                                 : "ai-switch-data/keys/id_ecdsa";
            }
            editingMachine_ = m;
            editingMachineIndex_ = mAction == 3 ? -1 : mIdx;
            machineStatus_.clear();
            machineScroll_ = Scroller();
            screen_ = Screen::MachineEditor;
        } else if (mAction == 1) {
            m.id = storage::newId();
            m.name += " (copy)";
            cfg_.machines.insert(cfg_.machines.begin() + mIdx + 1, m);
            saveConfig();
        } else {
            confirmText_ = "Delete machine \"" + m.name + "\"?";
            confirmAction_ = ConfirmAction::DeleteMachine;
            confirmArg_ = m.id;
            modal_ = Modal::Confirm;
        }
        return;
    }
    if (actionIdx < 0) return;
    Profile p = cfg_.profiles[actionIdx];
    if (action == 0) {
        editing_ = p;
        editingIndex_ = actionIdx;
        editorStatus_.clear();
        editorScroll_ = Scroller();
        screen_ = Screen::Editor;
    } else if (action == 1) {
        p.id = storage::newId();
        p.name += " (copy)";
        cfg_.profiles.insert(cfg_.profiles.begin() + actionIdx + 1, p);
        saveConfig();
    } else if (action == 2) {
        cfg_.defaultProfile = p.id;
        saveConfig();
    } else {
        confirmText_ = "Delete provider \"" + p.name + "\"?";
        confirmAction_ = ConfirmAction::DeleteProfile;
        confirmArg_ = p.id;
        modal_ = Modal::Confirm;
    }
}

// ------------------------------------------------------------------ profile editor

void App::drawEditor() {
    drawTopBar(editingIndex_ < 0 ? "Add provider" : "Edit provider", true);
    int top = px(TopBarH);
    SDL_Rect area{0, top, W, H - top};
    int cw = std::min(px(720), W - 2 * px(Gutter)), x = (W - cw) / 2;
    editorScroll_.view = area.h;
    editorScroll_.update(ui_, area, S, true);
    int origin = top + px(8) - (int)editorScroll_.pos;
    int y = origin;
    ui_.clip = area;
    SDL_RenderSetClipRect(ren_, &area);

    Profile& e = editing_;
    std::string temp = e.hasTemperature ? std::to_string(e.temperature).substr(0, 4) : "Provider default";
    std::string url = e.baseUrl.empty() ? "Default" : e.baseUrl;
    const std::string fields[10][2] = {
        {"Name", e.name},
        {"Type", e.type},
        {"Base URL", url},
        {"API key", maskKey(e.apiKey)},
        {"Model", e.model.empty() ? "Choose…" : e.model},
        {"Max tokens", std::to_string(e.maxTokens)},
        {"Max tokens field", e.maxTokensField + " (OpenAI-compatible)"},
        {"Temperature", temp},
        {"System prompt", e.systemPrompt.empty() ? "None" : e.systemPrompt},
        {"Extra headers", headerNames(e.extraHeaders)},
    };
    int hit = -1;
    for (int k = 0; k < 10; k++) {
        if (row(ID_FIELD + k, x, y, cw, fields[k][0], fields[k][1], &editorScroll_, origin)) hit = k;
        y += px(RowH);
    }
    net::UrlCheck uc = e.baseUrl.empty() ? net::UrlCheck::Ok : net::checkUrl(e.baseUrl);
    if (uc == net::UrlCheck::Insecure) {
        badge(x + px(16), y + px(8), "HTTP · not encrypted: only use this on your own network", C_Warn, C_WarnBg);
        y += px(40);
    }
    if (!editorStatus_.empty()) {
        TextLayout tl = text_.layout({{editorStatus_, F_Small, (unsigned char)(editorStatusError_ ? C_Error : C_Muted)}}, cw - px(32));
        text_.draw(tl, x + px(16), y + px(8), pc(), area.y, area.y + area.h);
        y += tl.height + px(16);
    }
    y += px(8);
    int bw = px(190);
    bool test = button(ID_TEST, {x, y, bw, px(44)}, testJob_ ? "Testing…" : "Test connection");
    bool save = button(ID_SAVE, {x + cw - 2 * bw - px(10), y, bw, px(44)}, "Save", true);
    bool cancel = button(ID_CANCEL, {x + cw - bw, y, bw, px(44)}, "Cancel");
    for (uint32_t id : {ID_TEST, ID_SAVE, ID_CANCEL})
        if (ui_.justFocused(id)) editorScroll_.ensureVisible(y - origin, px(44), px(16));
    y += px(64);
    SDL_RenderSetClipRect(ren_, nullptr);
    ui_.clip = {0, 0, W, H};
    editorScroll_.content = y - origin;
    ui_.preferFocus(ID_FIELD);

    std::string out;
    switch (hit) {
    case 0:
        if (platform::keyboard("Display name", e.name, out) && !trim(out).empty()) e.name = trim(out);
        break;
    case 1: {
        const auto& types = providerTypes();
        auto it = std::find(types.begin(), types.end(), e.type);
        e.type = it == types.end() || it + 1 == types.end() ? types[0] : *(it + 1);
        break;
    }
    case 2:
        if (platform::keyboard("Base URL (empty = provider default)", e.baseUrl, out)) e.baseUrl = trim(out);
        break;
    case 3:
        // Never pre-fill the existing key: the keyboard would show it in full.
        if (platform::keyboard("API key (leave empty to keep the current key)", "", out) && !trim(out).empty())
            e.apiKey = trim(out);
        break;
    case 4: openModels(true); break;
    case 5:
        if (platform::keyboard("Max tokens", std::to_string(e.maxTokens), out) && atoi(out.c_str()) > 0)
            e.maxTokens = atoi(out.c_str());
        break;
    case 6: e.maxTokensField = e.maxTokensField == "max_tokens" ? "max_completion_tokens" : "max_tokens"; break;
    case 7:
        if (platform::keyboard("Temperature 0-2 (empty = provider default)", e.hasTemperature ? temp : "", out)) {
            e.hasTemperature = !trim(out).empty();
            e.temperature = std::min(2.f, std::max(0.f, (float)atof(out.c_str())));
        }
        break;
    case 8:
        if (platform::keyboard("System prompt", e.systemPrompt, out, true, 1000)) e.systemPrompt = out;
        break;
    case 9: {
        std::string cur;
        for (const auto& kv : e.extraHeaders) cur += (cur.empty() ? "" : "; ") + kv.first + ": " + kv.second;
        if (platform::keyboard("Extra headers  (Name: value; Name2: value)", cur, out)) e.extraHeaders = parseHeaders(out);
        break;
    }
    default: break;
    }

    if (test) {
        if (testJob_) return;
        testJob_ = worker_.startTest(e);
        editorStatus_ = testJob_ ? "Testing…" : "Busy with another request, try again in a moment.";
        editorStatusError_ = !testJob_;
    }
    if (cancel) screen_ = Screen::Settings;
    if (save) {
        std::string why;
        if (!e.baseUrl.empty() && net::checkUrl(e.baseUrl, &why) == net::UrlCheck::Rejected) {
            editorStatus_ = why;
            editorStatusError_ = true;
            return;
        }
        if (editingIndex_ >= 0 && editingIndex_ < (int)cfg_.profiles.size())
            cfg_.profiles[editingIndex_] = e;
        else
            cfg_.profiles.push_back(e);
        if (!findProfile(cfg_.defaultProfile)) cfg_.defaultProfile = e.id;
        if (chat_.messages.empty() && chat_.profileId.empty()) {
            chat_.profileId = e.id;
            chat_.model = e.model;
        }
        saveConfig();
        screen_ = Screen::Settings;
    }
}

// ------------------------------------------------------------------ machine editor

void App::drawMachineEditor() {
    drawTopBar(editingMachineIndex_ < 0 ? "Add machine" : "Edit machine", true);
    int top = px(TopBarH);
    SDL_Rect area{0, top, W, H - top};
    int cw = std::min(px(720), W - 2 * px(Gutter)), x = (W - cw) / 2;
    machineScroll_.view = area.h;
    machineScroll_.update(ui_, area, S, true);
    int origin = top + px(8) - (int)machineScroll_.pos;
    int y = origin;
    ui_.clip = area;
    SDL_RenderSetClipRect(ren_, &area);

    net::Machine& m = editingMachine_;
    bool key = m.auth == "key";
    struct Field {
        int k;
        std::string label, value;
    };
    std::vector<Field> fields = {
        {0, "Name", m.name},
        {1, "Host", m.host.empty() ? "IP address or domain" : m.host},
        {2, "Port", std::to_string(m.port)},
        {3, "User", m.user.empty() ? "Not set" : m.user},
        {4, "Login with", key ? "Key file" : "Password"},
    };
    if (key) {
        fields.push_back({5, "Key file", m.keyPath.empty() ? "Not set" : m.keyPath});
        fields.push_back({6, "Key passphrase", m.passphrase.empty() ? "None" : "••••••••"});
    } else {
        fields.push_back({7, "Password", m.password.empty() ? "Not set" : "••••••••"});
    }
    fields.push_back({8, "Host key", m.hostKey.empty() ? "Checked on first connect" : m.hostKey});
    int hit = -1;
    for (const auto& f : fields) {
        if (row(ID_MFIELD + f.k, x, y, cw, f.label, f.value, &machineScroll_, origin)) hit = f.k;
        y += px(RowH);
    }
    if (key) {
        TextLayout hint = text_.layout(
            {{"Copy your ECDSA private key and its .pub file to the SD card (for example sdmc:/switch/ai-switch/keys/). "
              "ed25519 and RSA keys don't work with this SSH library.",
              F_Small, C_Muted}},
            cw - px(32));
        text_.draw(hint, x + px(16), y + px(8), pc(), area.y, area.y + area.h);
        y += hint.height + px(12);
    }
    if (!machineStatus_.empty()) {
        TextLayout tl = text_.layout({{machineStatus_, F_Small, (unsigned char)(machineStatusError_ ? C_Error : C_Muted)}},
                                     cw - px(32));
        text_.draw(tl, x + px(16), y + px(8), pc(), area.y, area.y + area.h);
        y += tl.height + px(16);
    }
    y += px(8);
    int bw = px(190);
    bool test = button(ID_TEST, {x, y, bw, px(44)}, machineTestJob_ ? "Connecting…" : "Test connection");
    bool save = button(ID_SAVE, {x + cw - 2 * bw - px(10), y, bw, px(44)}, "Save", true);
    bool cancel = button(ID_CANCEL, {x + cw - bw, y, bw, px(44)}, "Cancel");
    for (uint32_t id : {ID_TEST, ID_SAVE, ID_CANCEL})
        if (ui_.justFocused(id)) machineScroll_.ensureVisible(y - origin, px(44), px(16));
    y += px(64);
    SDL_RenderSetClipRect(ren_, nullptr);
    ui_.clip = {0, 0, W, H};
    machineScroll_.content = y - origin;
    ui_.preferFocus(ID_MFIELD);

    std::string out;
    switch (hit) {
    case 0:
        if (platform::keyboard("Display name", m.name, out) && !trim(out).empty()) m.name = trim(out);
        break;
    case 1:
        if (platform::keyboard("Host (IP address or domain)", m.host, out)) {
            if (trim(out) != m.host) m.hostKey.clear();  // a different server has a different key
            m.host = trim(out);
        }
        break;
    case 2:
        if (platform::keyboard("SSH port", std::to_string(m.port), out) && atoi(out.c_str()) > 0 && atoi(out.c_str()) < 65536)
            m.port = atoi(out.c_str());
        break;
    case 3:
        if (platform::keyboard("User name", m.user, out)) m.user = trim(out);
        break;
    case 4: m.auth = key ? "password" : "key"; break;
    case 5:
        if (platform::keyboard("Private key file on the SD card", m.keyPath, out)) m.keyPath = trim(out);
        break;
    case 6:
        // never pre-filled; an empty answer means "no passphrase"
        if (platform::keyboard("Key passphrase (empty = none)", "", out)) m.passphrase = out;
        break;
    case 7:
        if (platform::keyboard("Password (leave empty to keep the current one)", "", out) && !out.empty()) m.password = out;
        break;
    case 8:
        if (!m.hostKey.empty()) {
            m.hostKey.clear();
            toast("Pinned host key forgotten. You'll be asked to confirm it on the next connection.");
        }
        break;
    default: break;
    }

    std::string missing = m.host.empty() ? "Enter the host." : m.user.empty() ? "Enter the user name." :
                          key && m.keyPath.empty() ? "Enter the key file path." :
                          !key && m.password.empty() ? "Enter the password." : "";
    if ((test || save) && !missing.empty()) {
        machineStatus_ = missing;
        machineStatusError_ = true;
        return;
    }
    if (test && !machineTestJob_) {
        machineTestJob_ = worker_.startCommand(m, "echo connected; uname -sr");
        machineStatus_ = machineTestJob_ ? "Connecting…" : "Busy with another request, try again in a moment.";
        machineStatusError_ = !machineTestJob_;
    }
    if (cancel) screen_ = Screen::Settings;
    if (save) {
        if (editingMachineIndex_ >= 0 && editingMachineIndex_ < (int)cfg_.machines.size())
            cfg_.machines[editingMachineIndex_] = m;
        else
            cfg_.machines.push_back(m);
        saveConfig();
        screen_ = Screen::Settings;
    }
}

// ------------------------------------------------------------------ album picker

void App::drawAlbum() {
    const int cols = 4, gap = px(12), dw = 900;
    int cw = std::min(px(dw), W - 2 * px(Gutter)) - px(24);  // matches dialog()'s content width
    int tw = (cw - (cols - 1) * gap) / cols, th = tw * 9 / 16, tileH = th + px(28);
    int rows = ((int)shots_.size() + cols - 1) / cols;
    SDL_Rect c = dialog("Attach a screenshot", shots_.empty() ? px(96) : rows * (tileH + gap), dw);
    if (shots_.empty()) {
        TextLayout tl = text_.layout(
            {{"No screenshots in " + platform::albumDir() +
                  ". Press the capture button in a game first. Screenshots saved to System Memory aren't visible "
                  "here: set System Settings > Data Management > Save Screenshots to microSD card.",
              F_Small, C_Muted}},
            c.w - px(24));
        text_.draw(tl, c.x + px(12), c.y + px(4), pc(), 0, H);
        return;
    }
    listScroll_.content = rows * (tileH + gap);
    listScroll_.view = c.h;
    listScroll_.update(ui_, c, S, true);
    ui_.clip = c;
    SDL_RenderSetClipRect(ren_, &c);
    int hit = -1;
    for (size_t i = 0; i < shots_.size(); i++) {
        int r = (int)i / cols, k = (int)i % cols;
        SDL_Rect t{c.x + k * (tw + gap), c.y + r * (tileH + gap) - (int)listScroll_.pos, tw, th};
        uint32_t id = ID_ROW + (uint32_t)i;
        if (ui_.item(id, {t.x, t.y, tw, tileH})) hit = (int)i;
        if (ui_.justFocused(id)) listScroll_.ensureVisible(r * (tileH + gap), tileH, px(8));
        if (t.y + tileH < c.y || t.y > c.y + c.h) continue;  // only visible tiles get decoded
        if (ui_.showFocus(id)) focusRing(t, px(8));
        if (SDL_Texture* tex = thumb(shots_[i].path)) {
            SDL_SetTextureAlphaMod(tex, (Uint8)(255 * alpha_));
            SDL_RenderCopy(ren_, tex, nullptr, &t);
        } else {
            gfx_.round(t, px(8), col(C_CodeBg));
        }
        text_.draw(F_Small, shots_[i].label, t.x + px(2), t.y + th + px(6), col(C_Muted));
    }
    SDL_RenderSetClipRect(ren_, nullptr);
    ui_.clip = {0, 0, W, H};
    if (hit < 0) return;
    if (draftImages_.size() >= 4)
        toast("Up to 4 screenshots per message.");
    else
        draftImages_.push_back(shots_[hit].path);
    modal_ = Modal::None;
    ui_.setFocus(ID_SEND);
}
