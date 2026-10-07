// Everything console-specific (libnx services, pad, swkbd, system theme, local
// time) lives here. Non-Switch builds get small desktop stand-ins so the UI can
// be previewed on a PC.
#pragma once
#include <string>
#include <vector>

#include "ui/widgets.hpp"

namespace platform {

void init();
void shutdown();
// Polls SDL + pad input. Returns false when the app should quit.
bool poll(Input& in, int width, int height);
// Shows the system keyboard (blocking). Returns false if cancelled.
bool keyboard(const std::string& header, const std::string& initial, std::string& out, bool multiline = false,
              unsigned maxLen = 500);
bool systemDarkTheme();
int localHour();
std::string assetDir();  // romfs root

// Screenshots from the Album on the SD card, newest first.
struct Screenshot {
    std::string path, label;
};
std::string albumDir();
std::vector<Screenshot> screenshots(size_t max);

// Installed games with play statistics, as text for the AI. Slow the first
// time (reads every game's metadata), so call it on the worker thread.
std::string gameLibrary(const std::string& sortBy);

void rumble();               // short double pulse on the active controller
void keepAwake(bool awake);  // no screen dimming / auto-sleep while true

}  // namespace platform
