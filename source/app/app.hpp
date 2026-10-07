// Application state, actions and the main loop. Screens are drawn in
// ui/screens.cpp, assistant/user messages in ui/chatview.cpp.
#pragma once
#include <SDL.h>

#include <algorithm>
#include <string>
#include <vector>

#include <map>

#include "app/platform.hpp"
#include "markdown/markdown.hpp"
#include "net/http.hpp"
#include "storage/storage.hpp"
#include "text/text.hpp"
#include "ui/gfx.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#ifndef APP_VERSION_STR
#define APP_VERSION_STR "dev"
#endif

// Focus ids. Lists add their row index to a base id.
enum : uint32_t {
    ID_COMPOSER = 1,
    ID_SEND,
    ID_CHIP,
    ID_MENU,
    ID_NEW,
    ID_SETTINGS,
    ID_BACK,
    ID_NAME,
    ID_THEME,
    ID_FONT,
    ID_DEFAULT,
    ID_ADD_PROFILE,
    ID_QUIT,
    ID_TEST,
    ID_SAVE,
    ID_CANCEL,
    ID_OK,
    ID_TYPE_MODEL,
    ID_CHANGE_MODEL,
    ID_MANAGE,
    ID_SIDEBAR_NEW,
    ID_MACHINE_CHIP,
    ID_NO_MACHINE,
    ID_ADD_MACHINE,
    ID_TOOL_RUN,
    ID_TOOL_DENY,
    ID_TOOL_ALWAYS,
    ID_ATTACH,
    ID_GAMES,
    ID_FIELD = 0x100,     // + field index (profile editor)
    ID_MFIELD = 0x200,    // + field index (machine editor)
    ID_ROW = 0x10000,     // + index (chat list, pickers)
    ID_ROW_DEL = 0x20000, // + index (delete buttons in the chat list)
    ID_CARD = 0x30000,    // + index * 8 + button (provider cards)
    ID_MCARD = 0x40000,   // + index * 8 + button (machine cards)
    ID_ATTACHED = 0x50000,  // + index (screenshots attached to the draft)
};

constexpr int kMaxToolSteps = 25;  // commands in a row before the AI must hand back

class App {
public:
    App(SDL_Window* w, SDL_Renderer* r) : win_(w), ren_(r) {}
    bool init();
    void run();

private:
    enum class Screen { Main, Settings, Editor, MachineEditor };
    enum class Modal { None, Sidebar, ProfilePicker, ModelPicker, PresetPicker, MachinePicker, Confirm, TrustHost, Album };

    // Cached layout of one Markdown block of an assistant message.
    struct Piece {
        enum Kind { Text, CodeBox, QuoteBar, Rule } kind = Text;
        SDL_Rect r{};  // relative to the block's top-left
        TextLayout text;
        std::string label;  // code block language
    };
    struct BlockView {
        md::Block block;
        int h = 0;
        std::vector<Piece> pieces;
    };
    // One command card (a tool call and, once run, its output).
    struct CallView {
        std::string id, header, status, more;
        ColorRole statusColor = C_Muted;
        TextLayout cmd, out;
        int h = 0;
        bool pending = false;  // waiting for Run / Deny
    };
    // Layout cache of one message. Text is re-laid out only when it, the error
    // or the column width changes (while streaming only changed blocks are
    // redone); command cards when their state changes.
    struct MsgView {
        size_t len = (size_t)-1, errLen = 0, callsN = 0;
        int width = -1, textH = 0;
        bool user = false;
        std::vector<BlockView> blocks;  // assistant (Markdown)
        TextLayout plain;               // user text
        TextLayout error;
        std::string callsKey;
        std::vector<CallView> calls;
        int callsH = 0;
        uint32_t born = 0;  // when it appeared (slide-in), 0 = no animation
        int imagesH = 0;    // user: attached screenshots above the bubble
    };

    // ---- frame / environment
    void frame();
    void updateScale();
    void applyTheme();
    void pumpWorker();
    void handleGlobalButtons();

    // ---- actions
    Profile* findProfile(const std::string& id);
    Profile* chatProfile();
    std::string chatModel();
    void newChat();
    void openChat(const std::string& id);
    void send();
    void startChatJob();
    void stop();
    void finishResponse();
    void editDraft();
    void openModels(bool forEditor);
    // agent loop: the model asks to run commands on the chat's SSH machine
    net::Machine* findMachine(const std::string& id);
    net::Machine* chatMachine();
    const ToolCall* findCall(const std::string& id);
    bool hasResult(const std::string& callId);
    void afterAssistantTurn();
    void nextCall();
    void runCall(const ToolCall& c);
    void addToolResult(const ToolCall& c, const std::string& text, int exitCode, bool isError);
    void runGames(const ToolCall& c);
    void handleTyping(Input& in);
    SDL_Texture* thumb(const std::string& path);  // screenshot thumbnail, decoded lazily
    void toast(const std::string& msg);
    void saveConfig();

