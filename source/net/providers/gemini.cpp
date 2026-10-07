// Google Gemini: POST {base}/v1beta/models/{model}:streamGenerateContent?alt=sse
#include "net/providers/provider.hpp"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {
const char* kDefaultBase = "https://generativelanguage.googleapis.com";

std::string bareModel(const std::string& m) { return m.rfind("models/", 0) == 0 ? m.substr(7) : m; }

class GeminiProvider : public IProvider {
public:
    HttpRequest buildRequest(const std::vector<Message>& conv, const Profile& p,
                             const std::vector<ToolSpec>& tools) const override {
        json contents = json::array();
        for (const auto& m : normalizeHistory(conv, !tools.empty())) {
            std::string role = m.role == "assistant" ? "model" : "user";
            json parts = m.native.empty() ? json::array() : json::parse(m.native, nullptr, false);
            if (m.role == "tool") {
                json fr = {{"name", m.toolName},
                           {"response", {{"output", toolResultText(m)}, {"error", m.isError}}}};
                if (m.callId.rfind("local_", 0) != 0) fr["id"] = m.callId;  // ids we made up stay local
                parts.push_back({{"functionResponse", fr}});
            } else if (!parts.is_array() || parts.empty()) {
                // neutral form; otherwise `native` (parts with thought signatures) is sent unchanged
                parts = json::array();
                for (const auto& img : m.images)
                    parts.push_back({{"inlineData", {{"mimeType", "image/jpeg"}, {"data", img}}}});
                if (!m.text.empty()) parts.push_back({{"text", m.text}});
                for (const auto& c : m.calls) {
                    json args = json::parse(c.args, nullptr, false);
                    json fc = {{"name", c.name}, {"args", args.is_object() ? args : json::object()}};
                    if (c.id.rfind("local_", 0) != 0) fc["id"] = c.id;
                    parts.push_back({{"functionCall", fc}});
                }
            }
            if (!contents.empty() && contents.back()["role"] == role)
                for (auto& part : parts) contents.back()["parts"].push_back(part);
            else
                contents.push_back({{"role", role}, {"parts", parts}});
        }
        json body = {{"contents", contents}};
        if (!p.systemPrompt.empty())
            body["systemInstruction"] = {{"parts", json::array({{{"text", p.systemPrompt}}})}};
        json gen = json::object();
        if (p.maxTokens > 0) gen["maxOutputTokens"] = p.maxTokens;
        if (p.hasTemperature) gen["temperature"] = p.temperature;
        if (!gen.empty()) body["generationConfig"] = gen;
        if (!tools.empty()) {
            json decl = json::array();
            for (const auto& s : tools)
                decl.push_back({{"name", s.name}, {"description", s.description}, {"parameters", json::parse(s.schema)}});
            body["tools"] = json::array({{{"functionDeclarations", decl}}});
        }

        HttpRequest r;
        r.url = joinUrl(p.baseUrl, kDefaultBase,
                        "/v1beta/models/" + bareModel(p.model) + ":streamGenerateContent?alt=sse");
        r.body = body.dump(-1, ' ', false, json::error_handler_t::replace);
        r.headers = {{"x-goog-api-key", p.apiKey}, {"Content-Type", "application/json"}};
        r.headers.insert(r.headers.end(), p.extraHeaders.begin(), p.extraHeaders.end());
        return r;
    }

    // Gemini has no explicit end marker: the HTTP stream simply ends, and the
    // transport layer turns a clean end of stream into Done.
    StreamEvent parseStreamChunk(const SseEvent& ev) override {
        StreamEvent out;
        json j = json::parse(ev.data, nullptr, false);
        if (j.is_array() && !j.empty()) j = j[0];
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
        if (j.contains("promptFeedback") && j["promptFeedback"].contains("blockReason")) {
            out.type = StreamEvent::Error;
            out.text = "Prompt blocked by the provider (" + j["promptFeedback"].value("blockReason", "") + ")";
            return out;
        }
        if (j.contains("candidates") && j["candidates"].is_array() && !j["candidates"].empty()) {
            const json& c = j["candidates"][0];
            if (c.contains("content") && c["content"].contains("parts") && c["content"]["parts"].is_array())
                for (const auto& part : c["content"]["parts"]) {
                    if (!part.is_object()) continue;
                    parts_.push_back(part);  // kept with thought signatures for the tool loop
                    if (part.contains("text") && part["text"].is_string() && !part.value("thought", false))
                        out.text += part["text"].get<std::string>();
                }
            if (!out.text.empty()) {
                out.type = StreamEvent::TextDelta;
            } else if (c.value("finishReason", "") == "SAFETY") {
                out.type = StreamEvent::Error;
                out.text = "Response blocked by the provider's safety filters.";
            }
        }
        return out;
    }

    TurnResult finishTurn() override {
        TurnResult r;
        for (const auto& part : parts_) {
            if (!part.contains("functionCall") || !part["functionCall"].is_object()) continue;
            const json& fc = part["functionCall"];
            std::string id = fc.contains("id") && fc["id"].is_string() ? fc["id"].get<std::string>()
                                                                        : "local_" + std::to_string(r.calls.size());
            r.calls.push_back({id, fc.value("name", ""), fc.value("args", json::object()).dump()});
        }
        if (!r.calls.empty()) r.native = parts_.dump(-1, ' ', false, json::error_handler_t::replace);
        return r;
    }

    HttpRequest modelsRequest(const Profile& p) const override {
        HttpRequest r;
        r.method = "GET";
        r.url = joinUrl(p.baseUrl, kDefaultBase, "/v1beta/models?pageSize=1000");
        r.headers = {{"x-goog-api-key", p.apiKey}};
        r.headers.insert(r.headers.end(), p.extraHeaders.begin(), p.extraHeaders.end());
        return r;
    }

    std::vector<ModelInfo> parseModels(const std::string& body) const override {
        std::vector<ModelInfo> v;
        json j = json::parse(body, nullptr, false);
        if (!j.is_object() || !j["models"].is_array()) return v;
        for (const auto& m : j["models"]) {
            if (!m.is_object() || !m.contains("name") || !m["name"].is_string()) continue;
            bool chat = !m.contains("supportedGenerationMethods");
            if (!chat)
                for (const auto& g : m["supportedGenerationMethods"]) chat = chat || g == "generateContent";
            if (chat) v.push_back({bareModel(m["name"].get<std::string>()), m.value("displayName", "")});
        }
        return v;
    }

private:
    json parts_ = json::array();  // the model turn's parts, as streamed
};
}  // namespace

std::unique_ptr<IProvider> makeGeminiProvider() { return std::make_unique<GeminiProvider>(); }
