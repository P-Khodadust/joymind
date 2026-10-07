// Provider abstraction. The UI and chat logic only ever talk to IProvider;
// everything that differs between APIs lives in net/providers/*.cpp.
#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

// A tool invocation requested by the model. args is a JSON object as text.
struct ToolCall {
    std::string id, name, args;
};

// Neutral conversation format, converted per provider in buildRequest().
// role: "user", "assistant", or "tool" (the result of one ToolCall).
struct Message {
    std::string role;
    std::string text;              // user/assistant text, or tool output
    std::vector<std::string> imagePaths;  // user: attached JPEG screenshots (what gets saved)
    std::vector<std::string> images;      // user: the same images base64-encoded (filled by the worker)
    std::vector<ToolCall> calls;   // assistant: tools it asked to run
    std::string callId, toolName;  // tool: which call this answers
    int exitCode = -1;             // tool: command exit status, -1 if it never ran
    bool isError = false;          // tool: denied or failed to run
    // assistant: the provider's own content for a turn with tool calls (e.g.
    // Anthropic thinking blocks + signatures, Gemini thought signatures). These
    // must be sent back unchanged while that tool loop continues.
    std::string native;
};

// A tool offered to the model; schema is a JSON Schema object as text.
struct ToolSpec {
    std::string name, description, schema;
};

// What a finished assistant turn asked for, collected while streaming.
struct TurnResult {
    std::vector<ToolCall> calls;
    std::string native;
};

using Headers = std::vector<std::pair<std::string, std::string>>;

// One provider profile as stored in config.json.
struct Profile {
    std::string id, name;
    std::string type = "anthropic";  // anthropic | openai_compat | gemini
    std::string baseUrl, apiKey, model, systemPrompt;
    std::string maxTokensField = "max_tokens";  // openai_compat: or "max_completion_tokens"
    int maxTokens = 8192;
    bool hasTemperature = false;
    float temperature = 1.0f;
    Headers extraHeaders;
};

struct HttpRequest {
    std::string method = "POST";
    std::string url, body;
    Headers headers;
};

struct ModelInfo {
    std::string id, name;
};

// One Server-Sent Event as produced by SseParser.
struct SseEvent {
    std::string event, data;
};

struct StreamEvent {
    enum Type { None, TextDelta, Done, Error, Usage } type = None;
    std::string text;
    int inputTokens = 0, outputTokens = 0;
};

// One instance per request: parseStreamChunk() accumulates tool calls, which
// finishTurn() returns once the stream has ended.
class IProvider {
public:
    virtual ~IProvider() = default;
    // Full streaming request for this conversation. p.model is the model to use.
    virtual HttpRequest buildRequest(const std::vector<Message>& conv, const Profile& p,
                                     const std::vector<ToolSpec>& tools) const = 0;
    virtual StreamEvent parseStreamChunk(const SseEvent& ev) = 0;
    virtual TurnResult finishTurn() { return {}; }
    virtual HttpRequest modelsRequest(const Profile& p) const = 0;
    virtual std::vector<ModelInfo> parseModels(const std::string& body) const = 0;
    // GET the model list. Implemented in net/http.cpp because it does network I/O.
    std::vector<ModelInfo> listModels(const Profile& p, std::string& error) const;
};

std::unique_ptr<IProvider> makeProvider(const std::string& type);

// Shared helpers for the provider implementations.
const std::vector<std::string>& providerTypes();
std::vector<Profile> providerPresets();
// Cleans history for sending: drops empty turns, merges same-role turns, adds
// "not run" results for unanswered tool calls, drops `native` outside the
// current tool loop, and flattens tool turns into text when tools are off.
std::vector<Message> normalizeHistory(const std::vector<Message>& in, bool toolsEnabled);
std::string toolResultText(const Message& toolMsg);  // what the model sees
ToolSpec runCommandTool();
ToolSpec gameLibraryTool();
std::string base64(const std::string& bytes);
std::string argOf(const ToolCall& c, const char* key);  // string argument, empty if missing
inline std::string commandOf(const ToolCall& c) { return argOf(c, "command"); }
std::string extractErrorMessage(const std::string& body);
std::string joinUrl(const std::string& base, const std::string& fallback, const std::string& path);
