// Runs one shell command on a remote machine over SSH (libssh2). Each call
// opens its own connection; the host key is pinned on first use.
#pragma once
#include <atomic>
#include <string>

namespace net {

// One SSH machine as stored in config.json.
struct Machine {
    std::string id, name, host;
    int port = 22;
    std::string user;
    std::string auth = "key";  // key | password
    std::string password;      // password auth (also answers keyboard-interactive prompts)
    std::string keyPath;       // key auth: OpenSSH or PEM private key (RSA/ECDSA) on the SD card
    std::string passphrase;    // optional, for an encrypted key
    std::string hostKey;       // pinned "SHA256:..." fingerprint; empty until first trusted
};

struct SshResult {
    enum Status { Ok, Failed, UnknownHostKey, HostKeyChanged } status = Failed;
    int exitCode = -1;
    std::string output;       // stdout + stderr, valid UTF-8, capped
    std::string error;        // for Failed / HostKeyChanged
    std::string fingerprint;  // the server's key, for UnknownHostKey / HostKeyChanged
};

constexpr size_t kMaxCommandOutput = 16 * 1024;
constexpr int kCommandTimeoutSec = 120;

void sshGlobalInit();
void sshGlobalCleanup();
SshResult sshExec(const Machine& m, const std::string& command, const std::atomic<bool>* cancel);

// Replaces invalid UTF-8 with U+FFFD, strips ANSI escape codes and \r.
std::string cleanTerminalText(const std::string& s);

}  // namespace net
