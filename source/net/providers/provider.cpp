#include "net/providers/provider.hpp"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

std::unique_ptr<IProvider> makeAnthropicProvider();
std::unique_ptr<IProvider> makeOpenAICompatProvider();
std::unique_ptr<IProvider> makeGeminiProvider();

std::unique_ptr<IProvider> makeProvider(const std::string& type) {
    if (type == "openai_compat") return makeOpenAICompatProvider();
    if (type == "gemini") return makeGeminiProvider();
    return makeAnthropicProvider();
}

const std::vector<std::string>& providerTypes() {
    static const std::vector<std::string> t{"anthropic", "openai_compat", "gemini"};
    return t;
}

std::vector<Profile> providerPresets() {
    auto mk = [](const char* name, const char* type, const char* url, const char* model,
                 int maxTok, const char* field) {
        Profile p;
        p.name = name;
        p.type = type;
        p.baseUrl = url;
        p.model = model;
        p.maxTokens = maxTok;
        p.maxTokensField = field;
        return p;
    };
    // Model names are only suggestions; they are always editable and can be
    // replaced from the provider's live model list.
    std::vector<Profile> v{
        mk("Anthropic", "anthropic", "https://api.anthropic.com", "claude-opus-5-5", 32000, "max_tokens"),
        mk("OpenAI", "openai_compat", "https://api.openai.com/v1", "", 8192, "max_completion_tokens"),
        mk("OpenRouter", "openai_compat", "https://openrouter.ai/api/v1", "", 8192, "max_tokens"),
        mk("Groq", "openai_compat", "https://api.groq.com/openai/v1", "", 8192, "max_tokens"),
        mk("DeepSeek", "openai_compat", "https://api.deepseek.com", "", 8192, "max_tokens"),
        mk("Mistral", "openai_compat", "https://api.mistral.ai/v1", "", 8192, "max_tokens"),
        mk("Google Gemini", "gemini", "https://generativelanguage.googleapis.com", "", 8192, "max_tokens"),
        mk("Custom (OpenAI-compatible)", "openai_compat", "http://192.168.1.100:11434/v1", "", 4096,
           "max_tokens"),
    };
    v[2].extraHeaders = {{"HTTP-Referer", "https://pouyakh.dev"}, {"X-Title", "Claude (Unofficial) for Switch"}};
    return v;
}

namespace {
// Gives every call of the last assistant turn a result, so providers that
// require one result per call (all of them) accept the history.
void closeOpenCalls(std::vector<Message>& out) {
    size_t a = out.size();
    while (a > 0 && out[a - 1].role == "tool") a--;
    if (a == 0 || out[a - 1].role != "assistant") return;
    const Message& asst = out[a - 1];
    std::vector<Message> missing;
    for (const auto& c : asst.calls) {
        bool answered = false;
        for (size_t i = a; i < out.size(); i++) answered |= out[i].callId == c.id;
        if (answered) continue;
        Message t;
        t.role = "tool";
        t.callId = c.id;
        t.toolName = c.name;
        t.text = "Not run: the user cancelled this command.";
        t.isError = true;
        missing.push_back(t);
    }
    out.insert(out.end(), missing.begin(), missing.end());
}

void appendMerged(std::vector<Message>& out, Message m) {
    if (!out.empty() && out.back().role == m.role && m.role != "tool") {
        Message& b = out.back();
        b.text += (b.text.empty() || m.text.empty() ? "" : "\n\n") + m.text;
        b.calls.insert(b.calls.end(), m.calls.begin(), m.calls.end());
        b.images.insert(b.images.end(), m.images.begin(), m.images.end());
        b.native.clear();  // a merged turn is no longer what the provider sent
    } else {
        out.push_back(std::move(m));
    }
}
}  // namespace

