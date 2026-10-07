#include "app/platform.hpp"

#include <SDL.h>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#ifdef __SWITCH__
#include <arpa/inet.h>  // struct in_addr for __nxlink_host
#include <switch.h>
#include <unistd.h>
#endif

namespace platform {
namespace {
SDL_FingerID g_finger = -1;
bool g_touch = false;
int g_tx = 0, g_ty = 0;
uint32_t g_keys = 0;  // pad buttons held via a keyboard
#ifdef __SWITCH__
PadState g_pad;
int g_nxlinkSock = -1;
HidVibrationDeviceHandle g_vib[6];
int g_vibCount = 0;
int g_rumbleStep = -1;
uint32_t g_rumbleAt = 0;

void vibrate(bool on) {
    HidVibrationValue v[6];
    for (int i = 0; i < g_vibCount; i++)
        v[i] = on ? HidVibrationValue{0.45f, 160.f, 0.45f, 320.f} : HidVibrationValue{0.f, 160.f, 0.f, 320.f};
    if (g_vibCount) hidSendVibrationValues(g_vib, v, g_vibCount);
}
#endif

// US layout: the character a key produces, or 0 for non-printing keys.
char typedChar(SDL_Keycode k, Uint16 mod) {
    if (k < 32 || k > 126) return 0;
    bool shift = mod & KMOD_SHIFT;
    if (k >= 'a' && k <= 'z') return (char)(shift != bool(mod & KMOD_CAPS) ? k - 32 : k);
    static const char plain[] = "`1234567890-=[]\\;',./", shifted[] = "~!@#$%^&*()_+{}|:\"<>?";
    const char* p = shift ? strchr(plain, (int)k) : nullptr;
    return p ? shifted[p - plain] : (char)k;
}

void handleEvent(const SDL_Event& e, Input& in, int w, int h, bool& quit) {
    switch (e.type) {
    case SDL_QUIT:
        quit = true;
        break;
    case SDL_FINGERDOWN:
        if (g_finger == -1) {
            g_finger = e.tfinger.fingerId;
            g_touch = true;
            g_tx = (int)(e.tfinger.x * w);
            g_ty = (int)(e.tfinger.y * h);
        }
        break;
    case SDL_FINGERMOTION:
        if (e.tfinger.fingerId == g_finger) {
            g_tx = (int)(e.tfinger.x * w);
            g_ty = (int)(e.tfinger.y * h);
        }
        break;
    case SDL_FINGERUP:
        if (e.tfinger.fingerId == g_finger) {
            g_finger = -1;
            g_touch = false;
        }
        break;
    // Mouse acts as touch on desktop; mouse events synthesized from touch are skipped.
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT) {
            g_touch = e.type == SDL_MOUSEBUTTONDOWN;
            g_tx = e.button.x;
            g_ty = e.button.y;
        }
        break;
    case SDL_MOUSEMOTION:
        if (e.motion.which != SDL_TOUCH_MOUSEID && g_touch) {
            g_tx = e.motion.x;
            g_ty = e.motion.y;
        }
        break;
    case SDL_MOUSEWHEEL:
        in.scroll -= e.wheel.y * 3.0f;
        break;
    // A USB keyboard (Switch, docked) or the PC keyboard: printable keys type
    // text, the rest act as controller buttons.
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        SDL_Keycode k = e.key.keysym.sym;
        uint32_t b = 0;
        switch (k) {
        case SDLK_UP: b = BtnUp; break;
        case SDLK_DOWN: b = BtnDown; break;
        case SDLK_LEFT: b = BtnLeft; break;
        case SDLK_RIGHT: b = BtnRight; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: b = BtnA; break;
        case SDLK_ESCAPE: b = BtnB; break;
        case SDLK_PAGEUP: b = BtnL; break;
        case SDLK_PAGEDOWN: b = BtnR; break;
        case SDLK_F1: b = BtnPlus; break;
        case SDLK_F2: b = BtnMinus; break;
        case SDLK_F3: b = BtnX; break;
        case SDLK_F4: b = BtnY; break;
        default: break;
        }
        if (e.type == SDL_KEYDOWN) {
            if (b && !(g_keys & b)) in.down |= b;
            g_keys |= b;
            if (k == SDLK_BACKSPACE) in.backspaces++;
            if (b == BtnA) {
                in.enterKey = true;
                in.shift = e.key.keysym.mod & KMOD_SHIFT;
            }
            if (char c = typedChar(k, e.key.keysym.mod)) in.text += c;
        } else {
            g_keys &= ~b;
        }
        break;
    }
    default:
        break;
    }
}
}  // namespace

