// Config and chat persistence on the SD card (sdmc:/switch/ai-switch/).
// Missing or corrupt files are backed up (*.bad) and replaced by defaults.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "net/providers/provider.hpp"
#include "net/ssh.hpp"

struct Config {
    std::string userName;
    std::string theme = "auto";  // light | dark | auto (follows the console theme)
    int fontSize = 1;            // 0 small, 1 medium, 2 large
    std::string defaultProfile;
    std::vector<Profile> profiles;
    std::string defaultMachine;  // machine new chats start with ("" = none)
    bool shareGames = true;      // offer the get_game_library tool to the AI
    std::vector<net::Machine> machines;
};

struct ChatMessage : Message {
    std::string error;  // shown under the message, not persisted
};

struct Chat {
    std::string id, title, profileId, model;
    std::string machineId;  // SSH machine the AI may run commands on ("" = none)
    int64_t created = 0, updated = 0;
    std::vector<ChatMessage> messages;
};

struct ChatMeta {
    std::string id, title;
    int64_t updated = 0;
};

namespace storage {

void init();
// notice is set when a corrupt file had to be backed up.
Config loadConfig(std::string& notice);
bool saveConfig(const Config& c);
std::vector<ChatMeta> loadIndex();
bool loadChat(const std::string& id, Chat& out);
bool saveChat(const Chat& c, std::vector<ChatMeta>& index);
void deleteChat(const std::string& id, std::vector<ChatMeta>& index);
std::string newId();
std::string makeTitle(const std::string& firstUserMessage);

}  // namespace storage
