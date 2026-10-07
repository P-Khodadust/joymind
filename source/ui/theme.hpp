// All visual constants in one place. Sizes are in 720p units and multiplied by
// the current scale (1.0 handheld, 1.5 docked at 1080p).
#pragma once
#include <SDL.h>

enum ColorRole : unsigned char {
    C_Bg,
    C_Surface,
    C_UserBubble,
    C_Text,
    C_Muted,
    C_Border,
    C_Accent,
    C_OnAccent,
    C_CodeBg,
    C_InlineCodeBg,
    C_Error,
    C_ErrorBg,
    C_Warn,
    C_WarnBg,
    C_Hover,
    C_Scrim,
    C_COUNT
};

struct Palette {
    SDL_Color c[C_COUNT];
};

constexpr SDL_Color rgb(unsigned hex, unsigned char a = 255) {
    return {(unsigned char)(hex >> 16), (unsigned char)(hex >> 8), (unsigned char)hex, a};
}

constexpr Palette kLight = {{
    rgb(0xF5F4EE),      // C_Bg
    rgb(0xFAF9F5),      // C_Surface
    rgb(0xE9E6DC),      // C_UserBubble (slightly darker surface)
    rgb(0x1F1E1D),      // C_Text
    rgb(0x73726C),      // C_Muted
    rgb(0xE5E3DA),      // C_Border
    rgb(0xD97757),      // C_Accent
    rgb(0xFFFFFF),      // C_OnAccent
    rgb(0xECEAE2),      // C_CodeBg
    rgb(0xE8E5DC),      // C_InlineCodeBg
    rgb(0xB3261E),      // C_Error
    rgb(0xF9E3E0),      // C_ErrorBg
    rgb(0x8A5A12),      // C_Warn
    rgb(0xFBEBD0),      // C_WarnBg
    rgb(0xEDEBE3),      // C_Hover (focused rows)
    rgb(0x000000, 70),  // C_Scrim
}};

constexpr Palette kDark = {{
    rgb(0x262624),       // C_Bg
    rgb(0x30302E),       // C_Surface
    rgb(0x1C1C1A),       // C_UserBubble
    rgb(0xECEBE4),       // C_Text
    rgb(0x9C9A92),       // C_Muted
    rgb(0x45443F),       // C_Border
    rgb(0xD97757),       // C_Accent
    rgb(0xFFFFFF),       // C_OnAccent
    rgb(0x1E1E1C),       // C_CodeBg
    rgb(0x3A3A37),       // C_InlineCodeBg
    rgb(0xF2B8B5),       // C_Error
    rgb(0x3D2422),       // C_ErrorBg
    rgb(0xF0C27B),       // C_Warn
    rgb(0x3A2E1B),       // C_WarnBg
    rgb(0x3A3A37),       // C_Hover
    rgb(0x000000, 120),  // C_Scrim
}};

namespace metrics {
constexpr int TopBarH = 60;
constexpr int ColumnW = 760;     // chat column
constexpr int ComposerW = 780;
constexpr int Gutter = 24;
constexpr int Radius = 18;       // composer, cards
constexpr int BubbleRadius = 18;
constexpr int CodeRadius = 10;
constexpr int SidebarW = 360;
constexpr int RowH = 52;
constexpr int FocusRing = 3;
}  // namespace metrics

// Font roles (pixel sizes at 720p, before the font-size setting).
enum FontId : unsigned char {
    F_Body,
    F_Bold,
    F_Italic,
    F_BoldItalic,
    F_Small,
    F_SmallBold,
    F_Mono,
    F_MonoSmall,
    F_Greeting,
    F_H1,
    F_H2,
    F_H3,
    F_Icon,
    F_COUNT
};

struct FontSpec {
    const char* file;
    int px;
    bool userScaled;  // follows the font-size setting
};

constexpr FontSpec kFonts[F_COUNT] = {
    {"Inter-Regular.ttf", 19, true},           // F_Body
    {"Inter-Bold.ttf", 19, true},              // F_Bold
    {"Inter-Italic.ttf", 19, true},            // F_Italic
    {"Inter-BoldItalic.ttf", 19, true},        // F_BoldItalic
    {"Inter-Regular.ttf", 15, false},          // F_Small
    {"Inter-Bold.ttf", 15, false},             // F_SmallBold
    {"JetBrainsMono-Regular.ttf", 16, true},   // F_Mono
    {"JetBrainsMono-Regular.ttf", 13, false},  // F_MonoSmall
    {"SourceSerif4-Regular.ttf", 40, false},   // F_Greeting
    {"SourceSerif4-Semibold.ttf", 28, true},   // F_H1
    {"SourceSerif4-Semibold.ttf", 24, true},   // F_H2
    {"SourceSerif4-Semibold.ttf", 21, true},   // F_H3
    {"Inter-Bold.ttf", 22, false},             // F_Icon (arrows and symbols)
};

constexpr float kFontScales[3] = {0.88f, 1.0f, 1.15f};  // small / medium / large
