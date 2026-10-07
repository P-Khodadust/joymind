#include "storage/storage.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace storage {
namespace {

#ifdef __SWITCH__
const std::string kDir = "sdmc:/switch/ai-switch/";
#else
const std::string kDir = "ai-switch-data/";  // desktop preview build
#endif
const std::string kChats = kDir + "chats/";

bool readFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

// Write to *.tmp first so a crash or power loss mid-write never truncates the
// real file. FAT32 rename cannot replace, so remove the old file first; the
// loader falls back to *.tmp if we died in between.
bool writeFile(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(tmp.c_str());
        return false;
    }
    remove(path.c_str());
    return rename(tmp.c_str(), path.c_str()) == 0;
}

bool readJson(const std::string& path, json& j, bool& corrupt) {
    std::string s;
    corrupt = false;
    if (!readFile(path, s) && !readFile(path + ".tmp", s)) return false;
    j = json::parse(s, nullptr, false);
    corrupt = j.is_discarded() || !j.is_object();
    return !corrupt;
}

void backup(const std::string& path) {
    std::string bad = path + ".bad";
    remove(bad.c_str());
    rename(path.c_str(), bad.c_str());
}

std::string str(const json& j, const char* k, const std::string& def = "") {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : def;
}

double num(const json& j, const char* k, double def) {
    auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<double>() : def;
}

json profileToJson(const Profile& p) {
    json h = json::object();
    for (const auto& kv : p.extraHeaders) h[kv.first] = kv.second;
    return {{"id", p.id},
            {"name", p.name},
            {"type", p.type},
            {"base_url", p.baseUrl},
            {"api_key", p.apiKey},
            {"model", p.model},
            {"max_tokens", p.maxTokens},
            {"max_tokens_field", p.maxTokensField},
            {"temperature", p.hasTemperature ? json(p.temperature) : json(nullptr)},
            {"system_prompt", p.systemPrompt},
            {"extra_headers", h}};
}

Profile profileFromJson(const json& j) {
    Profile p;
    p.id = str(j, "id", newId());
    p.name = str(j, "name", "Provider");
    p.type = str(j, "type", "anthropic");
    if (std::find(providerTypes().begin(), providerTypes().end(), p.type) == providerTypes().end())
        p.type = "openai_compat";
    p.baseUrl = str(j, "base_url");
    p.apiKey = str(j, "api_key");
    p.model = str(j, "model");
    p.maxTokens = (int)num(j, "max_tokens", 4096);
    p.maxTokensField = str(j, "max_tokens_field", "max_tokens");
    p.hasTemperature = j.contains("temperature") && j["temperature"].is_number();
    p.temperature = (float)num(j, "temperature", 1.0);
    p.systemPrompt = str(j, "system_prompt");
    if (j.contains("extra_headers") && j["extra_headers"].is_object())
        for (auto it = j["extra_headers"].begin(); it != j["extra_headers"].end(); ++it)
            if (it->is_string()) p.extraHeaders.push_back({it.key(), it->get<std::string>()});
    return p;
}

json machineToJson(const net::Machine& m) {
    return {{"id", m.id},           {"name", m.name},         {"host", m.host},
            {"port", m.port},       {"user", m.user},         {"auth", m.auth},
            {"password", m.password}, {"key_path", m.keyPath}, {"passphrase", m.passphrase},
            {"host_key", m.hostKey}};
}

net::Machine machineFromJson(const json& j) {
    net::Machine m;
    m.id = str(j, "id", newId());
    m.name = str(j, "name", "Machine");
    m.host = str(j, "host");
    m.port = (int)num(j, "port", 22);
    m.user = str(j, "user");
    m.auth = str(j, "auth", "key") == "password" ? "password" : "key";
    m.password = str(j, "password");
    m.keyPath = str(j, "key_path");
    m.passphrase = str(j, "passphrase");
    m.hostKey = str(j, "host_key");
    return m;
}