void init() {
#ifdef __SWITCH__
    romfsInit();
    socketInitializeDefault();
    // When launched with `nxlink -s`, send stdout/stderr to the PC.
    if (__nxlink_host.s_addr != 0) g_nxlinkSock = nxlinkStdio();
    nifmInitialize(NifmServiceType_User);
    csrngInitialize();  // entropy for mbedtls (SSH key exchange)
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    nsInitialize();
    pdmqryInitialize();
    // vibration handles for handheld, Joy-Con pair and Pro Controller as player 1
    const struct {
        HidNpadIdType id;
        HidNpadStyleTag style;
    } pads[] = {{HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld},
                {HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual},
                {HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey}};
    for (const auto& pd : pads)
        if (R_SUCCEEDED(hidInitializeVibrationDevices(&g_vib[g_vibCount], 2, pd.id, pd.style))) g_vibCount += 2;
#endif
}

void shutdown() {
#ifdef __SWITCH__
    appletSetMediaPlaybackState(false);
    pdmqryExit();
    nsExit();
    csrngExit();
    nifmExit();
    if (g_nxlinkSock >= 0) close(g_nxlinkSock);
    socketExit();
    romfsExit();
#endif
}

bool poll(Input& in, int w, int h) {
    in.down = 0;
    in.scroll = 0;
    in.text.clear();
    in.backspaces = 0;
    in.enterKey = in.shift = false;
    bool quit = false;
    SDL_Event e;
    // On Switch, SDL's event pump also runs appletMainLoop() (HOME, sleep,
    // docked/handheld resize) and turns an exit request into SDL_QUIT.
    while (SDL_PollEvent(&e)) handleEvent(e, in, w, h, quit);
    in.touch = g_touch;
    in.tx = g_tx;
    in.ty = g_ty;
#ifdef __SWITCH__
    padUpdate(&g_pad);
    u64 down = padGetButtonsDown(&g_pad), held = padGetButtons(&g_pad);
    static const struct {
        u64 pad;
        uint32_t btn;
    } map[] = {
        {HidNpadButton_A, BtnA},       {HidNpadButton_B, BtnB},
        {HidNpadButton_X, BtnX},       {HidNpadButton_Y, BtnY},
        {HidNpadButton_L, BtnL},       {HidNpadButton_R, BtnR},
        {HidNpadButton_ZL, BtnZL},     {HidNpadButton_ZR, BtnZR},
        {HidNpadButton_Plus, BtnPlus}, {HidNpadButton_Minus, BtnMinus},
        {HidNpadButton_Up | HidNpadButton_StickLUp, BtnUp},
        {HidNpadButton_Down | HidNpadButton_StickLDown, BtnDown},
        {HidNpadButton_Left | HidNpadButton_StickLLeft, BtnLeft},
        {HidNpadButton_Right | HidNpadButton_StickLRight, BtnRight},
    };
    in.held = g_keys;
    for (const auto& m : map) {
        if (down & m.pad) in.down |= m.btn;
        if (held & m.pad) in.held |= m.btn;
    }
    HidAnalogStickState rs = padGetStickPos(&g_pad, 1);
    float ry = rs.y / 32767.0f;
    if (ry > 0.2f || ry < -0.2f) in.scroll -= ry;  // stick up scrolls up
    // rumble pattern: on 90 ms, off 70 ms, on 90 ms
    static const uint32_t steps[] = {0, 90, 160, 250};
    while (g_rumbleStep >= 0 && g_rumbleStep < 4 && SDL_GetTicks() - g_rumbleAt >= steps[g_rumbleStep])
        vibrate(g_rumbleStep++ % 2 == 0);
    if (g_rumbleStep >= 4) g_rumbleStep = -1;
#else
    in.held = g_keys;
#endif
    return !quit;
}

