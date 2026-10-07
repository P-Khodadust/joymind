#include "app/app.hpp"

#include <algorithm>
#include <ctime>

#include <SDL_image.h>

#include "app/platform.hpp"

namespace {
ChatMessage chatMsg(const char* role, const std::string& text) {
    ChatMessage m;
    m.role = role;
    m.text = text;
    return m;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
}  // namespace

bool App::init() {
    std::string notice;
    storage::init();
    cfg_ = storage::loadConfig(notice);
    index_ = storage::loadIndex();
    if (!gfx_.init(ren_)) return false;
    applyTheme();
    updateScale();
    if (openedFontsFor_ == 0) return false;
    newChat();
    if (!notice.empty()) toast(notice);
    return true;
}

void App::run() {
    while (running_) {
        Input in;
        if (!platform::poll(in, W, H)) break;
        updateScale();
        ticks_ = SDL_GetTicks();
        thumbBudget_ = 1;
        handleTyping(in);
        ui_.begin(in, S);
        pumpWorker();
        if (pendingSend_ && !worker_.busy()) startChatJob();
        handleGlobalButtons();
        frame();
        ui_.end();
        SDL_RenderPresent(ren_);
        if (generating_ != wasGenerating_) {
            wasGenerating_ = generating_;
            platform::keepAwake(generating_);  // long replies shouldn't let the screen dim
        }
    }
    if (generating_) stop();
}

// Docked (1080p) vs handheld (720p): the SDL Switch port resizes the window on
// operation-mode changes; we follow the renderer size and reopen the fonts at
// the new pixel size so text stays sharp instead of being scaled.
void App::updateScale() {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren_, &w, &h);
    if (w <= 0 || h <= 0) return;
    int key = h * 10 + cfg_.fontSize + 1;
    if (key == openedFontsFor_ && w == W) return;
    W = w;
    H = h;
    S = h / 720.f;
    if (key != openedFontsFor_) {
        std::vector<Text::Font> fonts;
        for (const auto& f : kFonts) fonts.push_back({f.file, px(f.px * (f.userScaled ? kFontScales[cfg_.fontSize] : 1.f))});
        openedFontsFor_ = text_.open(ren_, platform::assetDir() + "fonts/", fonts) ? key : 0;
    }
    views_.clear();
    skipMsgAnim_ = true;
    chatScroll_.pos = chatScroll_.goal = 1e9f;  // keep the newest messages in view after a resize
}

void App::applyTheme() {
    bool dark = cfg_.theme == "dark" || (cfg_.theme == "auto" && platform::systemDarkTheme());
    pal_ = dark ? kDark : kLight;
}

void App::frame() {
    const SDL_Color& bg = col(C_Bg);
    SDL_SetRenderDrawColor(ren_, bg.r, bg.g, bg.b, 255);
    SDL_RenderClear(ren_);
    if (screen_ != shownScreen_) {
        shownScreen_ = screen_;
        screenAt_ = ticks_;
    }
    switch (screen_) {
    case Screen::Main: drawMain(); break;
    case Screen::Settings: drawSettings(); break;
    case Screen::Editor: drawEditor(); break;
    case Screen::MachineEditor: drawMachineEditor(); break;
    }
    float sp = ease(screenAt_, 180);  // new screens fade in from the background colour
    if (sp < 1.f) {
        SDL_Color c = col(C_Bg);
        c.a = (Uint8)(255 * (1.f - sp));
        gfx_.fill({0, 0, W, H}, c);
    }
    if (modal_ != shownModal_) {
        // back from a confirm dialog to the chat list: don't slide it in again
        bool reopen = modal_ == Modal::Sidebar && shownModal_ == Modal::Confirm;
        shownModal_ = modal_;
        modalAt_ = reopen ? 0 : ticks_;
    }
    if (modal_ != Modal::None) {
        ui_.layer = 1;
        ui_.clip = {0, 0, W, H};
        drawModal();
        ui_.layer = 0;
    }
    alpha_ = 1.f;
    drawToast();
}

