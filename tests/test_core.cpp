// Host-side checks for the pure logic (SSE, providers, URL policy, errors,
// Markdown). Build and run on a PC, see README "Host tests". With a URL
// argument it also drives the real curl + worker path against
// tests/mock_server.py:  ./test_core http://127.0.0.1:8808
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <cstdio>
#include <nlohmann/json.hpp>

#include "markdown/markdown.hpp"
#include "net/http.hpp"
#include "net/sse.hpp"
#include "net/ssh.hpp"

using json = nlohmann::json;

static Message msg(const char* role, const char* text) {
    Message m;
    m.role = role;
    m.text = text;
    return m;
}


// Feeds a raw stream through SSE + provider, split into chunks of `step`
// bytes, and returns the concatenated text plus the final event type.
static StreamEvent::Type run(const char* type, const std::string& raw, size_t step, std::string& text) {
    auto prov = makeProvider(type);
    SseParser sse;
    StreamEvent::Type last = StreamEvent::None;
    text.clear();
    auto cb = [&](const SseEvent& e) {
        if (last == StreamEvent::Done || last == StreamEvent::Error) return;
        StreamEvent se = prov->parseStreamChunk(e);
        if (se.type == StreamEvent::TextDelta) text += se.text;
        if (se.type == StreamEvent::Done || se.type == StreamEvent::Error) {
            last = se.type;
            if (se.type == StreamEvent::Error) text = se.text;
        }
    };
    for (size_t i = 0; i < raw.size(); i += step) sse.feed(raw.data() + i, std::min(step, raw.size() - i), cb);
    sse.finish(cb);
    return last;
}

static void testSse() {
    const std::string anth =
        "event: message_start\r\ndata: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":9}}}\r\n\r\n"
        "event: ping\ndata: {\"type\":\"ping\"}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
        "\"thinking_delta\",\"thinking\":\"hmm\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":"
        "\"text_delta\",\"text\":\"Hel\"}}\n\n"
        ": keep-alive\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":"
        "\"text_delta\",\"text\":\"lo \\u00e9\"}}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
        "\"usage\":{\"output_tokens\":3}}\n\n"
        "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    std::string text;
    for (size_t step : {1, 3, 7, 4096}) {
        assert(run("anthropic", anth, step, text) == StreamEvent::Done);
        assert(text == "Hello \xc3\xa9");
    }

    // Anthropic error event mid-stream
    const std::string anthErr =
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\","
        "\"text\":\"partial\"}}\n\n"
        "event: error\ndata: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}\n\n";
    assert(run("anthropic", anthErr, 5, text) == StreamEvent::Error);
    assert(text.find("overloaded") != std::string::npos && text.find("Overloaded") != std::string::npos);

    // OpenAI-compatible: empty deltas, role-only delta, comments, [DONE]
    const std::string oai =
        ": OPENROUTER PROCESSING\n\n"
        "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":null,\"reasoning\":\"x\"}}]}\n\n"
        "data:{\"choices\":[{\"delta\":{\"content\":\" there\"},\"finish_reason\":\"stop\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":2}}\n\n"
        "data: [DONE]\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"IGNORED\"}}]}\n\n";
    for (size_t step : {1, 2, 64}) {
        assert(run("openai_compat", oai, step, text) == StreamEvent::Done);
        assert(text == "Hi there");
    }
    assert(run("openai_compat", "data: {\"error\":{\"message\":\"bad things\"}}\n\n", 3, text) == StreamEvent::Error);
    assert(text.find("bad things") != std::string::npos);
    assert(run("openai_compat", "data: {not json\n\n", 3, text) == StreamEvent::Error);

    // Gemini: no end marker, parts concatenated, thoughts skipped
    const std::string gem =
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"think\",\"thought\":true},{\"text\":\"A\"},"
        "{\"text\":\"B\"}],\"role\":\"model\"}}]}\r\n\r\n"
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"C\"}]},\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"totalTokenCount\":5}}\r\n\r\n";
    assert(run("gemini", gem, 6, text) == StreamEvent::None);  // the transport adds Done at EOF
    assert(text == "ABC");

    // multi-line data fields are joined with \n
    SseParser p;
    std::string got;
    p.feed("data: a\ndata: b\n\n", 17, [&](const SseEvent& e) { got = e.data; });
    assert(got == "a\nb");
}

