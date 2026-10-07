// Message layout and drawing: user bubbles, Markdown assistant replies,
// error cards and the streaming sparkle.
#include <algorithm>
#include <cmath>
#include <string>

#include "app/app.hpp"

namespace {
bool sameBlock(const md::Block& a, const md::Block& b) {
    return a.type == b.type && a.level == b.level && a.open == b.open && a.text == b.text && a.lang == b.lang &&
           a.marker == b.marker;
}
}  // namespace

std::vector<TextSpan> App::inlineSpans(const std::string& text, unsigned char base, unsigned char color) {
    std::vector<TextSpan> out;
    bool heading = base == F_H1 || base == F_H2 || base == F_H3;
    for (auto& in : md::parseInline(text)) {
        TextSpan s;
        s.text = std::move(in.text);
        s.color = (in.style & md::Link) ? (unsigned char)C_Accent : color;
        if (in.style & md::Code) {
            s.font = F_Mono;
            s.flags = SpanCodeBg;
        } else if (heading) {
            s.font = base;
        } else {
            bool b = in.style & md::Bold, i = in.style & md::Italic;
            s.font = base;
            if (b && i)
                s.font = F_BoldItalic;
            else if (b)
                s.font = F_Bold;
            else if (i)
                s.font = F_Italic;
        }
        out.push_back(std::move(s));
    }
    return out;
}

int App::blockGap(md::BlockType a, md::BlockType b) const {
    bool listA = a == md::BlockType::Bullet || a == md::BlockType::Ordered;
    bool listB = b == md::BlockType::Bullet || b == md::BlockType::Ordered;
    if (listA && listB) return px(6);
    if (a == md::BlockType::Heading) return px(8);
    return px(14);
}

App::BlockView App::layoutBlock(const md::Block& b, int width) {
    BlockView v;
    v.block = b;
    auto addText = [&](int x, int y, TextLayout&& tl) {
        Piece p;
        p.kind = Piece::Text;
        p.r = {x, y, tl.width, tl.height};
        p.text = std::move(tl);
        v.pieces.push_back(std::move(p));
    };
    switch (b.type) {
    case md::BlockType::Paragraph: {
        TextLayout tl = text_.layout(inlineSpans(b.text, F_Body, C_Text), width);
        v.h = tl.height;
        addText(0, 0, std::move(tl));
        break;
    }
    case md::BlockType::Heading: {
        unsigned char f = b.level == 1 ? F_H1 : b.level == 2 ? F_H2 : F_H3;
        TextLayout tl = text_.layout(inlineSpans(b.text, f, C_Text), width);
        v.h = px(6) + tl.height;
        addText(0, px(6), std::move(tl));
        break;
    }
    case md::BlockType::Bullet:
    case md::BlockType::Ordered: {
        int indent = px(22) * b.level;
        std::string marker = b.type == md::BlockType::Bullet ? "•" : b.marker;
        int markerW = std::max(px(24), text_.measure(F_Body, marker) + px(8));
        TextLayout tl = text_.layout(inlineSpans(b.text, F_Body, C_Text), std::max(px(80), width - indent - markerW));
        v.h = tl.height;
        addText(indent, 0, text_.layout({{marker, F_Body, C_Muted}}, markerW * 2));
        addText(indent + markerW, 0, std::move(tl));
        break;
    }
    case md::BlockType::Quote: {
        int pad = px(16);
        TextLayout tl = text_.layout(inlineSpans(b.text, F_Body, C_Muted), width - pad);
        Piece bar;
        bar.kind = Piece::QuoteBar;
        bar.r = {0, 0, px(3), tl.height};
        v.pieces.push_back(bar);
        v.h = tl.height;
        addText(pad, 0, std::move(tl));
        break;
    }
    case md::BlockType::Code: {
        int pad = px(14);
        int labelH = b.lang.empty() ? 0 : text_.lineHeight(F_MonoSmall) + px(6);
        std::string code;
        for (char c : b.text) code += c == '\t' ? std::string("    ") : std::string(1, c);
        // Long lines wrap inside the box (easier than horizontal scrolling with a pad).
        TextLayout tl = text_.layout({{code.empty() ? " " : code, F_Mono, C_Text}}, width - 2 * pad, true);
        Piece box;
        box.kind = Piece::CodeBox;
        box.r = {0, 0, width, pad + labelH + tl.height + pad};
        box.label = b.lang;
        v.h = box.r.h;
        v.pieces.push_back(box);
        addText(pad, pad + labelH, std::move(tl));
        break;
    }
    case md::BlockType::Rule: {
        Piece r;
        r.kind = Piece::Rule;
        r.r = {0, px(10), width, std::max(1, px(1))};
        v.pieces.push_back(r);
        v.h = px(21);
        break;
    }
    }
    return v;
}

