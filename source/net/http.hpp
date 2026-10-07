// HTTP transport (libcurl), URL policy, error mapping and the background
// network worker. The UI thread never blocks on anything in here.
#pragma once
#include <pthread.h>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "net/providers/provider.hpp"
#include "net/ssh.hpp"

namespace net {

void globalInit();
void globalCleanup();

// https:// is always allowed. http:// is allowed only for LAN/loopback hosts
// (10/8, 172.16/12, 192.168/16, 127/8, localhost) and flagged Insecure.
enum class UrlCheck { Ok, Insecure, Rejected };
UrlCheck checkUrl(const std::string& url, std::string* why = nullptr);
bool isPrivateHost(const std::string& host);
std::string hostOf(const std::string& url);

std::string describeHttpError(long status, const std::string& providerMsg, const std::string& retryAfter);

struct Event {
    enum Type { TextDelta, Done, Error, Models, CommandDone, HostKeyUnknown } type = Done;
    std::string text;               // delta, error, command output, or host key fingerprint
    std::vector<ModelInfo> models;  // Models
    std::vector<ToolCall> calls;    // Done of a chat turn: tools the model wants to run
    std::string native;             // Done of a chat turn: see Message::native
    int exitCode = -1;              // CommandDone
    unsigned job = 0;
};

// Runs one network job at a time on its own thread and hands results back
// through a mutex-protected queue that the UI drains once per frame.
class Worker {
public:
    ~Worker();
    bool busy() const { return running_; }
    // Each start* returns a job id (0 if busy). Events carry that id so the UI
    // can ignore late events from a job it already abandoned.
    unsigned startChat(const Profile& p, const std::vector<Message>& history, const std::vector<ToolSpec>& tools);
    unsigned startCommand(const Machine& m, const std::string& command);
    // Runs fn on the worker thread; its result arrives as a Done event's text.
    unsigned startTask(std::function<std::string()> fn);
    unsigned startModels(const Profile& p);
    unsigned startTest(const Profile& p);
    void cancel() { cancel_ = true; }
    bool poll(Event& out);

private:
    unsigned start(std::function<void(unsigned)> fn);
    void push(Event e);
    static void* trampoline(void* self);

    pthread_t thread_{};
    bool joinable_ = false;
    std::function<void()> job_;
    std::atomic<bool> running_{false}, cancel_{false};
    std::mutex mu_;
    std::deque<Event> queue_;
    unsigned nextJob_ = 1;
};

}  // namespace net
