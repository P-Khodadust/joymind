#include "net/ssh.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <libssh2.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace net {
namespace {
using Clock = std::chrono::steady_clock;

// One SSH connection. libssh2 runs non-blocking so every wait can notice the
// Stop button and the deadline.
struct Conn {
    int sock = -1;
    LIBSSH2_SESSION* s = nullptr;
    LIBSSH2_CHANNEL* ch = nullptr;
    const std::atomic<bool>* cancel = nullptr;
    Clock::time_point deadline;

    ~Conn() {
        if (s) {
            // In non-blocking mode the free calls can return EAGAIN and free
            // nothing; block for teardown, capped so a dead peer can't hang us.
            libssh2_session_set_timeout(s, 2000);
            libssh2_session_set_blocking(s, 1);
            if (ch) libssh2_channel_free(ch);
            libssh2_session_disconnect(s, "bye");
            libssh2_session_free(s);
        }
        if (sock >= 0) close(sock);
    }
    bool cancelled() const { return cancel && cancel->load(); }
    bool stop() const { return cancelled() || Clock::now() > deadline; }
    void setTimeout(int sec) { deadline = Clock::now() + std::chrono::seconds(sec); }
    void wait() {
        pollfd p{sock, 0, 0};
        int dir = s ? libssh2_session_block_directions(s) : 0;
        if (dir & LIBSSH2_SESSION_BLOCK_INBOUND) p.events |= POLLIN;
        if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) p.events |= POLLOUT;
        poll(&p, 1, 100);
    }
    // Repeats a non-blocking libssh2 call until it completes, is cancelled or times out.
    template <class F>
    int run(F f) {
        int rc;
        while ((rc = (int)f()) == LIBSSH2_ERROR_EAGAIN) {
            if (stop()) return LIBSSH2_ERROR_TIMEOUT;
            wait();
        }
        return rc;
    }
    std::string lastError() {
        char* msg = nullptr;
        int len = 0;
        libssh2_session_last_error(s, &msg, &len, 0);
        return msg ? std::string(msg, len) : "unknown error";
    }
    std::string stopReason() const { return cancelled() ? "Cancelled." : "Timed out."; }
};

std::string base64NoPad(const unsigned char* d, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        out += t[v >> 18 & 63];
        out += t[v >> 12 & 63];
        if (i + 1 < n) out += t[v >> 6 & 63];
        if (i + 2 < n) out += t[v & 63];
    }
    return out;
}

std::string connectTcp(Conn& c, const Machine& m) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(m.host.c_str(), std::to_string(m.port).c_str(), &hints, &res) != 0 || !res)
        return "Couldn't find " + m.host + ". Check the host name and your connection.";
    c.sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (c.sock < 0) {
        freeaddrinfo(res);
        return "Couldn't create a socket.";
    }
    fcntl(c.sock, F_SETFL, fcntl(c.sock, F_GETFL, 0) | O_NONBLOCK);
    int rc = connect(c.sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    std::string where = m.host + ":" + std::to_string(m.port);
    if (rc != 0 && errno != EINPROGRESS) return "Couldn't connect to " + where + ".";
    while (rc != 0) {
        pollfd p{c.sock, POLLOUT, 0};
        int n = poll(&p, 1, 100);
        if (n > 0) {
            int e = 0;
            socklen_t l = sizeof e;
            getsockopt(c.sock, SOL_SOCKET, SO_ERROR, &e, &l);
            if (e) return "Couldn't connect to " + where + " (" + strerror(e) + ").";
            break;
        }
        if (n < 0) return "Couldn't connect to " + where + ".";
        if (c.stop()) return c.cancelled() ? "Cancelled." : "Timed out connecting to " + where + ".";
    }
    return "";
}

// Answers every keyboard-interactive prompt with the password (servers that
// disable plain "password" auth usually still ask for it this way).
LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC(kbdAnswer) {
    (void)name, (void)name_len, (void)instruction, (void)instruction_len, (void)prompts;
    const auto* pw = static_cast<const std::string*>(*abstract);
    for (int i = 0; i < num_prompts; i++) {
        responses[i].text = strdup(pw->c_str());
        responses[i].length = (unsigned)pw->size();
    }
}