void App::layoutMessage(const ChatMessage& m, MsgView& v, int width) {
    if (v.len == m.text.size() && v.errLen == m.error.size() && v.callsN == m.calls.size() && v.width == width) return;
    v.user = m.role == "user";
    if (v.user) {
        v.plain = text_.layout({{m.text, F_Body, C_Text}}, (int)(width * 0.8f) - 2 * px(18));
        v.imagesH = m.imagePaths.empty() ? 0 : px(150) + (m.text.empty() ? 0 : px(8));
        v.textH = v.imagesH + (m.text.empty() ? 0 : v.plain.height + 2 * px(12));
    } else if (m.role == "tool") {
        v.textH = 0;  // shown inside the command card of its call
    } else {
        // Re-parse the whole message (cheap) but re-layout only blocks that
        // changed: while streaming that is just the last one or two.
        std::vector<md::Block> blocks = md::parse(m.text);
        std::vector<BlockView> nb;
        nb.reserve(blocks.size());
        for (size_t i = 0; i < blocks.size(); i++) {
            if (v.width == width && i < v.blocks.size() && sameBlock(v.blocks[i].block, blocks[i]))
                nb.push_back(std::move(v.blocks[i]));
            else
                nb.push_back(layoutBlock(blocks[i], width));
        }
        v.blocks = std::move(nb);
        int y = 0;
        for (size_t i = 0; i < v.blocks.size(); i++) {
            y += v.blocks[i].h;
            if (i + 1 < v.blocks.size()) y += blockGap(v.blocks[i].block.type, v.blocks[i + 1].block.type);
        }
        v.textH = m.text.empty() && m.error.empty() && m.calls.empty() ? px(28) : y;  // room for the waiting sparkle
    }
    if (!m.error.empty()) v.error = text_.layout({{"⚠  " + m.error, F_Small, C_Error}}, width - px(32));
    v.len = m.text.size();
    v.errLen = m.error.size();
    v.callsN = m.calls.size();
    v.width = width;
}

int App::msgHeight(const MsgView& v) const {
    int h = v.textH;
    if (v.callsH) h += (h ? px(12) : 0) + v.callsH;
    if (v.errLen) h += (h ? px(10) : 0) + v.error.height + px(18);
    return h;
}

