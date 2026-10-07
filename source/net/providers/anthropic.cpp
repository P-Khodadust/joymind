// Anthropic Messages API: POST {base}/v1/messages with "stream": true.
#include "net/providers/provider.hpp"

#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {
const char* kDefaultBase = "https://api.anthropic.com";

class AnthropicProvider : public IProvider {
public:
    HttpRequest buildRequest(const std::vector<Message>& conv, const Profile& p,
                             const std::vector<ToolSpec>& tools) const override {
        json body = {{"model", p.model}, {"max_tokens", p.maxTokens}, {"stream", true}};
        if (!p.systemPrompt.empty()) body["system"] = p.systemPrompt;  // top level, not a message
        if (p.hasTemperature) body["temperature"] = p.temperature;
        if (!tools.empty()) {
            json t = json::array();
            for (const auto& s : tools)
                t.push_back({{"name", s.name}, {"description", s.description}, {"input_schema", json::parse(s.schema)}});
            body["tools"] = t;
        }
        // Tool results travel in user turns; consecutive same-role turns are
        // merged so roles alternate (tool_result blocks first, then any text).
        json msgs = json::array();
        for (const auto& m : normalizeHistory(conv, !tools.empty())) {
            std::string role = m.role == "assistant" ? "assistant" : "user";
            json blocks = m.native.empty() ? json::array() : json::parse(m.native, nullptr, false);
            if (m.role == "tool") {
                blocks.push_back({{"type", "tool_result"},
                                  {"tool_use_id", m.callId},
                                  {"content", toolResultText(m)},
                                  {"is_error", m.isError}});
            } else if (!blocks.is_array() || blocks.empty()) {
                // neutral form; otherwise `native` (thinking + tool_use) is sent unchanged
                blocks = json::array();
                for (const auto& img : m.images)
                    blocks.push_back({{"type", "image"},
                                      {"source", {{"type", "base64"}, {"media_type", "image/jpeg"}, {"data", img}}}});
                if (!m.text.empty()) blocks.push_back({{"type", "text"}, {"text", m.text}});
                for (const auto& c : m.calls) {
                    json input = json::parse(c.args, nullptr, false);
                    blocks.push_back({{"type", "tool_use"},
                                      {"id", c.id},
                                      {"name", c.name},
                                      {"input", input.is_object() ? input : json::object()}});
                }
            }
            if (!msgs.empty() && msgs.back()["role"] == role)
                for (auto& b : blocks) msgs.back()["content"].push_back(b);
            else
                msgs.push_back({{"role", role}, {"content", blocks}});
        }
        body["messages"] = msgs;

        HttpRequest r;
        r.url = joinUrl(p.baseUrl, kDefaultBase, "/v1/messages");
        r.body = body.dump(-1, ' ', false, json::error_handler_t::replace);
        r.headers = authHeaders(p);
        r.headers.push_back({"content-type", "application/json"});
        r.headers.insert(r.headers.end(), p.extraHeaders.begin(), p.extraHeaders.end());
        return r;
    }

    StreamEvent parseStreamChunk(const SseEvent& ev) override {
        StreamEvent out;
        json j = json::parse(ev.data, nullptr, false);
        if (!j.is_object()) {
            out.type = StreamEvent::Error;
            out.text = "Malformed stream data from provider";
            return out;
        }
        std::string type = j.value("type", ev.event);
        int idx = j.contains("index") && j["index"].is_number_integer() ? j["index"].get<int>() : 0;
        if (type == "content_block_start") {
            block(idx) = j.value("content_block", json::object());
        } else if (type == "content_block_delta") {
            const json& d = j.value("delta", json::object());
            std::string dt = d.value("type", "");
            json& b = block(idx);
            if (dt == "text_delta") {
                out.text = d.value("text", "");
                b["text"] = b.value("text", "") + out.text;
                if (!out.text.empty()) out.type = StreamEvent::TextDelta;
            } else if (dt == "thinking_delta") {  // kept for the tool loop, never shown
                b["thinking"] = b.value("thinking", "") + d.value("thinking", "");
            } else if (dt == "signature_delta") {
                b["signature"] = b.value("signature", "") + d.value("signature", "");
            } else if (dt == "input_json_delta") {
                partialJson_[idx] += d.value("partial_json", "");
            }
        } else if (type == "content_block_stop") {
            json& b = block(idx);
            if (b.value("type", "") == "tool_use") {
                json input = json::parse(partialJson_[idx].empty() ? "{}" : partialJson_[idx], nullptr, false);
                b["input"] = input.is_object() ? input : json::object();
            }
        } else if (type == "message_start") {
            if (j.contains("message") && j["message"].contains("usage")) {
                out.type = StreamEvent::Usage;
                out.inputTokens = j["message"]["usage"].value("input_tokens", 0);
            }
        } else if (type == "message_delta") {
            if (j.contains("delta") && j["delta"].value("stop_reason", "") == "refusal") {
                out.type = StreamEvent::Error;
                out.text = "The model declined to respond to this request.";
            } else if (j.contains("usage")) {
                out.type = StreamEvent::Usage;
                out.outputTokens = j["usage"].value("output_tokens", 0);
            }
        } else if (type == "message_stop") {
            out.type = StreamEvent::Done;
        } else if (type == "error") {
            out.type = StreamEvent::Error;
            const json& e = j.value("error", json::object());
            std::string etype = e.value("type", ""), msg = e.value("message", "unknown error");
            if (etype == "overloaded_error")
                out.text = "Provider overloaded, try again shortly: " + msg;
            else if (etype == "rate_limit_error")
                out.text = "Rate limited: " + msg;
            else
                out.text = "Provider error: " + msg;
        }
        return out;  // ping: nothing to do
    }

    TurnResult finishTurn() override {
        TurnResult r;
        for (const auto& b : blocks_)
            if (b.is_object() && b.value("type", "") == "tool_use")
                r.calls.push_back({b.value("id", ""), b.value("name", ""), b.value("input", json::object()).dump()});
        if (!r.calls.empty()) r.native = blocks_.dump(-1, ' ', false, json::error_handler_t::replace);
        return r;
    }

    HttpRequest modelsRequest(const Profile& p) const override {
        HttpRequest r;
        r.method = "GET";
        r.url = joinUrl(p.baseUrl, kDefaultBase, "/v1/models?limit=1000");
        r.headers = authHeaders(p);
        r.headers.insert(r.headers.end(), p.extraHeaders.begin(), p.extraHeaders.end());
        return r;
    }

    std::vector<ModelInfo> parseModels(const std::string& body) const override {
        std::vector<ModelInfo> v;
        json j = json::parse(body, nullptr, false);
        if (!j.is_object() || !j["data"].is_array()) return v;
        for (const auto& m : j["data"])
            if (m.is_object() && m.contains("id")) v.push_back({m.value("id", ""), m.value("display_name", "")});
        return v;
    }

private:
    static Headers authHeaders(const Profile& p) {
        return {{"x-api-key", p.apiKey}, {"anthropic-version", "2023-06-01"}};
    }
    json& block(int idx) {
        if (idx < 0 || idx > 1000) idx = 0;
        while ((int)blocks_.size() <= idx) blocks_.push_back(json::object());
        return blocks_[idx];
    }
    json blocks_ = json::array();  // the assistant turn's content blocks, as streamed
    std::map<int, std::string> partialJson_;
};
}  // namespace

std::unique_ptr<IProvider> makeAnthropicProvider() { return std::make_unique<AnthropicProvider>(); }
