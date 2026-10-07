#!/usr/bin/env python3
"""Mock LLM server for testing without real API keys.

Serves Anthropic, OpenAI-compatible and Gemini streaming endpoints on
http://0.0.0.0:8808 under /anthropic, /openai and /gemini. Point a profile's
base URL at http://<pc-lan-ip>:8808/<provider> (http is allowed for LAN IPs).

Magic values: api key "bad" -> 401, model "nope" -> 404, model "ratelimit" -> 429,
model "overloaded" -> 529, model "midstream-error" -> error event mid-stream,
model "slow" -> one token every 0.2 s (test Stop), model "markdown" -> rich demo,
model "demo" (Anthropic) -> realistic answers and tool calls for screenshots.
"""
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEMO = """Here is a **quick tour** of what I can render, with *italic*, `inline code` and a [link](https://example.com).

## Lists
- First bullet
- Second bullet with a very long unbroken string: https://example.com/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
  1. nested numbered item

> A blockquote that spans
> two lines.

```python
def hello(name):
    print(f"Hello, {name}!")  # a fairly long comment that should wrap inside the code block nicely
```

---
That's all."""


# model "demo": questions that trigger a tool call, and canned answers.
DEMO_TOOLS = {"disk space": ("run_command", {"command": "df -h /"}),
              "played the most": ("get_game_library", {"sort_by": "play_time"})}


def demo_reply(last):
    low = last.lower()
    if "render" in low:
        return DEMO
    if low.startswith("result:") and "filesystem" in low:
        return ("Your root disk is **85% full**: 87 GB used and **16 GB free** out of 105 GB.\n\n"
                "That's fine for now. If you want to free some space, I can list the biggest folders "
                "with `du -sh /var/* | sort -h | tail` next.")
    if low.startswith("result:") and "games installed" in low:
        return ("Here's where your hours went:\n\n"
                "1. **Zelda: Tears of the Kingdom**: 142 h over 87 sessions\n"
                "2. **Stardew Valley**: 63 h\n"
                "3. **Mario Kart 8 Deluxe**: 25 h, but 140 launches, your go-to for quick races\n\n"
                "You haven't started **Hollow Knight** yet. It's a great pick for tonight if you want something new.")
    if "images:" in low:
        return ("This looks like a **pressure-plate puzzle**. The glowing ring is the exit, and the white tiles "
                "along the floor are the plates.\n\n"
                "1. Step on the plates from **left to right**.\n"
                "2. If a plate turns dark, start again from the first one.\n"
                "3. When the ring turns gold, stand in its centre to open the door.\n\n"
                "Stuck on the next room? Take another screenshot and attach it.")
    return "Happy to help! Ask me anything, attach a screenshot, or let me check your server."


DEMO_ALL = bool(os.environ.get("MOCK_DEMO"))  # every Anthropic model behaves like "demo"


def reply_text(model, last):
    if model == "demo" or DEMO_ALL:
        return demo_reply(last)
    if model == "markdown":
        return DEMO
    return f"You said: {last}. This reply comes from the mock server ({model})."


