#include "text/text.hpp"

#include <algorithm>

namespace {
size_t utf8Len(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1; }
}  // namespace

bool Text::open(SDL_Renderer* r, const std::string& fontDir, const std::vector<Font>& fonts) {
    close();
    r_ = r;
    for (const auto& f : fonts) {
        TTF_Font* font = TTF_OpenFont((fontDir + f.file).c_str(), f.px);
        if (!font) {
            SDL_Log("TTF_OpenFont %s: %s", f.file, TTF_GetError());
            return false;
        }
        TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
        fonts_.push_back(font);
        metrics_.push_back({TTF_FontLineSkip(font), TTF_FontAscent(font)});
    }
    return true;
}

void Text::close() {
    clearCache();
    for (auto* f : fonts_) TTF_CloseFont(f);
    fonts_.clear();
    metrics_.clear();
}

void Text::clearCache() {
    for (auto& kv : cache_) SDL_DestroyTexture(kv.second.tex);
    cache_.clear();
    lru_.clear();
    pixels_ = 0;
}

int Text::measure(unsigned char font, const std::string& s) {
    if (s.empty() || font >= fonts_.size()) return 0;
    int w = 0, h = 0;
    TTF_SizeUTF8(fonts_[font], s.c_str(), &w, &h);
    return w;
}

// Largest prefix of s[from..] (on a UTF-8 boundary) that fits in avail pixels.
size_t Text::fitBytes(unsigned char font, const std::string& s, size_t from, int avail) {
    std::vector<size_t> ends;
    for (size_t p = from; p < s.size();) {
        p = std::min(s.size(), p + utf8Len((unsigned char)s[p]));
        ends.push_back(p);
    }
    size_t lo = 0, hi = ends.size();  // number of code points that fit
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (measure(font, s.substr(from, ends[mid - 1] - from)) <= avail)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo ? ends[lo - 1] - from : 0;
}

TextLayout Text::layout(const std::vector<TextSpan>& spans, int maxW, bool preserve) {
    TextLayout L;
    TextLine line;
    int x = 0;
    bool wrapped = false;  // current line started because of a wrap (not '\n')
    unsigned char lastFont = spans.empty() ? 0 : spans[0].font;

    auto newLine = [&](bool byWrap) {
        line.h = lineHeight(lastFont);
        line.ascent = ascent(lastFont);
        // Word widths don't add up exactly to the width of the rendered run
        // (hinting rounds per call), so re-measure each merged segment.
        int sx = 0;
        for (auto& s : line.segs) {
            s.x = sx;
            s.w = measure(s.font, s.text);
            sx += s.w;
        }
        for (const auto& s : line.segs) {
            line.h = std::max(line.h, lineHeight(s.font));
            line.ascent = std::max(line.ascent, ascent(s.font));
            L.width = std::max(L.width, s.x + s.w);
        }
        line.y = L.height;
        L.height += line.h;
        L.lines.push_back(std::move(line));
        line = TextLine();
        x = 0;
        wrapped = byWrap;
    };
    auto add = [&](const TextSpan& sp, const std::string& piece, int w) {
        if (!line.segs.empty()) {
            TextSeg& b = line.segs.back();
            if (b.font == sp.font && b.color == sp.color && b.flags == sp.flags && b.x + b.w == x) {
                b.text += piece;
                b.w += w;
                x += w;
                return;
            }
        }
        line.segs.push_back({piece, x, w, sp.font, sp.color, sp.flags});
        x += w;
    };

    for (const auto& sp : spans) {
        lastFont = sp.font;
        const std::string& s = sp.text;
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == '\n') {
                newLine(false);
                i++;
                continue;
            }
            bool space = s[i] == ' ' || s[i] == '\t';
            size_t j = i;
            while (j < s.size() && s[j] != '\n' && ((s[j] == ' ' || s[j] == '\t') == space)) j++;
            std::string tok = s.substr(i, j - i);
            i = j;
            if (space) {
                if (preserve) std::replace(tok.begin(), tok.end(), '\t', ' ');
                if (!preserve && x == 0 && wrapped) continue;  // no leading space on wrapped lines
                int w = measure(sp.font, tok);
                if (x + w > maxW) {
                    newLine(true);
                    if (!preserve) continue;
                    w = std::min(w, maxW);
                }
                add(sp, tok, w);
                continue;
            }
            int w = measure(sp.font, tok);
            if (x + w <= maxW) {
                add(sp, tok, w);
                continue;
            }
            if (x > 0 && w <= maxW) {
                newLine(true);
                add(sp, tok, w);
                continue;
            }
            // Longer than a whole line (URL, hash, code): break between characters.
            for (size_t k = 0; k < tok.size();) {
                size_t n = fitBytes(sp.font, tok, k, maxW - x);
                if (n == 0) {
                    if (x > 0) {
                        newLine(true);
                        continue;
                    }
                    n = utf8Len((unsigned char)tok[k]);  // always place at least one character
                }
                std::string piece = tok.substr(k, n);
                add(sp, piece, measure(sp.font, piece));
                k += n;
                if (k < tok.size()) newLine(true);
            }
        }
    }
    if (!line.segs.empty() || L.lines.empty()) newLine(false);
    return L;
}

SDL_Texture* Text::texture(unsigned char font, const std::string& s, int& w, int& h) {
    key_.assign(1, (char)font);
    key_ += s;
    auto it = cache_.find(key_);
    if (it != cache_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        w = it->second.w;
        h = it->second.h;
        return it->second.tex;
    }
    // Rendered white; the colour is applied with SDL_SetTextureColorMod so one
    // texture serves every colour and theme.
    SDL_Surface* surf = TTF_RenderUTF8_Blended(fonts_[font], s.c_str(), SDL_Color{255, 255, 255, 255});
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r_, surf);
    w = surf->w;
    h = surf->h;
    SDL_FreeSurface(surf);
    if (!tex) return nullptr;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    lru_.push_front(key_);
    cache_.emplace(key_, Entry{tex, w, h, lru_.begin()});
    pixels_ += (size_t)w * h;
    while (pixels_ > kBudgetPixels && lru_.size() > 1) {
        auto old = cache_.find(lru_.back());
        pixels_ -= (size_t)old->second.w * old->second.h;
        SDL_DestroyTexture(old->second.tex);  // SDL flushes queued draws that still use it
        cache_.erase(old);
        lru_.pop_back();
    }
    return tex;
}

int Text::draw(unsigned char font, const std::string& s, int x, int y, SDL_Color c) {
    if (s.empty() || font >= fonts_.size()) return 0;
    int w, h;
    SDL_Texture* t = texture(font, s, w, h);
    if (!t) return 0;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    SDL_Rect dst{x, y, w, h};
    SDL_RenderCopy(r_, t, nullptr, &dst);
    return w;
}

void Text::draw(const TextLayout& l, int x, int y, const SDL_Color* palette, int clipTop, int clipBottom) {
    for (const auto& line : l.lines) {
        int ly = y + line.y;
        if (ly + line.h < clipTop) continue;
        if (ly >= clipBottom) break;
        for (const auto& s : line.segs)
            draw(s.font, s.text, x + s.x, ly + line.ascent - ascent(s.font), palette[s.color]);
    }
}