    // ---- screens (ui/screens.cpp)
    void drawMain();
    void drawTopBar(const std::string& title, bool back);
    void drawHome();
    void drawChat(int top);
    void drawComposer(const SDL_Rect& box, bool home);
    void drawSidebar();
    void drawSettings();
    void drawEditor();
    void drawMachineEditor();
    void drawModal();
    void drawToast();
    bool button(uint32_t id, const SDL_Rect& r, const std::string& label, bool primary = false, bool danger = false);
    bool row(uint32_t id, int x, int y, int w, const std::string& label, const std::string& value,
             Scroller* sc = nullptr, int scrollTop = 0);
    // Accent ring; pass the surrounding colour as `gap` for accent-filled controls.
    void focusRing(const SDL_Rect& r, int radius, ColorRole gap = C_COUNT);
    void badge(int x, int y, const std::string& text, ColorRole fg, ColorRole bg);
    void icon(const char* glyph, const SDL_Rect& r, SDL_Color c);
    std::string fit(unsigned char font, const std::string& s, int maxW);  // first line, ellipsized
    SDL_Rect dialog(const std::string& title, int contentH, int width = 640);
    void drawAlbum();
    int px(float v) const { return (int)(v * S + 0.5f); }
    // Every colour goes through col()/pc(), so setting alpha_ fades whatever is drawn.
    SDL_Color col(ColorRole c) const {
        SDL_Color k = pal_.c[c];
        k.a = (Uint8)(k.a * alpha_);
        return k;
    }
    const SDL_Color* pc() {
        if (alpha_ >= 1.f) return pal_.c;
        for (int i = 0; i < C_COUNT; i++) faded_[i] = col((ColorRole)i);
        return faded_;
    }
    // Ease-out progress 0..1 of an animation that started at `since`.
    float ease(uint32_t since, uint32_t ms) const {
        float t = since ? std::min(1.f, (float)(ticks_ - since) / ms) : 1.f;
        return 1.f - (1.f - t) * (1.f - t) * (1.f - t);
    }

    // ---- messages (ui/chatview.cpp)
    void layoutMessage(const ChatMessage& m, MsgView& v, int width);
    BlockView layoutBlock(const md::Block& b, int width);
    std::vector<TextSpan> inlineSpans(const std::string& text, unsigned char baseFont, unsigned char color);
    void drawMessage(const ChatMessage& m, const MsgView& v, int x, int y, int width, bool streaming, int clipTop,
                     int clipBottom);
    void drawInlineCodeBg(const TextLayout& l, int x, int y);
    int msgHeight(const MsgView& v) const;
    void layoutCalls(size_t msgIndex, MsgView& v, int width);
    int drawCalls(MsgView& v, int x, int y, int width);
    int blockGap(md::BlockType a, md::BlockType b) const;

    SDL_Window* win_;
    SDL_Renderer* ren_;
    Gfx gfx_;
    Text text_;
    Ui ui_;
    Palette pal_ = kLight;
    float alpha_ = 1.f;
    SDL_Color faded_[C_COUNT];
    // animation clocks
    Screen shownScreen_ = Screen::Main;
    Modal shownModal_ = Modal::None;
    uint32_t screenAt_ = 0, modalAt_ = 0, homeAt_ = 0, toastAt_ = 0;
    bool skipMsgAnim_ = true;  // opening a chat shows it at once
    float sendMix_ = 0;        // send button grey -> accent
    float S = 1.f;  // UI scale: output height / 720
    int W = 1280, H = 720;
    int openedFontsFor_ = 0;  // H*10 + font size the fonts were opened for
    bool running_ = true;
    uint32_t ticks_ = 0;

    Config cfg_;
    std::vector<ChatMeta> index_;
    Chat chat_;
    std::vector<MsgView> views_;
    std::string draft_, draftKey_;
    int draftW_ = 0;
    TextLayout draftLayout_;

    net::Worker worker_;
    unsigned chatJob_ = 0, modelsJob_ = 0, testJob_ = 0, commandJob_ = 0, machineTestJob_ = 0, gamesJob_ = 0;
    bool generating_ = false, pendingSend_ = false, followBottom_ = true;

    Screen screen_ = Screen::Main;
    Modal modal_ = Modal::None;
    Scroller chatScroll_, listScroll_, settingsScroll_, editorScroll_, machineScroll_;

    // agent loop
    std::string pendingCallId_, runningCallId_;  // waiting for approval / running now
    bool autoApprove_ = false;                   // "Always allow in this chat" (not saved)
    int toolSteps_ = 0;
    bool loopGames_ = false;  // game tool offered in this loop (fixed per loop: tools must not change mid-loop)
    // screenshots, keyboard, feedback
    std::vector<std::string> draftImages_;
    std::vector<platform::Screenshot> shots_;
    std::map<std::string, SDL_Texture*> thumbs_;
    int thumbBudget_ = 0;           // JPEG decodes allowed this frame
    bool keyboardMode_ = false;     // a physical keyboard was used: Enter sends
    bool wasGenerating_ = false;
    uint32_t genStart_ = 0;         // for "rumble when a long reply finishes"
    // machine editor
    net::Machine editingMachine_;
    int editingMachineIndex_ = -1;  // -1: new machine
    std::string machineStatus_;
    bool machineStatusError_ = false;
    // host key confirmation
    std::string trustFingerprint_;
    bool trustForEditor_ = false;

    // model picker
    bool modelsForEditor_ = false, modelsLoading_ = false;
    std::vector<ModelInfo> models_;
    std::string modelsError_;
    // profile editor
    Profile editing_;
    int editingIndex_ = -1;  // -1: new profile
    std::string editorStatus_;
    bool editorStatusError_ = false;
    // confirm dialog
    std::string confirmText_;
    enum class ConfirmAction { DeleteChat, DeleteProfile, DeleteMachine } confirmAction_ = ConfirmAction::DeleteChat;
    std::string confirmArg_;

    std::string toast_;
    uint32_t toastUntil_ = 0;
};