void App::handleGlobalButtons() {
    if (modal_ != Modal::None) {
        if (ui_.pressed(BtnB) || (modal_ == Modal::Sidebar && ui_.pressed(BtnMinus))) modal_ = Modal::None;
        return;
    }
    bool editor = screen_ == Screen::Editor || screen_ == Screen::MachineEditor;
    if (ui_.pressed(BtnB)) {
        if (editor)
            screen_ = Screen::Settings;
        else if (screen_ == Screen::Settings)
            screen_ = Screen::Main;
    }
    if (ui_.pressed(BtnPlus) && !editor) screen_ = screen_ == Screen::Main ? Screen::Settings : Screen::Main;
    if (ui_.pressed(BtnMinus) && !editor) {
        screen_ = Screen::Main;
        modal_ = Modal::Sidebar;
        listScroll_.pos = 0;
    }
    if (ui_.pressed(BtnX) && screen_ == Screen::Main) newChat();
    if (ui_.pressed(BtnY) && screen_ == Screen::Main) editDraft();
}

// ---------------------------------------------------------------- network events

void App::pumpWorker() {
    net::Event e;
    while (worker_.poll(e)) {
        if (chatJob_ && e.job == chatJob_ && !chat_.messages.empty()) {
            ChatMessage& m = chat_.messages.back();
            if (e.type == net::Event::TextDelta) {
                m.text += e.text;
            } else {
                chatJob_ = 0;
                m.calls = e.calls;
                m.native = e.native;
                if (e.type == net::Event::Error) m.error = e.text;
                if (e.type == net::Event::Done && !m.calls.empty())
                    afterAssistantTurn();
                else
                    finishResponse();
            }
        } else if (commandJob_ && e.job == commandJob_) {
            commandJob_ = 0;
            std::string id = runningCallId_;
            runningCallId_.clear();
            const ToolCall* found = findCall(id);
            if (!found) continue;
            ToolCall c = *found;
            if (e.type == net::Event::CommandDone) {
                addToolResult(c, e.text, e.exitCode, false);
                nextCall();
            } else if (e.type == net::Event::HostKeyUnknown) {
                // First contact with this server: ask before trusting its key.
                pendingCallId_ = id;
                trustFingerprint_ = e.text;
                trustForEditor_ = false;
                modal_ = Modal::TrustHost;
            } else {
                // SSH itself failed (login, network, ...): record it and hand back to the user.
                addToolResult(c, "Couldn't run the command: " + e.text, -1, true);
                for (auto it = chat_.messages.rbegin(); it != chat_.messages.rend(); ++it)
                    if (it->role == "assistant") {
                        it->error = e.text;
                        break;
                    }
                finishResponse();
            }
        } else if (gamesJob_ && e.job == gamesJob_) {
            gamesJob_ = 0;
            std::string id = runningCallId_;
            runningCallId_.clear();
            if (const ToolCall* c = findCall(id)) {
                ToolCall call = *c;
                addToolResult(call, e.text, -1, false);
                nextCall();
            }
        } else if (machineTestJob_ && e.job == machineTestJob_) {
            machineTestJob_ = 0;
            machineStatusError_ = e.type == net::Event::Error;
            if (e.type == net::Event::CommandDone) {
                machineStatus_ = "Connected. " + e.text;
            } else if (e.type == net::Event::HostKeyUnknown) {
                machineStatus_.clear();
                trustFingerprint_ = e.text;
                trustForEditor_ = true;
                modal_ = Modal::TrustHost;
            } else {
                machineStatus_ = e.text;
            }
        } else if (modelsJob_ && e.job == modelsJob_) {
            modelsJob_ = 0;
            modelsLoading_ = false;
            if (e.type == net::Event::Models)
                models_ = e.models;
            else
                modelsError_ = e.text;
        } else if (testJob_ && e.job == testJob_) {
            testJob_ = 0;
            editorStatus_ = e.text;
            editorStatusError_ = e.type == net::Event::Error;
        }
        // anything else belongs to a job that was cancelled: ignore it
    }
}

// ---------------------------------------------------------------- actions

Profile* App::findProfile(const std::string& id) {
    for (auto& p : cfg_.profiles)
        if (p.id == id) return &p;
    return nullptr;
}

Profile* App::chatProfile() {
    if (Profile* p = findProfile(chat_.profileId)) return p;
    if (Profile* p = findProfile(cfg_.defaultProfile)) return p;
    return cfg_.profiles.empty() ? nullptr : &cfg_.profiles[0];
}

