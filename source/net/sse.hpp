// Incremental Server-Sent Events parser. Bytes can arrive split anywhere
// (mid-line, mid-UTF-8 sequence); complete events are handed to the callback.
#pragma once
#include <functional>
#include <string>

#include "net/providers/provider.hpp"

class SseParser {
public:
    using Callback = std::function<void(const SseEvent&)>;
    void feed(const char* data, size_t len, const Callback& cb);
    void finish(const Callback& cb);  // flush an event that lacked the final blank line

private:
    void line(const std::string& l, const Callback& cb);
    void dispatch(const Callback& cb);
    std::string buf_;
    SseEvent cur_;
    bool hasData_ = false;
    bool lastWasCR_ = false;
};
