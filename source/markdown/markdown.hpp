// Small Markdown parser for assistant messages. Pure string processing, no
// rendering: the UI turns blocks + inline spans into laid-out text.
// Re-parsing a growing (streaming) message is cheap, and an unclosed code
// fence simply yields a Code block with open = true.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace md {

enum Style : uint8_t { Bold = 1, Italic = 2, Code = 4, Link = 8 };

struct Inline {
    std::string text;
    uint8_t style = 0;
};

enum class BlockType { Paragraph, Heading, Code, Bullet, Ordered, Quote, Rule };

struct Block {
    BlockType type = BlockType::Paragraph;
    int level = 0;       // heading level 1-3, or list nesting depth 0-3
    std::string marker;  // "3." for ordered list items
    std::string lang;    // fenced code language label
    std::string text;    // inline markdown, or raw code for Code blocks
    bool open = false;   // code fence not closed (yet)
};

std::vector<Block> parse(const std::string& src);
std::vector<Inline> parseInline(const std::string& text);

}  // namespace md
