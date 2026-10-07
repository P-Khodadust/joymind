// OpenAI-compatible Chat Completions: POST {base}/chat/completions. Covers OpenAI,
// OpenRouter, Groq, DeepSeek, Mistral and self-hosted servers (Ollama, LM Studio).
#include "net/providers/provider.hpp"

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {
const char* kDefaultBase = "https://api.openai.com/v1";

class OpenAICompatProvider : public IProvider {
public:
    HttpRequest buildRequest(const std::vector<Message>& conv, const Profile& p,
                             const std::vector<ToolSpec>& tools) const override {
        json msgs = json::array();
        if (!p.systemPrompt.empty()) msgs.push_back({{"role", "system"}, {"content", p.systemPrompt}});
        for (const auto& m : normalizeHistory(conv, !tools.empty())) {
            if (m.role == "tool") {
                msgs.push_back({{"role", "tool"}, {"tool_call_id", m.callId}, {"content", toolResultText(m)}});
                continue;
            }
            json msg = {{"role", m.role}, {"content", m.text}};
            if (!m.images.empty()) {
                json parts = json::array();
                for (const auto& img : m.images)
                    parts.push_back({{"type", "image_url"}, {"image_url", {{"url", "data:image/jpeg;base64," + img}}}});
                if (!m.text.empty()) parts.push_back({{"type", "text"}, {"text", m.text}});
                msg["content"] = parts;
            }
            if (!m.calls.empty()) {
                json calls = json::array();
                for (const auto& c : m.calls)
                    calls.push_back({{"id", c.id}, {"type", "function"}, {"function", {{"name", c.name}, {"arguments", c.args}}}});
                msg["tool_calls"] = calls;
                if (m.text.empty()) msg["content"] = nullptr;
            }
            msgs.push_back(msg);
        }

        json body = {{"model", p.model}, {"messages", msgs}, {"stream", true}};
        if (p.maxTokens > 0) body[p.maxTokensField.empty() ? "max_tokens" : p.maxTokensField] = p.maxTokens;
        if (p.hasTemperature) body["temperature"] = p.temperature;
        if (!tools.empty()) {
            json t = json::array();
            for (const auto& s : tools)
                t.push_back({{"type", "function"},
                             {"function", {{"name", s.name}, {"description", s.description}, {"parameters", json::parse(s.schema)}}}});
            body["tools"] = t;
        }

        HttpRequest r;
        r.url = joinUrl(p.baseUrl, kDefaultBase, "/chat/completions");
        r.body = body.dump(-1, ' ', false, json::error_handler_t::replace);
        r.headers = authHeaders(p);
        r.headers.push_back({"Content-Type", "application/json"});
        r.headers.insert(r.headers.end(), p.extraHeaders.begin(), p.extraHeaders.end());
        return r;
    }

    StreamEvent parseStreamChunk(const SseEvent& ev) override {
        StreamEvent out;
        if (ev.data == "[DONE]") {
            out.type = StreamEvent::Done;
            return out;
        }
        json j = json::parse(ev.data, nullptr, false);
        if (!j.is_object()) {
            out.type = StreamEvent::Error;
            out.text = "Malformed stream data from provider";
            return out;
        }
        if (j.contains("error")) {
            out.type = StreamEvent::Error;
            out.text = "Provider error: " + extractErrorMessage(ev.data);
            return out;
        }
        // Reasoning fields (delta.reasoning / reasoning_content) are ignored on purpose.
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            const json& delta = j["choices"][0].value("delta", json::object());
            if (delta.is_object() && delta.contains("content") && delta["content"].is_string()) {
                out.text = delta["content"].get<std::string>();
                if (!out.text.empty()) out.type = StreamEvent::TextDelta;
            }
            // Tool calls stream as fragments keyed by index: id/name first, then
            // pieces of the JSON arguments string.
            if (delta.is_object() && delta.contains("tool_calls") && delta["tool_calls"].is_array())
                for (const auto& tc : delta["tool_calls"]) {
                    if (!tc.is_object()) continue;
                    ToolCall& c = calls_[tc.value("index", 0)];
                    if (tc.contains("id") && tc["id"].is_string() && c.id.empty()) c.id = tc["id"].get<std::string>();
                    const json& f = tc.value("function", json::object());
                    if (f.contains("name") && f["name"].is_string() && c.name.empty()) c.name = f["name"].get<std::string>();
                    if (f.contains("arguments") && f["arguments"].is_string()) c.args += f["arguments"].get<std::string>();
                }
        } else if (j.contains("usage") && j["usage"].is_object()) {
            out.type = StreamEvent::Usage;
            out.inputTokens = j["usage"].value("prompt_tokens", 0);
            out.outputTokens = j["usage"].value("completion_tokens", 0);
        }
        return out;
    }

    TurnResult finishTurn() override {
        TurnResult r;
        for (auto& kv : calls_) {
            ToolCall c = kv.second;
            if (c.id.empty()) c.id = "call_" + std::to_string(kv.first);
            if (c.args.empty()) c.args = "{}";
            if (!c.name.empty()) r.calls.push_back(c);
        }
        return r;
    }

    HttpRequest modelsRequest(const Profile& p) const override {
        HttpRequest r;
        r.method = "GET";
        r.url = joinUrl(p.baseUrl, kDefaultBase, "/models");
        r.headers = authHeaders(p);
        r.headers.insert(r.headers.end(), p.extraHeaders.begin(), p.extraHeaders.end());
        return r;
    }

    std::vector<ModelInfo> parseModels(const std::string& body) const override {
        std::vector<ModelInfo> v;
        json j = json::parse(body, nullptr, false);
        if (!j.is_object() || !j["data"].is_array()) return v;
        for (const auto& m : j["data"])
            if (m.is_object() && m.contains("id") && m["id"].is_string())
                v.push_back({m["id"].get<std::string>(),
                             m.contains("name") && m["name"].is_string() ? m["name"].get<std::string>() : ""});
        std::sort(v.begin(), v.end(), [](const ModelInfo& a, const ModelInfo& b) { return a.id < b.id; });
        return v;
    }

private:
    static Headers authHeaders(const Profile& p) {
        if (p.apiKey.empty()) return {};  // local servers often need no key
        return {{"Authorization", "Bearer " + p.apiKey}};
    }
    std::map<int, ToolCall> calls_;
};
}  // namespace

std::unique_ptr<IProvider> makeOpenAICompatProvider() { return std::make_unique<OpenAICompatProvider>(); }
