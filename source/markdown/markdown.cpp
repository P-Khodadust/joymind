#include "markdown/markdown.hpp"

#include <cctype>

namespace md {
namespace {

int indentOf(const std::string& l) {
    int n = 0;
    for (char c : l) {
        if (c == ' ')
            n++;
        else if (c == '\t')
            n += 4;
        else
            break;
    }
    return n;
}

std::string ltrim(const std::string& s) {
    size_t i = s.find_first_not_of(" \t");
    return i == std::string::npos ? "" : s.substr(i);
}

std::string rtrim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

// "```lang" or "~~~": returns fence length (>= 3) or 0.
int fenceLen(const std::string& t, char& ch) {
    if (t.size() < 3 || (t[0] != '`' && t[0] != '~')) return 0;
    ch = t[0];
    int n = 0;
    while (n < (int)t.size() && t[n] == ch) n++;
    return n >= 3 ? n : 0;
}

bool isRule(const std::string& t) {
    char ch = 0;
    int count = 0;
    for (char c : t) {
        if (c == ' ' || c == '\t') continue;
        if (c != '-' && c != '*' && c != '_') return false;
        if (ch && c != ch) return false;
        ch = c;
        count++;
    }
    return count >= 3;
}

int headingLevel(const std::string& t) {
    int n = 0;
    while (n < (int)t.size() && t[n] == '#') n++;
    if (n == 0 || n > 6 || (n < (int)t.size() && t[n] != ' ')) return 0;
    return n;
}

// Bullet "- x" / "* x" / "+ x" -> content offset; ordered "12. x" / "12) x" sets marker.
size_t listItem(const std::string& t, bool& ordered, std::string& marker) {
    if (t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
        ordered = false;
        return 2;
    }
    size_t d = 0;
    while (d < t.size() && d < 9 && isdigit((unsigned char)t[d])) d++;
    if (d > 0 && d + 1 < t.size() && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ') {
        ordered = true;
        marker = t.substr(0, d) + ".";
        return d + 2;
    }
    return 0;
}

bool startsBlock(const std::string& t) {
    char ch;
    bool o;
    std::string m;
    return fenceLen(t, ch) || headingLevel(t) || isRule(t) || t[0] == '>' || listItem(t, o, m);
}

}  // namespace

std::vector<Block> parse(const std::string& src) {
    std::vector<std::string> lines;
    size_t s = 0;
    while (s <= src.size()) {
        size_t e = src.find('\n', s);
        if (e == std::string::npos) e = src.size();
        std::string l = src.substr(s, e - s);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        s = e + 1;
    }

    std::vector<Block> out;
    Block para;
    auto flush = [&] {
        if (!para.text.empty()) out.push_back(para);
        para = Block();
    };

    for (size_t i = 0; i < lines.size();) {
        const std::string& line = lines[i];
        std::string t = ltrim(line);
        int indent = indentOf(line);
        char fch;
        int flen = fenceLen(t, fch);

        if (flen) {
            flush();
            Block b;
            b.type = BlockType::Code;
            b.lang = rtrim(ltrim(t.substr(flen)));
            b.open = true;
            bool first = true;
            for (i++; i < lines.size(); i++) {
                std::string ct = ltrim(lines[i]);
                char c2;
                int l2 = fenceLen(ct, c2);
                if (l2 >= flen && c2 == fch && rtrim(ct.substr(l2)).empty()) {
                    b.open = false;
                    i++;
                    break;
                }
                // drop the fence's own indentation from each code line
                std::string cl = lines[i];
                size_t strip = 0;
                while (strip < cl.size() && (int)strip < indent && cl[strip] == ' ') strip++;
                b.text += (first ? "" : "\n") + cl.substr(strip);
                first = false;
            }
            out.push_back(b);
            continue;
        }
        if (t.empty()) {
            flush();
            i++;
            continue;
        }
        if (int h = headingLevel(t)) {
            flush();
            Block b;
            b.type = BlockType::Heading;
            b.level = h > 3 ? 3 : h;
            std::string txt = rtrim(t.substr(h));
            while (!txt.empty() && txt.back() == '#') txt.pop_back();
            b.text = rtrim(ltrim(txt));
            out.push_back(b);
            i++;
            continue;
        }
        if (isRule(t)) {
            flush();
            Block b;
            b.type = BlockType::Rule;
            out.push_back(b);
            i++;
            continue;
        }
        if (t[0] == '>') {
            flush();
            Block b;
            b.type = BlockType::Quote;
            for (; i < lines.size(); i++) {
                std::string q = ltrim(lines[i]);
                if (q.empty() || q[0] != '>') break;
                q = q.substr(1);
                if (!q.empty() && q[0] == ' ') q.erase(0, 1);
                b.text += (b.text.empty() ? "" : "\n") + q;
            }
            out.push_back(b);
            continue;
        }
        bool ordered;
        std::string marker;
        if (size_t off = listItem(t, ordered, marker)) {
            flush();
            Block b;
            b.type = ordered ? BlockType::Ordered : BlockType::Bullet;
            b.marker = marker;
            b.level = indent / 2 > 3 ? 3 : indent / 2;
            b.text = rtrim(t.substr(off));
            // lazy continuation lines belong to the item
            for (i++; i < lines.size(); i++) {
                std::string ct = ltrim(lines[i]);
                if (ct.empty() || startsBlock(ct)) break;
                b.text += "\n" + rtrim(ct);
            }
            out.push_back(b);
            continue;
        }
        // Paragraph: single newlines are kept as line breaks (models use them
        // deliberately for poems, addresses, etc.).
        para.text += (para.text.empty() ? "" : "\n") + rtrim(t);
        i++;
    }
    flush();
    return out;
}

std::vector<Inline> parseInline(const std::string& s) {
    std::vector<Inline> out;
    uint8_t style = 0;
    auto put = [&](const std::string& txt, uint8_t st) {
        if (txt.empty()) return;
        if (!out.empty() && out.back().style == st)
            out.back().text += txt;
        else
            out.push_back({txt, st});
    };
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n'; };
    const size_t n = s.size();

    for (size_t i = 0; i < n;) {
        char c = s[i];
        if (c == '\\' && i + 1 < n && ispunct((unsigned char)s[i + 1])) {
            put(std::string(1, s[i + 1]), style);
            i += 2;
            continue;
        }
        if (c == '`') {
            size_t k = 0;
            while (i + k < n && s[i + k] == '`') k++;
            std::string fence(k, '`');
            size_t close = s.find(fence, i + k);
            while (close != std::string::npos && close + k < n && s[close + k] == '`')
                close = s.find(fence, close + k + 1);
            if (close == std::string::npos) {
                put(fence, style);
                i += k;
                continue;
            }
            std::string code = s.substr(i + k, close - i - k);
            if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ') code = code.substr(1, code.size() - 2);
            put(code, Code);
            i = close + k;
            continue;
        }
        if (c == '*' || c == '_') {
            size_t k = 0;
            while (i + k < n && s[i + k] == c) k++;
            uint8_t bits = k >= 3 ? (Bold | Italic) : k == 2 ? Bold : Italic;
            size_t take = k >= 3 ? 3 : k;
            char prev = i > 0 ? s[i - 1] : ' ', next = i + k < n ? s[i + k] : ' ';
            bool canClose = !isSpace(prev), canOpen = !isSpace(next);
            if (c == '_') {  // no intraword emphasis with underscores (snake_case)
                canOpen = canOpen && !isalnum((unsigned char)prev);
                canClose = canClose && !isalnum((unsigned char)next);
            }
            if (canClose && (style & bits) == bits) {
                style &= ~bits;
                i += take;
                continue;
            }
            if (canOpen && (style & bits) == 0) {
                // only open if a matching closer exists later in the text
                std::string delim(take, c);
                bool found = false;
                for (size_t j = s.find(delim, i + take); j != std::string::npos; j = s.find(delim, j + 1)) {
                    if (!isSpace(s[j - 1])) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    style |= bits;
                    i += take;
                    continue;
                }
            }
            put(std::string(k, c), style);
            i += k;
            continue;
        }
        if (c == '[') {
            size_t rb = s.find(']', i + 1);
            if (rb != std::string::npos && rb + 1 < n && s[rb + 1] == '(') {
                size_t rp = s.find(')', rb + 2);
                if (rp != std::string::npos) {
                    put(s.substr(i + 1, rb - i - 1), style | Link);
                    i = rp + 1;
                    continue;
                }
            }
        }
        // copy a run of ordinary characters at once
        size_t j = i + 1;
        while (j < n && s[j] != '\\' && s[j] != '`' && s[j] != '*' && s[j] != '_' && s[j] != '[') j++;
        put(s.substr(i, j - i), style);
        i = j;
    }
    return out;
}

}  // namespace md
