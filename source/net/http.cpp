#include "net/http.hpp"

#include <curl/curl.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "net/sse.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

#ifndef APP_VERSION_STR
#define APP_VERSION_STR "dev"
#endif

namespace net {
namespace {

// Cancel flag of the job running on the current worker thread (null elsewhere).
thread_local const std::atomic<bool>* tl_cancel = nullptr;

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool parseUrl(const std::string& url, std::string& scheme, std::string& host) {
    size_t p = url.find("://");
    if (p == std::string::npos) return false;
    scheme = lower(url.substr(0, p));
    size_t start = p + 3, end = url.find_first_of("/?#", start);
    std::string auth = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    size_t at = auth.rfind('@');
    if (at != std::string::npos) auth = auth.substr(at + 1);
    if (!auth.empty() && auth[0] == '[')
        host = lower(auth.substr(1, auth.find(']') - 1));
    else
        host = lower(auth.substr(0, auth.find(':')));
    return !host.empty();
}

bool parseIPv4(const std::string& h, int o[4]) {
    int n = 0;
    size_t i = 0;
    while (n < 4) {
        size_t j = i;
        int v = 0;
        while (j < h.size() && isdigit((unsigned char)h[j]) && j - i < 3) v = v * 10 + (h[j++] - '0');
        if (j == i || v > 255) return false;
        o[n++] = v;
        if (n < 4) {
            if (j >= h.size() || h[j] != '.') return false;
            j++;
        } else if (j != h.size()) {
            return false;
        }
        i = j;
    }
    return true;
}

bool isOnline() {
#ifdef __SWITCH__
    NifmInternetConnectionType type;
    u32 strength = 0;
    NifmInternetConnectionStatus status;
    Result rc = nifmGetInternetConnectionStatus(&type, &strength, &status);
    return R_SUCCEEDED(rc) && status == NifmInternetConnectionStatus_Connected;
#else
    return true;
#endif
}

// Checks the URL policy and connectivity. LAN servers skip the internet check
// because the console may report "no internet" on a LAN-only network.
bool preflight(const std::string& url, std::string& err) {
    if (checkUrl(url, &err) == UrlCheck::Rejected) return false;
    if (!isPrivateHost(hostOf(url)) && !isOnline()) {
        err = "You're offline. Connect to the internet and try again.";
        return false;
    }
    return true;
}

struct Transfer {
    long status = 0;
    std::string retryAfter, errBody;
    std::function<bool(const char*, size_t)> onData;
    bool stoppedByUs = false;
};

size_t headerCb(char* b, size_t sz, size_t n, void* ud) {
    auto* t = static_cast<Transfer*>(ud);
    size_t len = sz * n;
    if (len > 5 && strncmp(b, "HTTP/", 5) == 0) {
        const char* sp = (const char*)memchr(b, ' ', len);
        t->status = sp ? strtol(sp + 1, nullptr, 10) : 0;
        t->retryAfter.clear();
    } else if (len > 12 && strncasecmp(b, "retry-after:", 12) == 0) {
        std::string v(b + 12, len - 12);
        size_t s = v.find_first_not_of(" \t"), e = v.find_last_not_of(" \t\r\n");
        t->retryAfter = s == std::string::npos ? "" : v.substr(s, e - s + 1);
    }
    return len;
}

size_t writeCb(char* b, size_t sz, size_t n, void* ud) {
    auto* t = static_cast<Transfer*>(ud);
    size_t len = sz * n;
    if (t->status >= 300) {  // error/redirect body: keep it for the message, don't parse as SSE
        if (t->errBody.size() < 65536) t->errBody.append(b, len);
        return len;
    }
    if (!t->onData(b, len)) {
        t->stoppedByUs = true;
        return 0;  // makes curl abort with CURLE_WRITE_ERROR
    }
    return len;
}

int progressCb(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return tl_cancel && tl_cancel->load() ? 1 : 0;  // non-zero aborts the transfer
}

struct Result {
    CURLcode code = CURLE_OK;
    long status = 0;
    std::string retryAfter, errBody;
    bool stoppedByUs = false;
};

Result perform(const HttpRequest& req, bool streaming, std::function<bool(const char*, size_t)> onData) {
    Transfer t;
    t.onData = std::move(onData);
    Result r;
    CURL* c = curl_easy_init();
    if (!c) {
        r.code = CURLE_FAILED_INIT;
        return r;
    }
    curl_slist* hdrs = curl_slist_append(nullptr, "Expect:");
    for (const auto& h : req.headers) {
        if (h.first.empty() || (h.first + h.second).find_first_of("\r\n") != std::string::npos) continue;
        hdrs = curl_slist_append(hdrs, (h.first + ": " + h.second).c_str());
    }
    curl_easy_setopt(c, CURLOPT_URL, req.url.c_str());
    if (req.method == "POST") {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
    } else {
        curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");  // model lists can be large
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "joymind/" APP_VERSION_STR);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    // Idle timeout: abort if less than 1 byte/s arrives for 3 minutes. Reasoning
    // models can stay quiet for a long time before the first token.
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, streaming ? 180L : 30L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, headerCb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &t);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progressCb);

