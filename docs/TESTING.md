# Testing joymind

Everything below except the hardware checklist runs on a PC. CI runs the host
tests on every push (see `.github/workflows/build.yml`).

## Verifying it step by step

**Step 1, skeleton builds:** `make` prints `built ... ai-switch.nro` with no
warnings. In hbmenu the entry shows "joymind" by pouyakh.dev with the
sparkle icon.

**Step 2, networking + providers, no Switch needed:**

```sh
# terminal 1: fake Anthropic/OpenAI/Gemini endpoints with magic failure modes
python3 tests/mock_server.py 8808
# terminal 2: host build of the net/markdown code + tests (needs libcurl-dev, libssh2-dev)
g++ -std=c++17 -Isource -Isource/third_party tests/test_core.cpp source/net/sse.cpp source/net/http.cpp \
    source/net/ssh.cpp source/net/providers/*.cpp source/markdown/markdown.cpp -lcurl -lssh2 -lpthread -o test_core
./test_core http://127.0.0.1:8808
# SSH tests against a real sshd (password + ECDSA key login, host key pinning, cancel, truncation)
SSH_TEST_HOST=127.0.0.1 SSH_TEST_PORT=22 SSH_TEST_USER=me SSH_TEST_PASSWORD=... SSH_TEST_KEY=~/.ssh/id_ecdsa ./test_core
```

This covers SSE split at every byte, all three stream formats, `[DONE]` with
empty deltas, an Anthropic error event mid-stream, 401/404/429 (Retry-After)/529
mapping, wrong base URL, unreachable host, the http:// policy, Stop (cancel <2 s),
the same conversation continued on two other providers, model lists and Test
connection. It also runs the full tool loop on all three providers (the mock
asks for `uname`, and the second turn must contain the result and the unchanged
thinking/thought signatures). On the console, `nxlink -s` shows one `[net]` line
per request and one `[ssh]` line per command.

For SSH, link the tests against libssh2 1.10 built with mbedtls 2.28 (the same
versions devkitPro ships). Distro libssh2 uses OpenSSL and accepts key types
the Switch build can't.

**Step 3, full UI on a PC (same code, SDL desktop stand-ins for pad/swkbd):**

```sh
g++ -std=c++17 -O1 -Isource -Isource/third_party $(sdl2-config --cflags) source/*/*.cpp source/net/providers/*.cpp \
    -lSDL2 -lSDL2_ttf -lSDL2_image -lcurl -lssh2 -lpthread -o ai-switch-pc
./ai-switch-pc                 # 720p handheld layout; AI_SWITCH_DOCKED=1 for 1080p
```

Keys: arrows, Enter (A), Esc (B), PgUp/PgDn (L/R), F1 (+), F2 (−), F3 (X), F4 (Y),
mouse = touch, and typing goes straight into the message box (as with a USB
keyboard on the Switch). The on-screen keyboard (F4 / A on the message box)
reads a line from the terminal (`\n` = newline). Data goes to
`./ai-switch-data/`; put test screenshots in `./ai-switch-data/Album/`. Point a profile at `http://127.0.0.1:8808/anthropic`
with model `markdown` to see the Markdown demo stream in.

**Step 4, Markdown:** use the mock model `markdown`. It covers headings,
bold/italic, inline code, a link, nested lists, a quote, a fenced code block with
a long wrapped line, a rule and a very long URL. During streaming the open fence
already renders as a code box.

**Step 5, Gemini:** Gemini preset with your key, then Model (lists only
`generateContent` models), then Test connection.

## Hardware checklist

- [ ] **Offline:** turn on airplane mode and send. Expect "You're offline…". A LAN profile skips this check.
- [ ] **Wrong API key:** expect "Invalid API key or no access (HTTP 401): …" with the provider's message.
- [ ] **Wrong model:** expect "Not found (HTTP 404): check the model name and base URL: …".
- [ ] **Wrong base URL** (e.g. `https://api.anthropic.com/nope`, or a bad host): expect 404, or "Couldn't find <host>".
- [ ] **Streaming + Stop** on each type (anthropic, openai_compat, gemini). The arrow turns into a square; Stop keeps the partial text; a new message sends right after.
- [ ] **Anthropic error mid-stream:** mock model `midstream-error`. Partial text stays and an error card follows.
- [ ] **OpenAI-compatible `[DONE]` + empty deltas:** mock `/openai`. The text is complete and nothing follows `[DONE]`.
- [ ] **Rate limit:** mock model `ratelimit`. Expect "retry after 17s". **Overloaded:** model `overloaded` (529).
- [ ] **Two providers, one conversation:** chat on Anthropic, switch the chip to an OpenAI-compatible profile and continue. The reply should see the earlier turns.
- [ ] **LAN http://:** Ollama/LM Studio at `http://192.168.x.x:…/v1` works and shows the HTTP badge. `http://api.openai.com/v1` is refused at Save and at send.
- [ ] **Long code block, long unbroken URL:** both wrap inside the column with no overflow.
- [ ] **Many messages:** 50+ turns. Scroll with the stick, L/R, touch fling; the frame rate stays smooth.
- [ ] **Auto-scroll:** while streaming, scroll up. The view stays put; scroll back to the bottom and it follows again.
- [ ] **Docked ↔ handheld** during a reply: layout re-flows to 1080p/720p with sharp text.
- [ ] **Sleep/wake** during a reply: after wake, an error card (connection lost) appears or the reply finishes; the app keeps working.
- [ ] **HOME button:** suspend/resume works; closing from HOME exits cleanly.
- [ ] **Corrupt config:** put garbage in `config.json`. You get a notice, `config.json.bad`, and defaults.
- [ ] **Keys never shown:** Settings and the editor show only `••••` plus the last 4; nxlink output has no keys.
- [ ] **SSH, first connect:** Test connection shows the fingerprint. Cancel means nothing is sent; Trust pins it ("Host key pinned").
- [ ] **SSH, approval:** "please check disk space on my server" leads to a `df -h` card. Run shows output + exit code and the AI answers; Deny lets the AI continue without running; Always allow runs the next commands directly.
- [ ] **SSH, failures:** a wrong password, a missing `.pub`, a changed host key (reinstall the server) and an unreachable host each give a clear error card, and the loop stops.
- [ ] **SSH, Stop** during `sleep 100` returns within ~1 s and the card shows "Not run".
- [ ] **SSH on each provider type:** Anthropic, an OpenAI-compatible API with tool support, Gemini.
- [ ] **Screenshot:** take one in a game, attach it with **+**, ask "what's on screen?" on each provider (vision model). The thumbnail shows above your message and the reply describes it. An empty Album shows the System Memory hint.
- [ ] **USB keyboard (docked):** type, Backspace, Shift+Enter, then Enter to send. Without a keyboard nothing changes.
- [ ] **Game library:** "what have I played the most?" shows a Game library card with real names and hours. With the setting off, no tool is offered.
- [ ] **Rumble / awake:** a reply longer than 8 s gives a double rumble at the end, and the screen doesn't dim during a long reply.
