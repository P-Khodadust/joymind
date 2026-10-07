// Text layer: measure / layout / draw. The UI only uses this interface, so
// complex-script shaping (HarfBuzz + FriBidi for RTL) can be added in here later
// without touching UI code. Today it does Latin text: word wrap, breaking of
// over-long words (URLs, code), and an LRU cache of rendered text textures.
#pragma once
#include <SDL.h>
#include <SDL_ttf.h>

#include <list>
#include <string>
#include <unordered_map>
#include <vector>

struct TextSpan {
    std::string text;
    unsigned char font = 0, color = 0, flags = 0;
};
enum : unsigned char { SpanCodeBg = 1 };  // inline code: UI draws a pill behind it

struct TextSeg {
    std::string text;
    int x = 0, w = 0;
    unsigned char font = 0, color = 0, flags = 0;
};

struct TextLine {
    int y = 0, h = 0, ascent = 0;
    std::vector<TextSeg> segs;
};

struct TextLayout {
    std::vector<TextLine> lines;
    int width = 0, height = 0;
};

class Text {
public:
    struct Font {
        const char* file;
        int px;
    };
    ~Text() { close(); }
    // (Re)opens every font at the given pixel sizes and clears the cache.
    bool open(SDL_Renderer* r, const std::string& fontDir, const std::vector<Font>& fonts);
    void close();

    int measure(unsigned char font, const std::string& s);
    int lineHeight(unsigned char font) const { return metrics_[font].skip; }
    int ascent(unsigned char font) const { return metrics_[font].ascent; }

    // Wraps spans into lines no wider than maxWidth. preserveSpaces keeps
    // leading/trailing whitespace (code blocks).
    TextLayout layout(const std::vector<TextSpan>& spans, int maxWidth, bool preserveSpaces = false);

    // Draws one run with its top-left at (x, y); returns its width.
    int draw(unsigned char font, const std::string& s, int x, int y, SDL_Color c);
    // Draws a layout; lines outside [clipTop, clipBottom) are skipped.
    void draw(const TextLayout& l, int x, int y, const SDL_Color* palette, int clipTop, int clipBottom);

    void clearCache();

private:
    struct Entry {
        SDL_Texture* tex;
        int w, h;
        std::list<std::string>::iterator lru;
    };
    struct Metrics {
        int skip = 0, ascent = 0;
    };
    SDL_Texture* texture(unsigned char font, const std::string& s, int& w, int& h);
    size_t fitBytes(unsigned char font, const std::string& s, size_t from, int avail);

    SDL_Renderer* r_ = nullptr;
    std::vector<TTF_Font*> fonts_;
    std::vector<Metrics> metrics_;
    std::unordered_map<std::string, Entry> cache_;
    std::list<std::string> lru_;  // front = most recently used
    size_t pixels_ = 0;
    std::string key_;  // reused lookup buffer: no allocation on cache hits
    // ponytail: fixed texture budget (~24 MB RGBA); make it adaptive if applet mode runs short.
    static constexpr size_t kBudgetPixels = 6u << 20;
};