bool keyboard(const std::string& header, const std::string& initial, std::string& out, bool multiline,
              unsigned maxLen) {
#ifdef __SWITCH__
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header.c_str());
    swkbdConfigSetGuideText(&kbd, header.c_str());
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, maxLen);
    swkbdConfigSetReturnButtonFlag(&kbd, multiline ? 1 : 0);
    if (multiline) swkbdConfigSetTextDrawType(&kbd, SwkbdTextDrawType_Box);
    std::vector<char> buf(maxLen * 4 + 1, 0);  // UTF-8: up to 4 bytes per character
    Result rc = swkbdShow(&kbd, buf.data(), buf.size());
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return false;  // cancelled
    out = buf.data();
    return true;
#else
    (void)multiline;
    (void)maxLen;
    std::printf("\n[keyboard] %s (current: \"%s\")\n> ", header.c_str(), initial.c_str());
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    // "\n" typed literally becomes a newline, so multi-line text can be tested.
    for (size_t p; (p = line.find("\\n")) != std::string::npos;) line.replace(p, 2, "\n");
    out = line;
    return true;
#endif
}

bool systemDarkTheme() {
#ifdef __SWITCH__
    ColorSetId id = ColorSetId_Light;
    if (R_SUCCEEDED(setsysInitialize())) {
        setsysGetColorSetId(&id);
        setsysExit();
    }
    return id == ColorSetId_Dark;
#else
    return false;
#endif
}

int localHour() {
#ifdef __SWITCH__
    // newlib's localtime() has no time zone on Switch; ask the time service.
    u64 now = 0;
    TimeCalendarTime cal;
    TimeCalendarAdditionalInfo info;
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock, &now)) &&
        R_SUCCEEDED(timeToCalendarTimeWithMyRule(now, &cal, &info)))
        return cal.hour;
    return 12;
#else
    time_t t = time(nullptr);
    tm lt;
    localtime_r(&t, &lt);
    return lt.tm_hour;
#endif
}

std::string assetDir() {
#ifdef __SWITCH__
    return "romfs:/";
#else
    return "romfs/";
#endif
}

// ---------------------------------------------------------------- album

std::string albumDir() {
#ifdef __SWITCH__
    return "sdmc:/Nintendo/Album/";
#else
    return "ai-switch-data/Album/";
#endif
}

namespace {
// Album layout is YYYY/MM/DD/YYYYMMDDHHMMSSxx-<hash>.jpg, so walking names in
// descending order yields the newest screenshots first.
void scanAlbum(const std::string& dir, std::vector<Screenshot>& out, size_t max) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = readdir(d))
        if (e->d_name[0] != '.') names.push_back(e->d_name);
    closedir(d);
    std::sort(names.rbegin(), names.rend());
    for (const auto& n : names) {
        if (out.size() >= max) return;
        std::string path = dir + n;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scanAlbum(path + "/", out, max);
        } else if (n.size() > 4 && (n.substr(n.size() - 4) == ".jpg" || n.substr(n.size() - 4) == ".JPG")) {
            std::string label = n;
            if (n.size() >= 12 && std::all_of(n.begin(), n.begin() + 12, ::isdigit))
                label = n.substr(0, 4) + "-" + n.substr(4, 2) + "-" + n.substr(6, 2) + "  " + n.substr(8, 2) + ":" +
                        n.substr(10, 2);
            out.push_back({path, label});
        }
    }
}
}  // namespace

std::vector<Screenshot> screenshots(size_t max) {
    std::vector<Screenshot> out;
    scanAlbum(albumDir(), out, max);
    return out;
}

// ---------------------------------------------------------------- games