static void testRequests() {
    std::vector<Message> conv{msg("user", "one"), msg("user", "two"), msg("assistant", ""), msg("assistant", "ok"),
                              msg("user", "q")};
    Profile p;
    p.model = "m";
    p.apiKey = "sk-test";
    p.systemPrompt = "be brief";
    p.maxTokens = 100;

    p.type = "anthropic";
    HttpRequest r = makeProvider(p.type)->buildRequest(conv, p, {});
    json b = json::parse(r.body);
    assert(r.url == "https://api.anthropic.com/v1/messages");
    assert(b["system"] == "be brief" && b["max_tokens"] == 100 && b["stream"] == true);
    assert(b["messages"].size() == 3 && b["messages"][0]["content"][0]["text"] == "one\n\ntwo");
    assert(b["messages"][1]["role"] == "assistant" && !b.contains("temperature"));

    p.type = "openai_compat";
    p.baseUrl = "https://openrouter.ai/api/v1/";
    p.maxTokensField = "max_completion_tokens";
    p.hasTemperature = true;
    p.temperature = 0.5f;
    p.extraHeaders = {{"X-Title", "t"}};
    r = makeProvider(p.type)->buildRequest(conv, p, {});
    b = json::parse(r.body);
    assert(r.url == "https://openrouter.ai/api/v1/chat/completions");
    assert(b["messages"][0]["role"] == "system" && b["messages"].size() == 4);
    assert(b["max_completion_tokens"] == 100 && !b.contains("max_tokens") && b["temperature"] == 0.5);
    bool auth = false, xt = false;
    for (auto& h : r.headers) auth |= h.first == "Authorization" && h.second == "Bearer sk-test", xt |= h.first == "X-Title";
    assert(auth && xt);

    p.type = "gemini";
    p.baseUrl = "";
    p.model = "models/gemini-x";
    r = makeProvider(p.type)->buildRequest(conv, p, {});
    b = json::parse(r.body);
    assert(r.url ==
           "https://generativelanguage.googleapis.com/v1beta/models/gemini-x:streamGenerateContent?alt=sse");
    assert(b["contents"][1]["role"] == "model" && b["systemInstruction"]["parts"][0]["text"] == "be brief");
    assert(b["generationConfig"]["maxOutputTokens"] == 100);

    auto models = makeProvider("gemini")->parseModels(
        R"({"models":[{"name":"models/a","supportedGenerationMethods":["embedContent"]},)"
        R"({"name":"models/b","displayName":"B","supportedGenerationMethods":["generateContent"]}]})");
    assert(models.size() == 1 && models[0].id == "b");
    models = makeProvider("anthropic")->parseModels(R"({"data":[{"id":"claude-x","display_name":"X"}],"has_more":false})");
    assert(models.size() == 1 && models[0].name == "X");
}

static TurnResult runTurn(IProvider& prov, const std::string& raw, std::string& text) {
    SseParser sse;
    text.clear();
    sse.feed(raw.data(), raw.size(), [&](const SseEvent& e) {
        StreamEvent se = prov.parseStreamChunk(e);
        if (se.type == StreamEvent::TextDelta) text += se.text;
    });
    sse.finish([&](const SseEvent& e) { prov.parseStreamChunk(e); });
    return prov.finishTurn();
}

