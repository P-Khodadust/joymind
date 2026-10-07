#include "net/sse.hpp"

void SseParser::feed(const char* data, size_t len, const Callback& cb) {
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        // Lines end with \n, \r or \r\n; a \n right after \r is the same line end.
        if (c == '\n' && lastWasCR_) {
            lastWasCR_ = false;
            continue;
        }
        lastWasCR_ = (c == '\r');
        if (c == '\n' || c == '\r') {
            line(buf_, cb);
            buf_.clear();
        } else {
            buf_ += c;
        }
    }
}

void SseParser::finish(const Callback& cb) {
    if (!buf_.empty()) line(buf_, cb);
    buf_.clear();
    dispatch(cb);
}

void SseParser::line(const std::string& l, const Callback& cb) {
    if (l.empty()) {
        dispatch(cb);
        return;
    }
    if (l[0] == ':') return;  // comment / keep-alive
    size_t colon = l.find(':');
    std::string field = l.substr(0, colon);
    std::string value;
    if (colon != std::string::npos) {
        value = l.substr(colon + 1);
        if (!value.empty() && value[0] == ' ') value.erase(0, 1);
    }
    if (field == "data") {
        if (hasData_) cur_.data += '\n';
        cur_.data += value;
        hasData_ = true;
    } else if (field == "event") {
        cur_.event = value;
    }
}

void SseParser::dispatch(const Callback& cb) {
    if (hasData_) cb(cur_);
    cur_ = SseEvent();
    hasData_ = false;
}