    r.code = curl_easy_perform(c);
    r.status = t.status;
    r.retryAfter = t.retryAfter;
    r.errBody = std::move(t.errBody);
    r.stoppedByUs = t.stoppedByUs;
    // Log host + status only: never URLs with queries, headers or bodies (keys!).
    std::printf("[net] %s %s -> HTTP %ld, curl %d\n", req.method.c_str(), hostOf(req.url).c_str(), r.status,
                (int)r.code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

// Certificate checks include the date, so a console clock that never synced
// (common with DNS blocking of Nintendo servers) fails every HTTPS request.
std::string clockInfo() {
    auto fmt = [](time_t t) {
        tm g;
        gmtime_r(&t, &g);
        char b[32];
        strftime(b, sizeof b, "%Y-%m-%d", &g);
        return std::string(b);
    };
#ifdef __SWITCH__
    u64 net = 0, user = 0;
    bool okNet = R_SUCCEEDED(timeGetCurrentTime(TimeType_NetworkSystemClock, &net));
    bool okUser = R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock, &user));
    return " Console clock: " + (okNet ? fmt((time_t)net) : std::string("not set")) + " (network), " +
           (okUser ? fmt((time_t)user) : std::string("not set")) +
           " (user). If that is not today, set the date in System Settings > System > Date and Time.";
#else
    return " Clock: " + fmt(time(nullptr)) + ".";
#endif
}

std::string describeCurlError(CURLcode code, const std::string& host) {
    switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
        return "Couldn't find " + host + ". Check the base URL and your connection.";
    case CURLE_COULDNT_CONNECT:
        return "Couldn't connect to " + host + ". Is the server running and reachable?";
    case CURLE_OPERATION_TIMEDOUT:
        return "The request timed out: no data from " + host + ".";
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
        return "Secure connection to " + host + " failed: the certificate could not be verified (curl " +
               std::to_string((int)code) + "). Most often the console's date is wrong." + clockInfo();
    case CURLE_SSL_CONNECT_ERROR:
        return "Secure connection to " + host + " failed during the TLS handshake (curl 35). The console "
               "supports TLS 1.2; check that the server accepts it.";
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
        return "The connection to " + host + " was lost.";
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
        return "The base URL is not valid.";
    default:
        return std::string("Network error: ") + curl_easy_strerror(code);
    }
}

// Streams one chat completion. onText receives each text delta; returns an
// empty string on success or a readable error.
std::string streamChat(IProvider& prov, const Profile& p, const std::vector<Message>& hist,
                       const std::vector<ToolSpec>& tools, const std::function<void(const std::string&)>& onText,
                       TurnResult* turn = nullptr) {
    HttpRequest req = prov.buildRequest(hist, p, tools);
    std::string err;
    if (!preflight(req.url, err)) return err;

    SseParser sse;
    bool finished = false;
    size_t events = 0;
    std::string head;  // first bytes, to explain non-SSE responses
    auto handle = [&](const SseEvent& e) {
        if (finished) return;
        events++;
        StreamEvent se;
        try {
            se = prov.parseStreamChunk(e);
        } catch (const std::exception&) {
            se.type = StreamEvent::Error;
            se.text = "Malformed stream data from provider";
        }
        if (se.type == StreamEvent::TextDelta) {
            onText(se.text);
        } else if (se.type == StreamEvent::Done) {
            finished = true;
        } else if (se.type == StreamEvent::Error) {
            finished = true;
            err = se.text;
        }
    };
    Result r = perform(req, true, [&](const char* d, size_t n) {
        if (head.size() < 2048) head.append(d, std::min(n, 2048 - head.size()));
        sse.feed(d, n, handle);
        return !finished;
    });
    if (tl_cancel && tl_cancel->load()) return "";
    if (finished) {
        if (err.empty() && turn) *turn = prov.finishTurn();
        return err;
    }
    if (r.status >= 300) return describeHttpError(r.status, extractErrorMessage(r.errBody), r.retryAfter);
    if (r.code != CURLE_OK) return describeCurlError(r.code, hostOf(req.url));
    sse.finish(handle);
    if (finished && !err.empty()) return err;
    if (events == 0) {
        std::string s = "Unexpected response from the server";
        if (r.status != 200) s += " (HTTP " + std::to_string(r.status) + ")";
        std::string m = extractErrorMessage(head);
        return m.empty() ? s + "." : s + ": " + m;
    }
    if (turn) *turn = prov.finishTurn();  // also when the stream just ended (Gemini has no end marker)
    return "";
}

}  // namespace