static void testTools() {
    // History repair: unanswered call gets a "not run" result, native kept only in the current loop.
    Message a1 = msg("assistant", "");
    a1.calls = {{"c1", "run_command", R"({"command":"ls"})"}};
    a1.native = R"([{"type":"thinking","thinking":"","signature":"sig1"}])";
    Message a2 = msg("assistant", "checking");
    a2.calls = {{"c2", "run_command", R"({"command":"df -h"})"}};
    a2.native = "[]";
    Message t2 = msg("tool", "Filesystem 1K");
    t2.callId = "c2";
    t2.toolName = "run_command";
    t2.exitCode = 0;
    std::vector<Message> h{msg("user", "hi"), a1, msg("user", "now disk"), a2, t2};
    auto n = normalizeHistory(h, true);
    assert(n.size() == 6 && n[2].role == "tool" && n[2].callId == "c1" && n[2].isError);
    assert(n[1].native.empty() && n[4].native == "[]");
    assert(toolResultText(n[5]) == "Exit code 0\nFilesystem 1K");
    auto flat = normalizeHistory(h, false);
    for (auto& m : flat) assert(m.calls.empty() && m.role != "tool");
    assert(flat.size() == 5 && flat[4].text.find("[command output]") != std::string::npos);
    assert(commandOf(a2.calls[0]) == "df -h" && commandOf({"x", "y", "not json"}).empty());

    std::string text;
    // Anthropic: thinking + signature + text + tool_use with streamed JSON input
    {
        auto prov = makeProvider("anthropic");
        auto turn = runTurn(*prov,
            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"SIG\"}}\n\n"
            "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Let me check.\"}}\n\n"
            "data: {\"type\":\"content_block_start\",\"index\":2,\"content_block\":{\"type\":\"tool_use\",\"id\":\"tu_1\",\"name\":\"run_command\",\"input\":{}}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"comm\"}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"and\\\": \\\"uptime\\\"}\"}}\n\n"
            "data: {\"type\":\"content_block_stop\",\"index\":2}\n\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}\n\n"
            "data: {\"type\":\"message_stop\"}\n\n", text);
        assert(text == "Let me check.");
        assert(turn.calls.size() == 1 && turn.calls[0].id == "tu_1" && commandOf(turn.calls[0]) == "uptime");
        json native = json::parse(turn.native);
        assert(native[0]["signature"] == "SIG" && native[2]["input"]["command"] == "uptime");

        Message asst = msg("assistant", "Let me check.");
        asst.calls = turn.calls;
        asst.native = turn.native;
        Message res = msg("tool", "up 3 days");
        res.callId = "tu_1";
        res.toolName = "run_command";
        res.exitCode = 0;
        Profile p;
        p.model = "m";
        auto req = makeProvider("anthropic")->buildRequest({msg("user", "uptime?"), asst, res}, p, {runCommandTool()});
        json b = json::parse(req.body);
        assert(b["tools"][0]["name"] == "run_command" && b["tools"][0]["input_schema"]["type"] == "object");
        assert(b["messages"].size() == 3 && b["messages"][1]["content"][0]["signature"] == "SIG");
        assert(b["messages"][2]["role"] == "user" && b["messages"][2]["content"][0]["type"] == "tool_result");
        assert(b["messages"][2]["content"][0]["tool_use_id"] == "tu_1");
        assert(b["messages"][2]["content"][0]["content"] == "Exit code 0\nup 3 days");
    }
    // OpenAI-compatible: arguments arrive in fragments
    {
        auto prov = makeProvider("openai_compat");
        auto turn = runTurn(*prov,
            "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"index\":0,\"id\":\"call_9\",\"type\":\"function\",\"function\":{\"name\":\"run_command\",\"arguments\":\"\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"command\\\":\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"\\\"whoami\\\"}\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
            "data: [DONE]\n\n", text);
        assert(turn.calls.size() == 1 && turn.calls[0].id == "call_9" && commandOf(turn.calls[0]) == "whoami");
        Message asst = msg("assistant", "");
        asst.calls = turn.calls;
        Message res = msg("tool", "root");
        res.callId = "call_9";
        res.exitCode = 0;
        Profile p;
        p.model = "m";
        json b = json::parse(makeProvider("openai_compat")->buildRequest({msg("user", "who"), asst, res}, p, {runCommandTool()}).body);
        assert(b["tools"][0]["function"]["name"] == "run_command");
        assert(b["messages"][1]["content"].is_null() && b["messages"][1]["tool_calls"][0]["function"]["arguments"] == turn.calls[0].args);
        assert(b["messages"][2]["role"] == "tool" && b["messages"][2]["tool_call_id"] == "call_9");
    }
    // Gemini: functionCall part with a thought signature
    {
        auto prov = makeProvider("gemini");
        auto turn = runTurn(*prov,
            "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"functionCall\":{\"name\":\"run_command\",\"args\":{\"command\":\"ls /\"}},\"thoughtSignature\":\"TS\"}]}}]}\n\n", text);
        assert(turn.calls.size() == 1 && turn.calls[0].id == "local_0" && commandOf(turn.calls[0]) == "ls /");
        Message asst = msg("assistant", "");
        asst.calls = turn.calls;
        asst.native = turn.native;
        Message res = msg("tool", "bin");
        res.callId = "local_0";
        res.toolName = "run_command";
        res.exitCode = 0;
        Profile p;
        p.model = "g";
        json b = json::parse(makeProvider("gemini")->buildRequest({msg("user", "ls"), asst, res}, p, {runCommandTool()}).body);
        assert(b["tools"][0]["functionDeclarations"][0]["name"] == "run_command");
        assert(b["contents"][1]["parts"][0]["thoughtSignature"] == "TS");
        assert(b["contents"][2]["parts"][0]["functionResponse"]["name"] == "run_command");
        assert(!b["contents"][2]["parts"][0]["functionResponse"].contains("id"));
    }
    // Images: each provider's own format, before the text
    Message shot = msg("user", "what is this?");
    shot.images = {"QUJD"};
    Message onlyImg = msg("user", "");
    onlyImg.images = {"QUJD"};
    assert(normalizeHistory({onlyImg}, false).size() == 1);
    Profile ip;
    ip.model = "m";
    json ab = json::parse(makeProvider("anthropic")->buildRequest({shot}, ip, {}).body);
    assert(ab["messages"][0]["content"][0]["type"] == "image" &&
           ab["messages"][0]["content"][0]["source"]["data"] == "QUJD" &&
           ab["messages"][0]["content"][1]["text"] == "what is this?");
    json ob = json::parse(makeProvider("openai_compat")->buildRequest({shot}, ip, {}).body);
    assert(ob["messages"][0]["content"][0]["image_url"]["url"] == "data:image/jpeg;base64,QUJD" &&
           ob["messages"][0]["content"][1]["type"] == "text");
    json gb = json::parse(makeProvider("gemini")->buildRequest({shot}, ip, {}).body);
    assert(gb["contents"][0]["parts"][0]["inlineData"]["data"] == "QUJD" && gb["contents"][0]["parts"][1]["text"] == "what is this?");
    assert(base64("abc") == "YWJj" && base64("ab") == "YWI=" && base64("a") == "YQ==" && base64("").empty());
    json gs = json::parse(gameLibraryTool().schema);
    assert(gs["properties"]["sort_by"]["enum"].size() == 3);
    assert(argOf({"x", "get_game_library", R"({"sort_by":"play_time"})"}, "sort_by") == "play_time");

    assert(net::cleanTerminalText("\x1b[31mred\x1b[0m\r\nline\rbar\xff!") == "red\nline\nbar\xEF\xBF\xBD!");
}