def chunks(text, n=6):
    return [text[i:i + n] for i in range(0, len(text), n)]


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("mock: " + fmt % args + "\n")

    def body(self):
        n = int(self.headers.get("Content-Length", 0))
        return json.loads(self.rfile.read(n) or b"{}")

    def send_json(self, code, obj, extra=None):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def start_sse(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

    def sse(self, data, event=None):
        out = (f"event: {event}\n" if event else "") + f"data: {data}\n\n"
        self.wfile.write(out.encode())
        self.wfile.flush()

    def check(self, key, model):
        if key == "bad":
            self.send_json(401, {"type": "error", "error": {"type": "authentication_error", "message": "invalid x-api-key"}})
        elif model == "nope":
            self.send_json(404, {"error": {"message": f"model '{model}' not found"}})
        elif model == "ratelimit":
            self.send_json(429, {"error": {"message": "slow down"}}, {"Retry-After": "17"})
        elif model == "overloaded":
            self.send_json(529, {"type": "error", "error": {"type": "overloaded_error", "message": "Overloaded"}})
        else:
            return True
        return False

    def redirect(self):
        self.send_response(302)
        self.send_header("Location", "https://example.com/")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        if self.path.startswith("/redir"):
            return self.redirect()
        if self.path.startswith("/anthropic/v1/models"):
            if self.headers.get("x-api-key") == "bad":
                return self.check("bad", "")
            return self.send_json(200, {"data": [{"id": m, "display_name": m.title()} for m in
                                                 ["mock-1", "markdown", "slow", "midstream-error", "nope", "ratelimit"]]})
        if self.path.startswith("/openai/models"):
            return self.send_json(200, {"data": [{"id": "mock-oai"}, {"id": "markdown"}, {"id": "slow"}]})
        if self.path.startswith("/gemini/v1beta/models"):
            return self.send_json(200, {"models": [{"name": "models/mock-gemini", "displayName": "Mock Gemini",
                                                    "supportedGenerationMethods": ["generateContent"]}]})
        self.send_json(404, {"error": {"message": "no such endpoint"}})

    def do_POST(self):
        b = self.body()
        if self.path.startswith("/redir"):
            return self.redirect()
        if self.path == "/anthropic/v1/messages":
            model = b.get("model", "")
            if not self.check(self.headers.get("x-api-key"), model):
                return
            last = b["messages"][-1]["content"]
            if isinstance(last, list):  # tool results (and maybe text) as blocks
                res = [blk for blk in last if blk.get("type") == "tool_result"]
                imgs = [blk for blk in last if blk.get("type") == "image"]
                last = "Result: " + res[0]["content"] if res else " ".join(blk.get("text", "") for blk in last)
                if imgs:
                    last = f"IMAGES:{len(imgs)} " + last
            self.start_sse()
            self.sse(json.dumps({"type": "message_start", "message": {"usage": {"input_tokens": 5}}}), "message_start")
            ev = lambda d, name: self.sse(json.dumps(d), name)
            demo = [t for k, t in DEMO_TOOLS.items() if (model == "demo" or DEMO_ALL) and b.get("tools") and k in last.lower()]
            if demo:
                name, args = demo[0]
                ev({"type": "content_block_start", "index": 0, "content_block": {"type": "thinking", "thinking": ""}}, "content_block_start")
                ev({"type": "content_block_delta", "index": 0, "delta": {"type": "signature_delta", "signature": "mock-sig"}}, "content_block_delta")
                ev({"type": "content_block_stop", "index": 0}, "content_block_stop")
                ev({"type": "content_block_start", "index": 1, "content_block": {"type": "tool_use", "id": "toolu_demo", "name": name, "input": {}}}, "content_block_start")
                ev({"type": "content_block_delta", "index": 1, "delta": {"type": "input_json_delta", "partial_json": json.dumps(args)}}, "content_block_delta")
                ev({"type": "content_block_stop", "index": 1}, "content_block_stop")
                ev({"type": "message_delta", "delta": {"stop_reason": "tool_use"}, "usage": {"output_tokens": 9}}, "message_delta")
                ev({"type": "message_stop"}, "message_stop")
                return
            if b.get("tools") and last.startswith("please list my games"):
                ev({"type": "content_block_start", "index": 0, "content_block": {"type": "tool_use", "id": "toolu_g", "name": "get_game_library", "input": {}}}, "content_block_start")
                ev({"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": '{"sort_by": "play_time"}'}}, "content_block_delta")
                ev({"type": "content_block_stop", "index": 0}, "content_block_stop")
                ev({"type": "message_delta", "delta": {"stop_reason": "tool_use"}, "usage": {"output_tokens": 9}}, "message_delta")
                ev({"type": "message_stop"}, "message_stop")
                return
            if b.get("tools") and last.startswith("please run "):
                ev({"type": "content_block_start", "index": 0, "content_block": {"type": "thinking", "thinking": ""}}, "content_block_start")
                ev({"type": "content_block_delta", "index": 0, "delta": {"type": "signature_delta", "signature": "mock-sig"}}, "content_block_delta")
                ev({"type": "content_block_stop", "index": 0}, "content_block_stop")
                ev({"type": "content_block_start", "index": 1, "content_block": {"type": "tool_use", "id": "toolu_1", "name": "run_command", "input": {}}}, "content_block_start")
                args = json.dumps({"command": last[len("please run "):]})
                for i in range(0, len(args), 5):
                    ev({"type": "content_block_delta", "index": 1, "delta": {"type": "input_json_delta", "partial_json": args[i:i + 5]}}, "content_block_delta")
                ev({"type": "content_block_stop", "index": 1}, "content_block_stop")
                ev({"type": "message_delta", "delta": {"stop_reason": "tool_use"}, "usage": {"output_tokens": 9}}, "message_delta")
                ev({"type": "message_stop"}, "message_stop")
                return
            if last.startswith("Result: ") and b["messages"][-2]["content"][-1].get("name") == "run_command":
                # the thinking block must come back unchanged before tool_use
                prev = b["messages"][-2]["content"]
                assert prev[0].get("signature") == "mock-sig", prev
            self.sse(json.dumps({"type": "ping"}), "ping")
            for i, c in enumerate(chunks(reply_text(model, last))):
                if model == "midstream-error" and i == 3:
                    self.sse(json.dumps({"type": "error", "error": {"type": "overloaded_error", "message": "Overloaded"}}), "error")
                    return
                self.sse(json.dumps({"type": "content_block_delta", "index": 0,
                                     "delta": {"type": "text_delta", "text": c}}), "content_block_delta")
                time.sleep(0.2 if model == "slow" else 0.01)
            self.sse(json.dumps({"type": "message_delta", "delta": {"stop_reason": "end_turn"}, "usage": {"output_tokens": 9}}), "message_delta")
            self.sse(json.dumps({"type": "message_stop"}), "message_stop")
        elif self.path == "/openai/chat/completions":
            model = b.get("model", "")
            key = (self.headers.get("Authorization") or "").replace("Bearer ", "")
            if not self.check(key, model):
                return
            if model == "notools" and b.get("tools"):
                return self.send_json(400, {"error": {"message": "This model does not support tools"}})
            last = b["messages"][-1]
            if last["role"] == "tool":
                last = "Result: " + last["content"]
            elif isinstance(last["content"], list):
                imgs = [p for p in last["content"] if p.get("type") == "image_url"]
                last = f"IMAGES:{len(imgs)} " + " ".join(p.get("text", "") for p in last["content"])
            else:
                last = last["content"]
            self.start_sse()
            self.wfile.write(b": keep-alive\n\n")
            if b.get("tools") and last.startswith("please run "):
                args = json.dumps({"command": last[len("please run "):]})
                self.sse(json.dumps({"choices": [{"delta": {"role": "assistant", "content": None, "tool_calls": [
                    {"index": 0, "id": "call_1", "type": "function", "function": {"name": "run_command", "arguments": ""}}]}}]}))
                for i in range(0, len(args), 4):
                    self.sse(json.dumps({"choices": [{"delta": {"tool_calls": [{"index": 0, "function": {"arguments": args[i:i + 4]}}]}}]}))
                self.sse(json.dumps({"choices": [{"delta": {}, "finish_reason": "tool_calls"}]}))
                self.sse("[DONE]")
                return
            self.sse(json.dumps({"choices": [{"delta": {"role": "assistant", "content": ""}}]}))
            for c in chunks(reply_text(model, last)):
                self.sse(json.dumps({"choices": [{"delta": {"content": c}}]}))
                self.sse(json.dumps({"choices": [{"delta": {}}]}))
                time.sleep(0.2 if model == "slow" else 0.01)
            self.sse("[DONE]")
        elif self.path.startswith("/gemini/v1beta/models/"):
            model = self.path.split("/models/")[1].split(":")[0]
            if not self.check(self.headers.get("x-goog-api-key"), model):
                return
            parts = b["contents"][-1]["parts"]
            part = parts[0]
            imgs = [p for p in parts if "inlineData" in p]
            if "functionResponse" in part:
                last = "Result: " + part["functionResponse"]["response"]["output"]
            else:
                last = (f"IMAGES:{len(imgs)} " if imgs else "") + " ".join(p.get("text", "") for p in parts)
            self.start_sse()
            if b.get("tools") and last.startswith("please run "):
                self.sse(json.dumps({"candidates": [{"content": {"role": "model", "parts": [
                    {"functionCall": {"name": "run_command", "args": {"command": last[len("please run "):]}}, "thoughtSignature": "ts"}]}}]}))
                return
            if last.startswith("Result: "):
                assert b["contents"][-2]["parts"][0].get("thoughtSignature") == "ts"
            for c in chunks(reply_text(model, last)):
                self.sse(json.dumps({"candidates": [{"content": {"parts": [{"text": c}], "role": "model"}}]}))
                time.sleep(0.2 if model == "slow" else 0.01)
        else:
            self.send_json(404, {"error": {"message": "no such endpoint"}})


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8808
    print(f"mock server on :{port}")
    ThreadingHTTPServer(("0.0.0.0", port), H).serve_forever()