std::string authenticate(Conn& c, const Machine& m) {
    char* list = nullptr;
    while (!(list = libssh2_userauth_list(c.s, m.user.c_str(), (unsigned)m.user.size())) &&
           libssh2_session_last_errno(c.s) == LIBSSH2_ERROR_EAGAIN) {
        if (c.stop()) return c.stopReason();
        c.wait();
    }
    if (!list) return libssh2_userauth_authenticated(c.s) ? "" : "Login failed: " + c.lastError();
    std::string methods = list;
    int rc = LIBSSH2_ERROR_AUTHENTICATION_FAILED;
    if (m.auth == "key") {
        if (methods.find("publickey") == std::string::npos)
            return "The server doesn't accept key login (it allows: " + methods + ").";
        // With mbedtls, libssh2 can only derive the public key from RSA keys, so
        // use the matching .pub file next to the private key when it exists.
        std::string pub = m.keyPath + ".pub";
        FILE* f = fopen(pub.c_str(), "rb");
        if (f) fclose(f);
        rc = c.run([&] {
            return libssh2_userauth_publickey_fromfile_ex(c.s, m.user.c_str(), (unsigned)m.user.size(),
                                                          f ? pub.c_str() : nullptr, m.keyPath.c_str(),
                                                          m.passphrase.empty() ? nullptr : m.passphrase.c_str());
        });
        if (rc == LIBSSH2_ERROR_FILE)
            return "Couldn't use the key file " + m.keyPath + ". Check the path and passphrase, put " + m.keyPath +
                   ".pub next to it, and use an ECDSA key (ed25519 is not supported).";
        if (rc == LIBSSH2_ERROR_AUTHENTICATION_FAILED || rc == LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED)
            return "The server rejected the key. Add the .pub to ~/.ssh/authorized_keys of " + m.user +
                   ". RSA keys don't work with OpenSSH 8.8+ (this SSH library signs them with SHA-1): use ECDSA.";
    } else {
        if (methods.find("password") != std::string::npos)
            rc = c.run([&] { return libssh2_userauth_password(c.s, m.user.c_str(), m.password.c_str()); });
        if (rc != 0 && rc != LIBSSH2_ERROR_TIMEOUT && methods.find("keyboard-interactive") != std::string::npos) {
            *libssh2_session_abstract(c.s) = const_cast<std::string*>(&m.password);
            rc = c.run([&] { return libssh2_userauth_keyboard_interactive(c.s, m.user.c_str(), kbdAnswer); });
        }
        if (rc != 0 && rc != LIBSSH2_ERROR_TIMEOUT) {
            if (methods.find("password") == std::string::npos && methods.find("keyboard-interactive") == std::string::npos)
                return "The server doesn't accept password login (it allows: " + methods + ").";
            return "Wrong user name or password for " + m.user + "@" + m.host + ".";
        }
    }
    if (rc == LIBSSH2_ERROR_TIMEOUT) return c.stopReason();
    return rc == 0 ? "" : "Login failed: " + c.lastError();
}
}  // namespace

void sshGlobalInit() { libssh2_init(0); }
void sshGlobalCleanup() { libssh2_exit(); }

std::string cleanTerminalText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        if (c == 0x1B) {  // ANSI escape: CSI "ESC [ ... final", OSC "ESC ] ... BEL/ST", else 2 bytes
            if (i + 1 < n && s[i + 1] == '[') {
                i += 2;
                while (i < n && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7E)) i++;
                i++;
            } else if (i + 1 < n && s[i + 1] == ']') {
                i += 2;
                while (i < n && s[i] != '\a' && !(s[i] == 0x1B && i + 1 < n && s[i + 1] == '\\')) i++;
                i += (i < n && s[i] == 0x1B) ? 2 : 1;
            } else {
                i += 2;
            }
            continue;
        }
        if (c == '\r') {  // \r\n -> \n; a lone \r (progress bars) also becomes \n
            if (i + 1 >= n || s[i + 1] != '\n') out += '\n';
            i++;
            continue;
        }
        if (c < 0x20 && c != '\n' && c != '\t') {
            i++;
            continue;
        }
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len > 0 && i + len <= n;
        for (size_t k = 1; ok && k < len; k++) ok = ((unsigned char)s[i + k] & 0xC0) == 0x80;
        if (ok) {
            out.append(s, i, len);
            i += len;
        } else {
            out += "\xEF\xBF\xBD";  // U+FFFD
            i++;
        }
    }
    return out;
}