json messageToJson(const ChatMessage& m) {
    json j = {{"role", m.role}, {"text", m.text}};
    if (!m.imagePaths.empty()) j["images"] = m.imagePaths;
    if (!m.calls.empty()) {
        json calls = json::array();
        for (const auto& c : m.calls) calls.push_back({{"id", c.id}, {"name", c.name}, {"args", c.args}});
        j["calls"] = calls;
    }
    if (!m.native.empty()) j["native"] = m.native;
    if (m.role == "tool") {
        j["call_id"] = m.callId;
        j["tool_name"] = m.toolName;
        j["exit_code"] = m.exitCode;
        j["is_error"] = m.isError;
    }
    return j;
}

ChatMessage messageFromJson(const json& j) {
    ChatMessage m;
    m.role = str(j, "role", "user");
    m.text = str(j, "text");
    if (j.contains("calls") && j["calls"].is_array())
        for (const auto& c : j["calls"])
            if (c.is_object()) m.calls.push_back({str(c, "id"), str(c, "name"), str(c, "args", "{}")});
    m.native = str(j, "native");
    if (j.contains("images") && j["images"].is_array())
        for (const auto& p : j["images"])
            if (p.is_string()) m.imagePaths.push_back(p.get<std::string>());
    m.callId = str(j, "call_id");
    m.toolName = str(j, "tool_name");
    m.exitCode = (int)num(j, "exit_code", -1);
    m.isError = j.contains("is_error") && j["is_error"].is_boolean() && j["is_error"].get<bool>();
    return m;
}

std::string dump(const json& j, int indent = -1) { return j.dump(indent, ' ', false, json::error_handler_t::replace); }

void sortIndex(std::vector<ChatMeta>& index) {
    std::sort(index.begin(), index.end(), [](const ChatMeta& a, const ChatMeta& b) { return a.updated > b.updated; });
}

bool saveIndex(const std::vector<ChatMeta>& index) {
    json a = json::array();
    for (const auto& m : index) a.push_back({{"id", m.id}, {"title", m.title}, {"updated", m.updated}});
    return writeFile(kChats + "index.json", dump(json({{"chats", a}})));
}

}  // namespace

void init() {
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
#endif
    mkdir(kDir.c_str(), 0777);
    mkdir(kChats.c_str(), 0777);
}

Config loadConfig(std::string& notice) {
    Config c;
    json j;
    bool corrupt;
    std::string path = kDir + "config.json";
    if (!readJson(path, j, corrupt)) {
        if (corrupt) {
            backup(path);
            notice = "config.json was unreadable; it was saved as config.json.bad and defaults were loaded.";
        }
        return c;
    }
    c.userName = str(j, "user_name");
    c.theme = str(j, "theme", "auto");
    c.fontSize = std::clamp((int)num(j, "font_size", 1), 0, 2);
    c.defaultProfile = str(j, "default_profile");
    if (j.contains("profiles") && j["profiles"].is_array())
        for (const auto& pj : j["profiles"])
            if (pj.is_object()) c.profiles.push_back(profileFromJson(pj));
    c.defaultMachine = str(j, "default_machine");
    c.shareGames = !j.contains("share_games") || !j["share_games"].is_boolean() || j["share_games"].get<bool>();
    if (j.contains("machines") && j["machines"].is_array())
        for (const auto& mj : j["machines"])
            if (mj.is_object()) c.machines.push_back(machineFromJson(mj));
    return c;
}

bool saveConfig(const Config& c) {
    json profiles = json::array();
    for (const auto& p : c.profiles) profiles.push_back(profileToJson(p));
    json machines = json::array();
    for (const auto& m : c.machines) machines.push_back(machineToJson(m));
    json j = {{"version", 1},
              {"user_name", c.userName},
              {"theme", c.theme},
              {"font_size", c.fontSize},
              {"default_profile", c.defaultProfile},
              {"profiles", profiles},
              {"default_machine", c.defaultMachine},
              {"share_games", c.shareGames},
              {"machines", machines}};
    return writeFile(kDir + "config.json", dump(j, 2));
}