std::string App::chatModel() {
    if (!chat_.model.empty()) return chat_.model;
    Profile* p = chatProfile();
    return p ? p->model : "";
}

void App::newChat() {
    if (generating_) stop();
    chat_ = Chat();
    if (Profile* p = chatProfile()) {
        chat_.profileId = p->id;
        chat_.model = p->model;
    }
    if (findMachine(cfg_.defaultMachine)) chat_.machineId = cfg_.defaultMachine;
    autoApprove_ = false;
    views_.clear();
    chatScroll_ = Scroller();
    followBottom_ = true;
    screen_ = Screen::Main;
    modal_ = Modal::None;
    ui_.setFocus(ID_COMPOSER);
}

void App::openChat(const std::string& id) {
    if (generating_) stop();
    Chat c;
    if (!storage::loadChat(id, c)) {
        storage::deleteChat(id, index_);
        toast("That chat could not be read and was removed from the list.");
        return;
    }
    chat_ = std::move(c);
    autoApprove_ = false;
    views_.clear();
    skipMsgAnim_ = true;
    chatScroll_ = Scroller();
    chatScroll_.pos = chatScroll_.goal = 1e9f;
    followBottom_ = true;
    screen_ = Screen::Main;
    modal_ = Modal::None;
    ui_.setFocus(ID_COMPOSER);
}

void App::editDraft() {
    std::string out;
    if (!platform::keyboard("Message", draft_, out, true, 1000)) return;
    draft_ = out;
    if (!trim(draft_).empty()) ui_.setFocus(ID_SEND);
}

void App::send() {
    if (generating_ && pendingCallId_.empty()) return;
    if (generating_) stop();  // replying instead of approving: the pending command is not run
    if (trim(draft_).empty() && draftImages_.empty()) return editDraft();
    Profile* p = chatProfile();
    if (!p) {
        toast("Add a provider in Settings first.");
        screen_ = Screen::Settings;
        return;
    }
    if (chat_.id.empty()) {
        chat_.id = storage::newId();
        chat_.created = (int64_t)time(nullptr);
    }
    chat_.profileId = p->id;
    if (chat_.model.empty()) chat_.model = p->model;
    if (chat_.title.empty()) chat_.title = storage::makeTitle(trim(draft_).empty() ? "Screenshot" : trim(draft_));
    ChatMessage um = chatMsg("user", trim(draft_));
    um.imagePaths = draftImages_;
    chat_.messages.push_back(um);
    chat_.messages.push_back(chatMsg("assistant", ""));
    draft_.clear();
    draftImages_.clear();
    toolSteps_ = 0;
    loopGames_ = cfg_.shareGames;
    genStart_ = ticks_;
    generating_ = true;
    followBottom_ = true;
    ui_.setFocus(ID_COMPOSER);
    startChatJob();
}

void App::startChatJob() {
    pendingSend_ = false;
    Profile* p = chatProfile();
    if (!p || chat_.messages.empty()) return;
    Profile req = *p;
    req.model = chatModel();
    if (req.model.empty()) {
        chat_.messages.back().error = "Choose a model first: tap the model chip in the message box.";
        return finishResponse();
    }
    // With a machine attached the model gets the run_command tool.
    std::vector<ToolSpec> tools;
    if (loopGames_) tools.push_back(gameLibraryTool());
    if (net::Machine* m = chatMachine()) {
        tools.push_back(runCommandTool());
        std::string agent = "You can run shell commands on the user's machine \"" + m->name + "\" (" + m->user + "@" +
                            m->host + ") with the run_command tool. The user approves each command before it runs. "
                            "Commands run without a terminal: avoid interactive programs (editors, pagers, top) and "
                            "pass non-interactive flags such as -y. Keep output short (head, tail, grep). Say briefly "
                            "what you are about to do before running a command.";
        req.systemPrompt = req.systemPrompt.empty() ? agent : agent + "\n\n" + req.systemPrompt;
    }
    // Neutral history (slicing ChatMessage to Message); each provider converts it in buildRequest().
    std::vector<Message> hist(chat_.messages.begin(), chat_.messages.end() - 1);
    unsigned job = worker_.startChat(req, hist, tools);
    if (job == 0) {
        pendingSend_ = true;  // a cancelled job is still winding down; retry next frame
        return;
    }
    chatJob_ = job;
}