void globalInit() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    sshGlobalInit();
}
void globalCleanup() {
    sshGlobalCleanup();
    curl_global_cleanup();
}

std::string hostOf(const std::string& url) {
    std::string scheme, host;
    parseUrl(url, scheme, host);
    return host;
}

bool isPrivateHost(const std::string& host) {
    if (host == "localhost" || host == "::1") return true;
    int o[4];
    if (!parseIPv4(host, o)) return false;
    return o[0] == 10 || o[0] == 127 || (o[0] == 192 && o[1] == 168) || (o[0] == 172 && o[1] >= 16 && o[1] <= 31);
}

UrlCheck checkUrl(const std::string& url, std::string* why) {
    std::string scheme, host, msg;
    UrlCheck res = UrlCheck::Rejected;
    if (!parseUrl(url, scheme, host))
        msg = "The base URL must look like https://host/path.";
    else if (scheme == "https")
        res = UrlCheck::Ok;
    else if (scheme == "http" && isPrivateHost(host))
        res = UrlCheck::Insecure;
    else if (scheme == "http")
        msg = "Plain http:// is only allowed for LAN addresses (10.x, 172.16-31.x, 192.168.x, localhost). "
              "Use https:// for " + host + ".";
    else
        msg = "Only https:// URLs are supported.";
    if (why) *why = msg;
    return res;
}

std::string describeHttpError(long st, const std::string& msg, const std::string& retryAfter) {
    std::string code = std::to_string(st), s;
    if (st == 401 || st == 403)
        s = "Invalid API key or no access (HTTP " + code + ")";
    else if (st == 404)
        s = "Not found (HTTP 404): check the model name and base URL";
    else if (st == 429) {
        s = "Rate limited (HTTP 429)";
        if (!retryAfter.empty())
            s += ", retry after " + retryAfter +
                 (retryAfter.find_first_not_of("0123456789") == std::string::npos ? "s" : "");
    } else if (st == 529 || st == 502 || st == 503 || st == 504)
        s = "Provider overloaded or unavailable (HTTP " + code + "), try again shortly";
    else if (st >= 500)
        s = "Provider error (HTTP " + code + "), try again shortly";
    else if (st >= 300 && st < 400)
        return "The server redirected (HTTP " + code + "): check the base URL. OpenAI-compatible APIs usually end in /v1.";
    else if (st == 400 || st == 422)
        s = "Request rejected (HTTP " + code + ")";
    else
        s = "HTTP error " + code;
    return msg.empty() ? s + "." : s + ": " + msg;
}

}  // namespace net

std::vector<ModelInfo> IProvider::listModels(const Profile& p, std::string& error) const {
    HttpRequest req = modelsRequest(p);
    if (!net::preflight(req.url, error)) return {};
    std::string body;
    net::Result r = net::perform(req, false, [&](const char* d, size_t n) {
        if (body.size() + n > (16u << 20)) return false;
        body.append(d, n);
        return true;
    });
    if (r.status >= 300) {
        error = net::describeHttpError(r.status, extractErrorMessage(r.errBody), r.retryAfter);
        return {};
    }
    if (r.code != CURLE_OK) {
        error = r.stoppedByUs ? "The model list is too large." : net::describeCurlError(r.code, net::hostOf(req.url));
        return {};
    }
    std::vector<ModelInfo> models;
    try {
        models = parseModels(body);
    } catch (const std::exception&) {
    }
    if (models.empty()) error = "The server returned no models. You can still type a model name.";
    return models;
}

namespace net {

Worker::~Worker() {
    cancel_ = true;
    if (joinable_) pthread_join(thread_, nullptr);
}

void* Worker::trampoline(void* self) {
    auto* w = static_cast<Worker*>(self);
    tl_cancel = &w->cancel_;
    w->job_();
    w->running_ = false;
    return nullptr;
}

unsigned Worker::start(std::function<void(unsigned)> fn) {
    if (running_) return 0;
    if (joinable_) pthread_join(thread_, nullptr);  // already finished: returns at once
    joinable_ = false;
    cancel_ = false;
    running_ = true;
    unsigned id = nextJob_++;
    job_ = [fn, id] { fn(id); };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);  // explicit: default thread stacks are small on Switch
    if (pthread_create(&thread_, &attr, trampoline, this) != 0) {
        running_ = false;
        pthread_attr_destroy(&attr);
        return 0;
    }
    pthread_attr_destroy(&attr);
    joinable_ = true;
    return id;
}