namespace {
struct Game {
    std::string name;
    uint64_t playNs = 0, lastPlayed = 0;
    uint32_t launches = 0;
};

std::vector<Game> loadGames(std::string& note) {
    std::vector<Game> games;
#ifdef __SWITCH__
    std::vector<NsApplicationRecord> recs(1024);
    s32 count = 0;
    if (R_FAILED(nsListApplicationRecord(recs.data(), (s32)recs.size(), 0, &count))) {
        note = "Couldn't read the installed games.";
        return games;
    }
    auto ctl = std::make_unique<NsApplicationControlData>();  // ~150 KB, keep it off the stack
    for (s32 i = 0; i < count; i++) {
        Game g;
        u64 size = 0;
        NacpLanguageEntry* lang = nullptr;
        if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, recs[i].application_id,
                                                    ctl.get(), sizeof(*ctl), &size)) &&
            R_SUCCEEDED(nacpGetLanguageEntry(&ctl->nacp, &lang)) && lang)
            g.name = lang->name;
        if (g.name.empty()) {
            char id[20];
            snprintf(id, sizeof id, "%016lX", (unsigned long)recs[i].application_id);
            g.name = std::string("Unknown title ") + id;
        }
        PdmPlayStatistics st{};
        if (R_SUCCEEDED(pdmqryQueryPlayStatisticsByApplicationId(recs[i].application_id, false, &st))) {
            g.playNs = st.playtime;
            g.launches = st.total_launches;
            g.lastPlayed = st.last_timestamp_user;
        }
        games.push_back(g);
    }
#else
    note = " (desktop preview: sample data)";
    // {name, play time ns, last played (posix), launches}
    games = {{"The Legend of Zelda: Tears of the Kingdom", 512340ull * 1000000000ull, 1791100000, 87},
             {"Mario Kart 8 Deluxe", 90000ull * 1000000000ull, 1791300000, 140},
             {"Stardew Valley", 230000ull * 1000000000ull, 1780000000, 61},
             {"Hollow Knight", 0, 0, 0}};
#endif
    return games;
}
}  // namespace

std::string gameLibrary(const std::string& sortBy) {
    static std::vector<Game> cache;  // worker thread only; games rarely change while the app runs
    static std::string note;
    if (cache.empty()) cache = loadGames(note);
    if (cache.empty()) return note.empty() ? "No games are installed." : note;
    std::vector<Game> g = cache;
    if (sortBy == "name")
        std::sort(g.begin(), g.end(), [](const Game& a, const Game& b) { return a.name < b.name; });
    else if (sortBy == "play_time")
        std::sort(g.begin(), g.end(), [](const Game& a, const Game& b) { return a.playNs > b.playNs; });
    else
        std::sort(g.begin(), g.end(), [](const Game& a, const Game& b) { return a.lastPlayed > b.lastPlayed; });
    std::string out = std::to_string(g.size()) + " games installed, sorted by " +
                      (sortBy.empty() ? "last_played" : sortBy) + note + ". Play time is the total on this console.\n";
    for (size_t i = 0; i < g.size() && out.size() < 15000; i++) {
        out += std::to_string(i + 1) + ". " + g[i].name + ": ";
        if (!g[i].launches) {
            out += "never played\n";
            continue;
        }
        uint64_t min = g[i].playNs / 60000000000ull;
        char when[16] = "unknown";
        time_t t = (time_t)g[i].lastPlayed;
        tm lt;
        if (t && gmtime_r(&t, &lt)) strftime(when, sizeof when, "%Y-%m-%d", &lt);
        out += std::to_string(min / 60) + " h " + std::to_string(min % 60) + " min, " +
               std::to_string(g[i].launches) + " launches, last played " + when + "\n";
    }
    return out;
}

// ---------------------------------------------------------------- feedback

void rumble() {
#ifdef __SWITCH__
    g_rumbleStep = 0;
    g_rumbleAt = SDL_GetTicks();
#endif
}

void keepAwake(bool awake) {
#ifdef __SWITCH__
    appletSetMediaPlaybackState(awake);
#else
    (void)awake;
#endif
}

}  // namespace platform