void App::stop() {
    if (!generating_) return;
    worker_.cancel();
    chatJob_ = commandJob_ = gamesJob_ = 0;  // late events from the cancelled job are ignored
    pendingSend_ = false;
    pendingCallId_.clear();  // unanswered calls are sent later as "not run"
    runningCallId_.clear();
    if (modal_ == Modal::TrustHost && !trustForEditor_) modal_ = Modal::None;
    const ChatMessage* last = chat_.messages.empty() ? nullptr : &chat_.messages.back();
    if (last && last->role == "assistant" && last->text.empty() && last->calls.empty()) chat_.messages.pop_back();
    finishResponse();
}

void App::finishResponse() {
    generating_ = false;
    if (genStart_ && ticks_ - genStart_ > 8000) platform::rumble();  // you may have looked away
    genStart_ = 0;
    chat_.updated = (int64_t)time(nullptr);
    if (!chat_.id.empty() && !storage::saveChat(chat_, index_)) toast("Couldn't save the chat to the SD card.");
}

void App::openModels(bool forEditor) {
    const Profile* p = forEditor ? &editing_ : chatProfile();
    if (!p) return;
    modelsForEditor_ = forEditor;
    models_.clear();
    modelsError_.clear();
    modal_ = Modal::ModelPicker;
    listScroll_ = Scroller();
    modelsJob_ = worker_.startModels(*p);
    modelsLoading_ = modelsJob_ != 0;
    if (!modelsJob_) modelsError_ = "Busy with another request. You can still type a model name.";
}

// ---------------------------------------------------------------- agent loop

net::Machine* App::findMachine(const std::string& id) {
    for (auto& m : cfg_.machines)
        if (m.id == id) return &m;
    return nullptr;
}

net::Machine* App::chatMachine() { return chat_.machineId.empty() ? nullptr : findMachine(chat_.machineId); }

const ToolCall* App::findCall(const std::string& id) {
    for (auto it = chat_.messages.rbegin(); it != chat_.messages.rend(); ++it)
        for (const auto& c : it->calls)
            if (c.id == id) return &c;
    return nullptr;
}

bool App::hasResult(const std::string& callId) {
    for (auto it = chat_.messages.rbegin(); it != chat_.messages.rend() && it->role != "user"; ++it)
        if (it->role == "tool" && it->callId == callId) return true;
    return false;
}

void App::afterAssistantTurn() {
    chat_.updated = (int64_t)time(nullptr);
    storage::saveChat(chat_, index_);  // keep progress if the console sleeps mid-loop
    if (++toolSteps_ > kMaxToolSteps) {
        chat_.messages.back().error =
            "Paused after " + std::to_string(kMaxToolSteps) + " commands in a row. Send a message to continue.";
        return finishResponse();
    }
    nextCall();
}

// Next unanswered call of the latest assistant turn: ask for approval (or run
// it if this chat is on "always allow"). When all are answered, the model
// gets the results and continues.
void App::nextCall() {
    for (auto it = chat_.messages.rbegin(); it != chat_.messages.rend(); ++it) {
        if (it->role != "assistant") continue;
        for (const auto& c : it->calls) {
            if (hasResult(c.id)) continue;
            if (c.name == "get_game_library") return runGames(c);  // read-only: no approval needed
            if (autoApprove_) return runCall(c);
            pendingCallId_ = c.id;
            followBottom_ = true;
            ui_.setFocus(ID_TOOL_RUN);
            if (genStart_ && ticks_ - genStart_ > 8000) platform::rumble();
            return;
        }
        break;
    }
    chat_.messages.push_back(chatMsg("assistant", ""));
    followBottom_ = true;
    startChatJob();
}

void App::runCall(const ToolCall& call) {
    ToolCall c = call;  // the message vector may grow below
    pendingCallId_.clear();
    net::Machine* m = chatMachine();
    std::string cmd = commandOf(c);
    if (!m || cmd.empty()) {
        addToolResult(c, m ? "Invalid arguments: expected {\"command\": \"...\"}." : "No machine is attached to this chat.",
                      -1, true);
        return nextCall();
    }
    commandJob_ = worker_.startCommand(*m, cmd);
    if (!commandJob_) {
        addToolResult(c, "Couldn't start the command: the app is busy.", -1, true);
        return finishResponse();
    }
    runningCallId_ = c.id;
    followBottom_ = true;
    genStart_ = ticks_;
}