static void testPolicyAndErrors() {
    using net::UrlCheck;
    assert(net::checkUrl("https://api.anthropic.com") == UrlCheck::Ok);
    assert(net::checkUrl("http://192.168.1.20:11434/v1") == UrlCheck::Insecure);
    assert(net::checkUrl("http://10.0.0.5/v1") == UrlCheck::Insecure);
    assert(net::checkUrl("http://172.20.1.1") == UrlCheck::Insecure);
    assert(net::checkUrl("http://localhost:1234/v1") == UrlCheck::Insecure);
    assert(net::checkUrl("http://172.32.0.1") == UrlCheck::Rejected);
    assert(net::checkUrl("http://api.openai.com/v1") == UrlCheck::Rejected);
    assert(net::checkUrl("http://10.evil.com/v1") == UrlCheck::Rejected);
    assert(net::checkUrl("http://192.168.1.1.evil.com") == UrlCheck::Rejected);
    assert(net::checkUrl("http://user@192.168.1.1@evil.com/") == UrlCheck::Rejected);
    assert(net::checkUrl("ftp://192.168.1.1") == UrlCheck::Rejected);
    assert(net::checkUrl("api.openai.com") == UrlCheck::Rejected);

    assert(net::describeHttpError(401, "invalid x-api-key", "").find("Invalid API key") == 0);
    assert(net::describeHttpError(429, "", "20").find("retry after 20s") != std::string::npos);
    assert(net::describeHttpError(529, "Overloaded", "").find("overloaded") != std::string::npos);
    assert(net::describeHttpError(302, "", "").find("/v1") != std::string::npos);
    assert(extractErrorMessage(R"({"type":"error","error":{"type":"not_found_error","message":"model: x"}})") ==
           "model: x");
    assert(extractErrorMessage(R"([{"error":{"code":400,"message":"API key not valid"}}])") == "API key not valid");
    assert(extractErrorMessage(R"({"error":"plain"})") == "plain");
    assert(extractErrorMessage("<html>\nnope</html>") == "<html> nope</html>");
}