SshResult sshExec(const Machine& m, const std::string& command, const std::atomic<bool>* cancel) {
    SshResult r;
    Conn c;
    c.cancel = cancel;
    c.setTimeout(20);  // connect + handshake + login
    if ((r.error = connectTcp(c, m)) != "") return r;

    c.s = libssh2_session_init();
    if (!c.s) {
        r.error = "Couldn't start an SSH session (out of memory?).";
        return r;
    }
    libssh2_session_set_blocking(c.s, 0);
    int rc = c.run([&] { return libssh2_session_handshake(c.s, c.sock); });
    if (rc != 0) {
        r.error = rc == LIBSSH2_ERROR_TIMEOUT ? c.stopReason() : "SSH handshake failed: " + c.lastError();
        return r;
    }

    // Host key pinning: never log in to a server whose key we haven't confirmed.
    const char* hash = libssh2_hostkey_hash(c.s, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!hash) {
        r.error = "The server sent no host key.";
        return r;
    }
    r.fingerprint = "SHA256:" + base64NoPad(reinterpret_cast<const unsigned char*>(hash), 32);
    if (m.hostKey.empty()) {
        r.status = SshResult::UnknownHostKey;
        return r;
    }
    if (m.hostKey != r.fingerprint) {
        r.status = SshResult::HostKeyChanged;
        r.error = "The host key of " + m.host + " changed (pinned " + m.hostKey + ", got " + r.fingerprint +
                  "). Not connecting: this could be an attack. If you reinstalled the server, forget the pinned "
                  "key in the machine settings.";
        return r;
    }
    if ((r.error = authenticate(c, m)) != "") return r;

    while (!(c.ch = libssh2_channel_open_session(c.s)) && libssh2_session_last_errno(c.s) == LIBSSH2_ERROR_EAGAIN) {
        if (c.stop()) break;
        c.wait();
    }
    if (!c.ch) {
        r.error = "Couldn't open a channel: " + c.lastError();
        return r;
    }
    c.run([&] { return libssh2_channel_handle_extended_data2(c.ch, LIBSSH2_CHANNEL_EXTENDED_DATA_MERGE); });
    if ((rc = c.run([&] { return libssh2_channel_exec(c.ch, command.c_str()); })) != 0) {
        r.error = rc == LIBSSH2_ERROR_TIMEOUT ? c.stopReason() : "Couldn't start the command: " + c.lastError();
        return r;
    }

    c.setTimeout(kCommandTimeoutSec);
    std::string out;
    std::string note;
    char buf[4096];
    for (;;) {
        ssize_t n = libssh2_channel_read(c.ch, buf, sizeof buf);
        if (n > 0) {
            size_t room = kMaxCommandOutput - out.size();
            out.append(buf, std::min((size_t)n, room));
            if ((size_t)n > room) {
                note = "\n[output truncated at 16 KB]";
                break;
            }
            continue;
        }
        if (n == 0 && libssh2_channel_eof(c.ch)) break;
        if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) {
            note = "\n[connection error: " + c.lastError() + "]";
            break;
        }
        if (c.cancelled()) {
            r.error = "Cancelled.";
            return r;
        }
        if (c.stop()) {
            note = "\n[timed out after " + std::to_string(kCommandTimeoutSec) + " s; the command was stopped]";
            break;
        }
        c.wait();
    }
    c.setTimeout(3);  // closing must not hang
    c.run([&] { return libssh2_channel_close(c.ch); });
    c.run([&] { return libssh2_channel_wait_closed(c.ch); });
    r.exitCode = note.empty() ? libssh2_channel_get_exit_status(c.ch) : -1;
    r.output = cleanTerminalText(out) + note;
    r.status = SshResult::Ok;
    return r;
}

}  // namespace net