void App::layoutCalls(size_t mi, MsgView& v, int width) {
    const ChatMessage& m = chat_.messages[mi];
    // Each call's state: answered (by a tool message right after), running,
    // waiting for approval, or never run. Re-lay out only when that changes.
    std::vector<const ChatMessage*> results(m.calls.size(), nullptr);
    std::string key = std::to_string(width);
    for (size_t k = 0; k < m.calls.size(); k++) {
        const std::string& id = m.calls[k].id;
        for (size_t j = mi + 1; j < chat_.messages.size() && chat_.messages[j].role == "tool"; j++)
            if (chat_.messages[j].callId == id) results[k] = &chat_.messages[j];
        key += "|" + id +
               (results[k] ? "R" + std::to_string(results[k]->text.size())
                           : id == runningCallId_ ? "X" : id == pendingCallId_ ? "P" : "N");
    }
    if (key == v.callsKey) return;
    v.callsKey = key;
    v.calls.clear();
    v.callsH = 0;
    const int pad = px(14), inner = width - 2 * pad;
    for (size_t k = 0; k < m.calls.size(); k++) {
        const ToolCall& c = m.calls[k];
        CallView cv;
        cv.id = c.id;
        bool games = c.name == "get_game_library";
        net::Machine* mc = chatMachine();
        cv.header = games ? "Game library" : "Run on " + (mc ? mc->name : std::string("the machine"));
        std::string cmd = games ? "installed games and play time" + (argOf(c, "sort_by").empty() ? std::string()
                                                                       : ", by " + argOf(c, "sort_by"))
                                : "$ " + (commandOf(c).empty() ? c.args : commandOf(c));
        cv.cmd = text_.layout({{cmd, F_Mono, C_Text}}, inner, true);
        if (const ChatMessage* r = results[k]) {
            if (r->isError) {
                cv.status = "Not run";
            } else if (r->exitCode >= 0) {
                cv.status = "Exit " + std::to_string(r->exitCode);
                cv.statusColor = r->exitCode == 0 ? C_Muted : C_Error;
            } else {
                cv.status = games ? "Done" : "Stopped";
            }
            // first 30 lines; the model still gets everything (up to 16 KB)
            std::string shown;
            int total = 0;
            for (size_t start = 0; start < r->text.size(); total++) {
                size_t e = r->text.find('\n', start);
                if (e == std::string::npos) e = r->text.size();
                if (total < 30) shown += r->text.substr(start, e - start) + "\n";
                start = e + 1;
            }
            int hidden = total - std::min(total, 30);
            while (!shown.empty() && shown.back() == '\n') shown.pop_back();
            cv.out = text_.layout({{shown.empty() ? "(no output)" : shown, F_MonoSmall,
                                    (unsigned char)(shown.empty() ? C_Muted : C_Text)}},
                                  inner, true);
            if (hidden > 0) cv.more = "… " + std::to_string(hidden) + " more lines";
        } else if (c.id == runningCallId_) {
            cv.status = "Running…";
            cv.statusColor = C_Accent;
        } else if (c.id == pendingCallId_) {
            cv.status = "Waiting for approval";
            cv.statusColor = C_Accent;
            cv.pending = true;
        } else {
            cv.status = "Not run";
        }
        cv.h = pad + text_.lineHeight(F_SmallBold) + px(8) + cv.cmd.height + pad;
        if (results[k]) cv.h += px(10) + cv.out.height + (cv.more.empty() ? 0 : text_.lineHeight(F_Small) + px(4));
        if (cv.pending) cv.h += px(10) + px(44);
        v.callsH += cv.h + (k ? px(10) : 0);
        v.calls.push_back(std::move(cv));
    }
}

// Draws the command cards; returns 1 Run, 2 Deny, 3 Always allow, 0 nothing.
int App::drawCalls(MsgView& v, int x, int y, int width) {
    int action = 0;
    const int pad = px(14);
    for (const auto& cv : v.calls) {
        int cardH = cv.h - (cv.pending ? px(54) : 0);
        SDL_Rect card{x, y, width, cardH};
        if (cv.pending)  // waiting for the user: outline it in the accent colour
            gfx_.roundBorder(card, px(12), std::max(2, px(2)), col(C_Accent), col(C_CodeBg));
        else
            gfx_.round(card, px(12), col(C_CodeBg));
        int ty = y + pad;
        text_.draw(F_SmallBold, fit(F_SmallBold, cv.header, width / 2), x + pad, ty, col(C_Muted));
        if (cv.status == "Running…") {  // animated dots, left-aligned so the text doesn't jitter
            int sx = x + width - pad - text_.measure(F_Small, "Running...");
            text_.draw(F_Small, "Running" + std::string(ticks_ / 350 % 4, '.'), sx, ty, col(cv.statusColor));
        } else {
            text_.draw(F_Small, cv.status, x + width - pad - text_.measure(F_Small, cv.status), ty, col(cv.statusColor));
        }
        ty += text_.lineHeight(F_SmallBold) + px(8);
        text_.draw(cv.cmd, x + pad, ty, pc(), 0, H);
        ty += cv.cmd.height;
        if (!cv.out.lines.empty()) {
            ty += px(5);
            gfx_.fill({x + pad, ty, width - 2 * pad, std::max(1, px(1))}, col(C_Border));
            ty += px(5);
            text_.draw(cv.out, x + pad, ty, pc(), 0, H);
            ty += cv.out.height;
            if (!cv.more.empty()) text_.draw(F_Small, cv.more, x + pad, ty + px(4), col(C_Muted));
        }
        if (cv.pending) {
            int by = y + cardH + px(10);
            if (button(ID_TOOL_RUN, {x, by, px(120), px(44)}, "Run", true)) action = 1;
            if (button(ID_TOOL_DENY, {x + px(130), by, px(110), px(44)}, "Deny")) action = 2;
            if (button(ID_TOOL_ALWAYS, {x + px(250), by, px(250), px(44)}, "Always allow in this chat")) action = 3;
        }
        y += cv.h + px(10);
    }
    return action;
}