static void testMarkdown() {
    using md::BlockType;
    auto b = md::parse("# Title\n\nSome **bold** and *it* text\nnext line\n\n- a\n- b\n  more\n1. one\n\n"
                       "> quote\n> two\n\n---\n```py\nx = 1\n\n  y\n```\nafter");
    assert(b.size() == 9);
    assert(b[0].type == BlockType::Heading && b[0].level == 1 && b[0].text == "Title");
    assert(b[1].type == BlockType::Paragraph && b[1].text == "Some **bold** and *it* text\nnext line");
    assert(b[2].type == BlockType::Bullet && b[3].text == "b\nmore");
    assert(b[4].type == BlockType::Ordered && b[4].marker == "1.");
    assert(b[5].type == BlockType::Quote && b[5].text == "quote\ntwo");
    assert(b[6].type == BlockType::Rule);
    assert(b[7].type == BlockType::Code && b[7].lang == "py" && b[7].text == "x = 1\n\n  y" && !b[7].open);
    assert(b[8].text == "after");

    // streaming: unclosed fence renders as an open code block
    b = md::parse("Here:\n```cpp\nint main() {");
    assert(b.size() == 2 && b[1].type == BlockType::Code && b[1].open && b[1].text == "int main() {");

    auto in = md::parseInline("a **b** *c* `d*e` snake_case_name 2 * 3 [link](http://x) \\*lit\\* **open");
    std::string plain;
    for (auto& s : in) plain += s.text;
    assert(plain == "a b c d*e snake_case_name 2 * 3 link *lit* **open");
    assert(in[1].text == "b" && in[1].style == md::Bold);
    assert(in[3].text == "c" && in[3].style == md::Italic);
    assert(in[5].text == "d*e" && in[5].style == md::Code);
    bool link = false;
    for (auto& s : in) link |= s.text == "link" && (s.style & md::Link);
    assert(link);
}

// Waits for the job to finish; returns "DONE", "ERR:<message>" or "MODELS:<n>".
static net::Event g_last;  // last finishing event seen by wait()