std::vector<ChatMeta> loadIndex() {
    std::vector<ChatMeta> index;
    json j;
    bool corrupt;
    if (readJson(kChats + "index.json", j, corrupt) && j.contains("chats") && j["chats"].is_array()) {
        for (const auto& m : j["chats"])
            if (m.is_object()) index.push_back({str(m, "id"), str(m, "title"), (int64_t)num(m, "updated", 0)});
        sortIndex(index);
        return index;
    }
    // Missing or corrupt index: rebuild it from the chat files (slow path, once).
    if (DIR* d = opendir(kChats.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            if (name.size() < 6 || name.substr(name.size() - 5) != ".json" || name == "index.json") continue;
            Chat chat;
            if (loadChat(name.substr(0, name.size() - 5), chat)) index.push_back({chat.id, chat.title, chat.updated});
        }
        closedir(d);
    }
    sortIndex(index);
    saveIndex(index);
    return index;
}

bool loadChat(const std::string& id, Chat& out) {
    json j;
    bool corrupt;
    std::string path = kChats + id + ".json";
    if (!readJson(path, j, corrupt)) {
        if (corrupt) backup(path);
        return false;
    }
    out = Chat();
    out.id = id;
    out.title = str(j, "title", "Untitled");
    out.profileId = str(j, "profile_id");
    out.model = str(j, "model");
    out.machineId = str(j, "machine_id");
    out.created = (int64_t)num(j, "created", 0);
    out.updated = (int64_t)num(j, "updated", 0);
    if (j.contains("messages") && j["messages"].is_array())
        for (const auto& m : j["messages"])
            if (m.is_object()) out.messages.push_back(messageFromJson(m));
    return true;
}

bool saveChat(const Chat& c, std::vector<ChatMeta>& index) {
    json msgs = json::array();
    for (const auto& m : c.messages)
        if (!m.text.empty() || !m.calls.empty() || !m.imagePaths.empty() || m.role == "tool")
            msgs.push_back(messageToJson(m));
    json j = {{"id", c.id},           {"title", c.title},     {"profile_id", c.profileId}, {"model", c.model},
              {"machine_id", c.machineId}, {"created", c.created}, {"updated", c.updated}, {"messages", msgs}};
    bool ok = writeFile(kChats + c.id + ".json", dump(j));
    auto it = std::find_if(index.begin(), index.end(), [&](const ChatMeta& m) { return m.id == c.id; });
    if (it == index.end())
        index.push_back({c.id, c.title, c.updated});
    else
        *it = {c.id, c.title, c.updated};
    sortIndex(index);
    return saveIndex(index) && ok;
}

void deleteChat(const std::string& id, std::vector<ChatMeta>& index) {
    remove((kChats + id + ".json").c_str());
    index.erase(std::remove_if(index.begin(), index.end(), [&](const ChatMeta& m) { return m.id == id; }),
                index.end());
    saveIndex(index);
}

std::string newId() {
    static unsigned counter = 0;
    return std::to_string((long long)time(nullptr)) + "-" + std::to_string(counter++);
}

std::string makeTitle(const std::string& s) {
    std::string t;
    for (char ch : s) {
        if (ch == '\n') break;
        if (ch == '\t' || ch == '\r') ch = ' ';
        if (ch == ' ' && (t.empty() || t.back() == ' ')) continue;
        t += ch;
    }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    const size_t kMax = 40;
    if (t.size() > kMax) {
        size_t cut = kMax;
        while (cut > 0 && ((unsigned char)t[cut] & 0xC0) == 0x80) cut--;  // don't split a UTF-8 sequence
        t = t.substr(0, cut) + "…";
    }
    return t.empty() ? "New chat" : t;
}

}  // namespace storage