std::vector<Message> normalizeHistory(const std::vector<Message>& in, bool toolsEnabled) {
    // The current tool loop starts at the last real user message. Only its
    // assistant turns keep provider-native content (older thinking blocks may
    // be dropped, and replaying them after a system/tool change is rejected).
    size_t loopStart = 0;
    for (size_t i = 0; i < in.size(); i++)
        if (in[i].role == "user" && !in[i].text.empty()) loopStart = i;

    std::vector<Message> out;
    for (size_t i = 0; i < in.size(); i++) {
        Message m = in[i];
        if (m.role == "tool") {
            // keep only results that answer a call of the turn right before
            bool known = false;
            for (size_t k = out.size(); k > 0; k--) {
                if (out[k - 1].role == "tool") continue;
                if (out[k - 1].role == "assistant")
                    for (const auto& c : out[k - 1].calls) known |= c.id == m.callId;
                break;
            }
            if (known) out.push_back(std::move(m));
            continue;
        }
        if (m.role == "assistant" && i < loopStart) m.native.clear();
        if (m.text.empty() && m.calls.empty() && m.images.empty()) continue;  // failed or empty turn
        closeOpenCalls(out);
        appendMerged(out, std::move(m));
    }
    closeOpenCalls(out);

    if (!toolsEnabled) {
        // No tools in this request: keep the earlier commands as plain text.
        std::vector<Message> flat;
        for (auto& m : out) {
            if (m.role == "assistant" && !m.calls.empty()) {
                for (const auto& c : m.calls) m.text += (m.text.empty() ? "" : "\n") + ("[ran command: " + commandOf(c) + "]");
                m.calls.clear();
                m.native.clear();
            } else if (m.role == "tool") {
                m.role = "user";
                m.text = "[command output]\n" + toolResultText(m);
            }
            appendMerged(flat, std::move(m));
        }
        out.swap(flat);
    }
    while (!out.empty() && out.front().role != "user") out.erase(out.begin());
    return out;
}

std::string toolResultText(const Message& m) {
    return (m.exitCode >= 0 ? "Exit code " + std::to_string(m.exitCode) + "\n" : "") + m.text;
}

ToolSpec runCommandTool() {
    return {"run_command",
            "Run a shell command on the user's remote machine over SSH and get its combined stdout/stderr "
            "(truncated to 16 KB) and exit code. Commands run non-interactively without a TTY and time out "
            "after 120 seconds. The user approves each command before it runs.",
            R"({"type":"object","properties":{"command":{"type":"string","description":"The shell command to run"}},"required":["command"]})"};
}

ToolSpec gameLibraryTool() {
    return {"get_game_library",
            "List the games installed on the user's Nintendo Switch with total play time, number of launches and "
            "when each was last played. Use it for questions about their games, play habits or what to play next.",
            R"json({"type":"object","properties":{"sort_by":{"type":"string","enum":["last_played","play_time","name"],)json"
            R"json("description":"Order of the list (default last_played)"}}})json"};
}

std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < in.size()) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < in.size()) v |= (unsigned char)in[i + 2];
        out += t[v >> 18 & 63];
        out += t[v >> 12 & 63];
        out += i + 1 < in.size() ? t[v >> 6 & 63] : '=';
        out += i + 2 < in.size() ? t[v & 63] : '=';
    }
    return out;
}

std::string argOf(const ToolCall& c, const char* key) {
    json j = json::parse(c.args, nullptr, false);
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : "";
}

// Pulls a human-readable message out of the error bodies used by all providers:
// {"error":{"message":..}}, {"type":"error","error":{..}}, [{"error":..}] (Gemini),
// {"error":"text"}, {"message":..}, {"detail":..}. Falls back to the raw text.
std::string extractErrorMessage(const std::string& body) {
    json j = json::parse(body, nullptr, false);
    if (j.is_array() && !j.empty()) j = j[0];
    if (j.is_object()) {
        auto str = [](const json& v) { return v.is_string() ? v.get<std::string>() : std::string(); };
        if (j.contains("error")) {
            const json& e = j["error"];
            if (e.is_string()) return e.get<std::string>();
            if (e.is_object()) {
                if (e.contains("message")) return str(e["message"]);
                if (e.contains("type")) return str(e["type"]);
            }
        }
        if (j.contains("message")) return str(j["message"]);
        if (j.contains("detail")) return str(j["detail"]);
    }
    std::string s;
    for (char c : body.substr(0, 200))
        s += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string joinUrl(const std::string& base, const std::string& fallback, const std::string& path) {
    std::string b = base.empty() ? fallback : base;
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b + path;
}