void App::drawInlineCodeBg(const TextLayout& l, int x, int y) {
    for (const auto& line : l.lines)
        for (const auto& s : line.segs)
            if (s.flags & SpanCodeBg)
                gfx_.round({x + s.x - px(3), y + line.y + line.ascent - text_.ascent(F_Mono) - px(1), s.w + px(6),
                            text_.lineHeight(F_Mono) + px(2)},
                           px(4), col(C_InlineCodeBg));
}

void App::drawMessage(const ChatMessage& m, const MsgView& v, int x, int y, int width, bool streaming, int clipTop,
                      int clipBottom) {
    if (v.user) {
        // attached screenshots, right-aligned like the bubble
        int tw = px(266), th = px(150), tx = x + width;
        for (auto it = m.imagePaths.rbegin(); it != m.imagePaths.rend(); ++it) {
            tx -= tw;
            SDL_Rect r{tx, y, tw, th};
            if (SDL_Texture* t = thumb(*it)) {
                SDL_SetTextureAlphaMod(t, (Uint8)(255 * alpha_));
                SDL_RenderCopy(ren_, t, nullptr, &r);
            } else {
                gfx_.round(r, px(10), col(C_CodeBg));
            }
            tx -= px(8);
        }
        if (m.text.empty()) return;
        y += v.imagesH;
        int padX = px(18), padY = px(12);
        int bw = v.plain.width + 2 * padX;
        SDL_Rect r{x + width - bw, y, bw, v.plain.height + 2 * padY};
        gfx_.round(r, px(metrics::BubbleRadius), col(C_UserBubble));
        text_.draw(v.plain, r.x + padX, r.y + padY, pc(), clipTop, clipBottom);
    } else {
        // Sparkle beside the reply: pulses while waiting for the first token,
        // turns slowly while streaming, still when done.
        float t = ticks_ / 1000.f;
        SDL_Color c = col(C_Accent);
        int size = px(22);
        double angle = 0;
        if (streaming && m.text.empty()) {
            size = px(22 + 5 * std::sin(t * 4.f));
            c.a = (Uint8)(170 + 85 * std::sin(t * 4.f));
            angle = std::fmod(t * 40.0, 360.0);
        } else if (streaming) {
            angle = std::fmod(t * 120.0, 360.0);
        }
        gfx_.sparkle(x - px(30), y + px(12), size, c, angle);

        int by = y;
        for (size_t i = 0; i < v.blocks.size(); i++) {
            const BlockView& bv = v.blocks[i];
            if (by > clipBottom) break;
            if (by + bv.h >= clipTop) {
                for (const auto& p : bv.pieces) {
                    SDL_Rect r{x + p.r.x, by + p.r.y, p.r.w, p.r.h};
                    switch (p.kind) {
                    case Piece::CodeBox:
                        gfx_.round(r, px(metrics::CodeRadius), col(C_CodeBg));
                        if (!p.label.empty()) text_.draw(F_MonoSmall, p.label, r.x + px(14), r.y + px(10), col(C_Muted));
                        break;
                    case Piece::QuoteBar: gfx_.round(r, px(2), col(C_Border)); break;
                    case Piece::Rule: gfx_.fill(r, col(C_Border)); break;
                    case Piece::Text:
                        drawInlineCodeBg(p.text, r.x, r.y);
                        text_.draw(p.text, r.x, r.y, pc(), clipTop, clipBottom);
                        break;
                    }
                }
            }
            by += bv.h;
            if (i + 1 < v.blocks.size()) by += blockGap(bv.block.type, v.blocks[i + 1].block.type);
        }
    }
    if (!m.error.empty()) {
        int ey = y + v.textH + (v.callsH ? (v.textH ? px(12) : 0) + v.callsH : 0);
        if (ey > y) ey += px(10);
        SDL_Rect r{x, ey, v.error.width + px(32), v.error.height + px(18)};
        gfx_.round(r, px(10), col(C_ErrorBg));
        text_.draw(v.error, r.x + px(16), r.y + px(9), pc(), clipTop, clipBottom);
    }
}