void App::runGames(const ToolCall& call) {
    ToolCall c = call;
    std::string sort = argOf(c, "sort_by");
    gamesJob_ = worker_.startTask([sort] { return platform::gameLibrary(sort); });
    if (!gamesJob_) {
        addToolResult(c, "Couldn't read the game library: the app is busy.", -1, true);
        return nextCall();
    }
    runningCallId_ = c.id;
}

// A USB keyboard (docked) types straight into the message box: Enter sends,
// Shift+Enter adds a line, Backspace deletes. Only on the chat screen.
void App::handleTyping(Input& in) {
    if (screen_ != Screen::Main || modal_ != Modal::None) return;
    if (!in.text.empty() || in.backspaces) {
        keyboardMode_ = true;
        draft_ += in.text;
        for (int i = 0; i < in.backspaces && !draft_.empty(); i++) {
            size_t n = draft_.size() - 1;
            while (n > 0 && ((unsigned char)draft_[n] & 0xC0) == 0x80) n--;  // whole UTF-8 character
            draft_.erase(n);
        }
        ui_.setFocus(ID_COMPOSER);
    }
    // Enter writes into the message box only when that's where you are; on a
    // button (Run, Deny, a chip...) it presses it like A.
    if (in.enterKey && keyboardMode_ && (ui_.hasFocus(ID_COMPOSER) || !draft_.empty())) {
        in.down &= ~BtnA;  // don't also open the on-screen keyboard
        if (in.shift)
            draft_ += '\n';
        else if (!trim(draft_).empty() || !draftImages_.empty())
            send();
    }
}

SDL_Texture* App::thumb(const std::string& path) {
    auto it = thumbs_.find(path);
    if (it != thumbs_.end()) return it->second;
    if (thumbBudget_ <= 0) return nullptr;  // one JPEG decode per frame keeps scrolling smooth
    thumbBudget_--;
    if (thumbs_.size() >= 40) {  // bounded texture memory (~9 MB)
        for (auto& kv : thumbs_)
            if (kv.second) SDL_DestroyTexture(kv.second);
        thumbs_.clear();
    }
    SDL_Texture* tex = nullptr;
    SDL_Surface* loaded = IMG_Load(path.c_str());
    SDL_Surface* src = loaded ? SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ABGR8888, 0) : nullptr;
    if (loaded) SDL_FreeSurface(loaded);
    if (src) {
        // box-filter down to about 320 px wide (screenshots are 1280x720)
        int f = std::max(1, src->w / 320), w = src->w / f, h = src->h / f;
        if (SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888)) {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    unsigned sum[4] = {0, 0, 0, 0};
                    for (int dy = 0; dy < f; dy++) {
                        const Uint8* p = (const Uint8*)src->pixels + (y * f + dy) * src->pitch + x * f * 4;
                        for (int dx = 0; dx < f * 4; dx++) sum[dx & 3] += p[dx];
                    }
                    Uint8* q = (Uint8*)dst->pixels + y * dst->pitch + x * 4;
                    for (int k = 0; k < 4; k++) q[k] = (Uint8)(sum[k] / (f * f));
                }
            tex = SDL_CreateTextureFromSurface(ren_, dst);
            if (tex) SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
            SDL_FreeSurface(dst);
        }
        SDL_FreeSurface(src);
    }
    thumbs_[path] = tex;  // a failed decode is remembered too
    return tex;
}

void App::addToolResult(const ToolCall& c, const std::string& text, int exitCode, bool isError) {
    ChatMessage t = chatMsg("tool", text);
    t.callId = c.id;
    t.toolName = c.name;
    t.exitCode = exitCode;
    t.isError = isError;
    chat_.messages.push_back(t);
}

void App::toast(const std::string& msg) {
    toast_ = msg;
    toastAt_ = ticks_;
    toastUntil_ = SDL_GetTicks() + 3500;
}

void App::saveConfig() {
    if (!storage::saveConfig(cfg_)) toast("Couldn't save settings to the SD card.");
}