static std::string wait(net::Worker& w, unsigned job, std::string& text, int cancelAfterMs = -1) {
    assert(job != 0);
    auto t0 = std::chrono::steady_clock::now();
    text.clear();
    for (;;) {
        net::Event e;
        while (w.poll(e)) {
            assert(e.job == job);
            if (e.type == net::Event::TextDelta) text += e.text;
            if (e.type != net::Event::TextDelta) g_last = e;
            if (e.type == net::Event::Error) return "ERR:" + e.text;
            if (e.type == net::Event::Models) return "MODELS:" + std::to_string(e.models.size());
            if (e.type == net::Event::Done) return "DONE" + (e.text.empty() ? "" : ":" + e.text);
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (cancelAfterMs >= 0 && ms > cancelAfterMs) w.cancel(), cancelAfterMs = -1;
        assert(ms < 20000);
        usleep(5000);
    }
}

static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

static void testNet(const std::string& base) {
    net::Worker w;
    std::string text, r;
    Profile a;
    a.type = "anthropic";
    a.baseUrl = base + "/anthropic";
    a.apiKey = "good";
    a.model = "mock-1";
    std::vector<Message> conv{msg("user", "hello")};

    r = wait(w, w.startChat(a, conv, {}), text);
    assert(r == "DONE" && has(text, "You said: hello"));
    std::string firstReply = text;

    Profile bad = a;
    bad.apiKey = "bad";
    r = wait(w, w.startChat(bad, conv, {}), text);
    assert(has(r, "Invalid API key") && has(r, "invalid x-api-key"));
    for (auto [model, expect] : {std::pair{"nope", "Not found"}, {"ratelimit", "retry after 17s"},
                                 {"overloaded", "overloaded"}}) {
        Profile p = a;
        p.model = model;
        r = wait(w, w.startChat(p, conv, {}), text);
        assert(has(r, expect));
    }
    Profile mid = a;
    mid.model = "midstream-error";
    r = wait(w, w.startChat(mid, conv, {}), text);
    assert(has(r, "overloaded") && !text.empty());

    // Stop: cancel a slow stream after 500 ms; the worker must finish promptly.
    Profile slow = a;
    slow.model = "slow";
    auto t0 = std::chrono::steady_clock::now();
    r = wait(w, w.startChat(slow, conv, {}), text, 500);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    assert(r == "DONE" && !text.empty() && ms < 2000);
    while (w.busy()) usleep(1000);

    Profile wrong = a;
    wrong.baseUrl = base + "/wrong";
    r = wait(w, w.startChat(wrong, conv, {}), text);
    assert(has(r, "Not found"));
    Profile redir = a;  // base URL missing /v1 on a server that redirects
    redir.baseUrl = base + "/redir";
    r = wait(w, w.startChat(redir, conv, {}), text);
    assert(has(r, "redirected (HTTP 302)"));
    Profile closed = a;
    closed.baseUrl = "http://127.0.0.1:1";
    r = wait(w, w.startChat(closed, conv, {}), text);
    assert(has(r, "Couldn't connect"));
    Profile pub = a;
    pub.baseUrl = "http://api.anthropic.com";
    r = wait(w, w.startChat(pub, conv, {}), text);
    assert(has(r, "only allowed for LAN"));

    // Same conversation continued on two other providers.
    std::vector<Message> conv2{msg("user", "hello"), msg("assistant", firstReply.c_str()), msg("user", "again")};
    Profile o;
    o.type = "openai_compat";
    o.baseUrl = base + "/openai";
    o.apiKey = "k";
    o.model = "mock-oai";
    r = wait(w, w.startChat(o, conv2, {}), text);
    assert(r == "DONE" && has(text, "You said: again"));
    Profile g;
    g.type = "gemini";
    g.baseUrl = base + "/gemini";
    g.apiKey = "k";
    g.model = "mock-gemini";
    r = wait(w, w.startChat(g, conv2, {}), text);
    assert(r == "DONE" && has(text, "You said: again"));

    // Full tool loop on each provider: the mock asks to run "uname", we answer
    // with a result, and the second turn must quote it back.
    for (Profile* prof : {&a, &o, &g}) {
        Profile pp = *prof;
        if (pp.type == "anthropic") pp.model = "mock-1";
        std::vector<Message> h{msg("user", "please run uname")};
        r = wait(w, w.startChat(pp, h, {runCommandTool()}), text);
        assert(r == "DONE" && g_last.calls.size() == 1 && commandOf(g_last.calls[0]) == "uname");
        Message asst = msg("assistant", text.c_str());
        asst.calls = g_last.calls;
        asst.native = g_last.native;
        Message res = msg("tool", "Linux");
        res.callId = g_last.calls[0].id;
        res.toolName = "run_command";
        res.exitCode = 0;
        h.push_back(asst);
        h.push_back(res);
        r = wait(w, w.startChat(pp, h, {runCommandTool()}), text);
        assert(r == "DONE" && has(text, "Result: Exit code 0") && has(text, "Linux"));
    }

    // An attached screenshot is read from disk on the worker and sent as an image block.
    {
        FILE* f = fopen("/tmp/test-shot.jpg", "wb");
        fwrite("FAKEJPEGBYTES", 1, 13, f);
        fclose(f);
        Message u = msg("user", "what is this");
        u.imagePaths = {"/tmp/test-shot.jpg", "/tmp/missing.jpg"};
        for (Profile* prof : {&a, &o, &g}) {
            r = wait(w, w.startChat(*prof, {u}, {}), text);
            assert(r == "DONE" && has(text, "IMAGES:1"));
        }
    }
    // A server that rejects `tools`: the worker retries without them.
    Profile nt = o;
    nt.model = "notools";
    r = wait(w, w.startChat(nt, conv, {runCommandTool()}), text);
    assert(r == "DONE" && has(text, "You said: hello"));

    r = wait(w, w.startModels(a), text);
    assert(r == "MODELS:6");
    r = wait(w, w.startTest(o), text);
    assert(r == "DONE:Connected: 3 models available.");
    r = wait(w, w.startTest(bad), text);
    assert(has(r, "Invalid API key"));
    std::puts("network tests passed");
}

// Needs a real sshd; see README "Host tests". Env: SSH_TEST_HOST, SSH_TEST_PORT,
// SSH_TEST_USER, SSH_TEST_PASSWORD, SSH_TEST_KEY (ECDSA), optional SSH_TEST_RSA_KEY
// and SSH_TEST_KBD_PORT (a server that only allows keyboard-interactive).
static void testSsh() {
    net::Machine m;
    m.host = getenv("SSH_TEST_HOST");
    m.port = atoi(getenv("SSH_TEST_PORT"));
    m.user = getenv("SSH_TEST_USER");
    m.auth = "password";
    m.password = getenv("SSH_TEST_PASSWORD");

    auto r = net::sshExec(m, "echo never", nullptr);  // first contact: nothing runs before the key is trusted
    assert(r.status == net::SshResult::UnknownHostKey && r.fingerprint.rfind("SHA256:", 0) == 0);
    if (const char* fp = getenv("SSH_TEST_FINGERPRINT")) assert(r.fingerprint == fp);
    m.hostKey = r.fingerprint;

    r = net::sshExec(m, "echo hello; echo oops >&2; printf '\\033[1mbold\\033[0m \\303\\251\\n'; exit 3", nullptr);
    std::printf("exec output: [%s] exit=%d\n", r.output.c_str(), r.exitCode);
    // stdout and stderr travel as separate SSH streams, so their order may vary
    assert(r.status == net::SshResult::Ok && r.exitCode == 3);
    assert(has(r.output, "hello\n") && has(r.output, "oops\n") && has(r.output, "bold \xC3\xA9\n") &&
           r.output.size() == strlen("hello\noops\nbold \xC3\xA9\n"));

    r = net::sshExec(m, "head -c 100000 /dev/zero | tr '\\0' a", nullptr);
    assert(r.status == net::SshResult::Ok && r.output.size() < net::kMaxCommandOutput + 64 &&
           r.output.find("[output truncated at 16 KB]") != std::string::npos);

    std::atomic<bool> cancel{false};
    auto t0 = std::chrono::steady_clock::now();
    std::thread stopper([&] { usleep(600000); cancel = true; });
    r = net::sshExec(m, "sleep 30", &cancel);
    stopper.join();
    assert(r.status == net::SshResult::Failed && r.error == "Cancelled.");
    assert(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));

    net::Machine wrong = m;
    wrong.password = "nope";
    r = net::sshExec(wrong, "true", nullptr);
    assert(r.status == net::SshResult::Failed && has(r.error, "Wrong user name or password"));

    net::Machine changed = m;
    changed.hostKey = "SHA256:AAAA";
    r = net::sshExec(changed, "true", nullptr);
    assert(r.status == net::SshResult::HostKeyChanged && has(r.error, "changed"));

    net::Machine key = m;
    key.auth = "key";
    key.keyPath = getenv("SSH_TEST_KEY");
    r = net::sshExec(key, "whoami", nullptr);
    std::printf("ECDSA key login: %s\n", r.status == net::SshResult::Ok ? "works" : r.error.c_str());
    fflush(stdout);
    assert(r.status == net::SshResult::Ok && r.output == m.user + "\n" && r.exitCode == 0);

    key.keyPath = "/nonexistent/key";
    r = net::sshExec(key, "true", nullptr);
    assert(r.status == net::SshResult::Failed && has(r.error, "key file") && has(r.error, ".pub"));

    if (const char* rsa = getenv("SSH_TEST_RSA_KEY")) {
        key.keyPath = rsa;
        r = net::sshExec(key, "whoami", nullptr);
        std::printf("RSA key login: %s\n", r.status == net::SshResult::Ok ? "works" : r.error.c_str());
    }
    if (const char* kp = getenv("SSH_TEST_KBD_PORT")) {
        net::Machine kbd = m;
        kbd.port = atoi(kp);
        kbd.hostKey = net::sshExec(kbd, "true", nullptr).fingerprint;
        r = net::sshExec(kbd, "echo kbd-ok", nullptr);
        assert(r.status == net::SshResult::Ok && r.output == "kbd-ok\n");
    }
    std::puts("ssh tests passed");
}

int main(int argc, char** argv) {
    testSse();
    testRequests();
    testPolicyAndErrors();
    testMarkdown();
    testTools();
    std::puts("all core tests passed");
    if (argc > 1) testNet(argv[1]);
    if (getenv("SSH_TEST_HOST")) {
        net::sshGlobalInit();
        testSsh();
    }
    return 0;
}