void Worker::push(Event e) {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.push_back(std::move(e));
}

bool Worker::poll(Event& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

unsigned Worker::startChat(const Profile& p, const std::vector<Message>& history, const std::vector<ToolSpec>& tools) {
    return start([this, p, history, tools](unsigned job) {
        auto prov = makeProvider(p.type);
        // Attached screenshots are read here, off the UI thread. A missing
        // file (deleted from the Album) is simply left out.
        std::vector<Message> hist = history;
        for (auto& m : hist)
            for (const auto& path : m.imagePaths) {
                std::string bytes;
                if (FILE* f = fopen(path.c_str(), "rb")) {
                    char buf[16384];
                    size_t n;
                    while ((n = fread(buf, 1, sizeof buf, f)) > 0) bytes.append(buf, n);
                    fclose(f);
                }
                if (!bytes.empty()) m.images.push_back(base64(bytes));
            }
        TurnResult turn;
        bool gotText = false;
        auto onText = [&](const std::string& t) {
            gotText = true;
            Event e;
            e.type = Event::TextDelta;
            e.text = t;
            e.job = job;
            push(std::move(e));
        };
        std::string err = streamChat(*prov, p, hist, tools, onText, &turn);
        // Some OpenAI-compatible servers/models reject `tools`: retry once without.
        std::string low = lower(err);
        if (!err.empty() && !gotText && !tools.empty() && low.find("tool") != std::string::npos &&
            !(tl_cancel && tl_cancel->load())) {
            prov = makeProvider(p.type);
            err = streamChat(*prov, p, hist, {}, onText, &turn);
        }
        Event e;
        e.type = err.empty() ? Event::Done : Event::Error;
        e.text = err;
        e.calls = std::move(turn.calls);
        e.native = std::move(turn.native);
        e.job = job;
        push(std::move(e));
    });
}

unsigned Worker::startTask(std::function<std::string()> fn) {
    return start([this, fn](unsigned job) {
        Event e;
        e.job = job;
        e.text = fn();
        push(std::move(e));
    });
}

unsigned Worker::startCommand(const Machine& m, const std::string& command) {
    return start([this, m, command](unsigned job) {
        SshResult r = sshExec(m, command, tl_cancel);
        Event e;
        e.job = job;
        e.exitCode = r.exitCode;
        if (r.status == SshResult::Ok) {
            e.type = Event::CommandDone;
            e.text = r.output;
        } else if (r.status == SshResult::UnknownHostKey) {
            e.type = Event::HostKeyUnknown;
            e.text = r.fingerprint;
        } else {
            e.type = Event::Error;
            e.text = r.error;
        }
        std::printf("[ssh] %s -> %s\n", m.host.c_str(), r.status == SshResult::Ok ? "ok" : "failed");
        push(std::move(e));
    });
}

unsigned Worker::startModels(const Profile& p) {
    return start([this, p](unsigned job) {
        Event e;
        e.job = job;
        e.models = makeProvider(p.type)->listModels(p, e.text);
        e.type = e.models.empty() ? Event::Error : Event::Models;
        push(std::move(e));
    });
}

// "Test connection": list models; if that fails but a model is set, fall back
// to a tiny chat request (some compatible servers have no /models endpoint).
unsigned Worker::startTest(const Profile& p) {
    return start([this, p](unsigned job) {
        auto prov = makeProvider(p.type);
        Event e;
        e.job = job;
        std::string listErr;
        auto models = prov->listModels(p, listErr);
        if (!models.empty()) {
            e.text = "Connected: " + std::to_string(models.size()) + " models available.";
        } else if (!p.model.empty()) {
            Profile tiny = p;
            tiny.maxTokens = 16;
            Message hi;
            hi.role = "user";
            hi.text = "Hi";
            std::string err = streamChat(*prov, tiny, {hi}, {}, [](const std::string&) {});
            e.type = err.empty() ? Event::Done : Event::Error;
            e.text = err.empty() ? "Connected: model \"" + p.model + "\" replied." : err;
        } else {
            e.type = Event::Error;
            e.text = listErr;
        }
        push(std::move(e));
    });
}

}  // namespace net
