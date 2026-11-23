// Shared helpers for the unit tests.
#ifndef DEPLOYGUARD_TEST_HELPERS_H
#define DEPLOYGUARD_TEST_HELPERS_H

#include "deployguard/logger.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace testutil {

namespace fs = std::filesystem;

// A fresh directory for one test, deleted recursively afterwards - even when
// the test fails. Tests never share files, so `ctest -j` can run them in
// parallel.
class TempDir {
public:
    TempDir() {
        std::string pattern = (fs::temp_directory_path() / "deployguard-test-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (::mkdtemp(buffer.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = buffer.data();
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const { return path_; }
    std::string operator/(const std::string& name) const { return path_ + "/" + name; }

private:
    std::string path_;
};

inline void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::trunc);
    out << content;
}

// Writes an executable /bin/sh script.
inline void writeScript(const std::string& path, const std::string& body) {
    writeFile(path, "#!/bin/sh\n" + body);
    ::chmod(path.c_str(), 0755);
}

inline std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline bool exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

inline bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Logger that writes only to <dir>/test.log (keeps test output quiet).
inline std::shared_ptr<DeployGuard::Logger> makeLogger(const TempDir& dir) {
    return std::make_shared<DeployGuard::Logger>(dir / "test.log", DeployGuard::ConsoleMode::Off);
}

inline bool waitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return condition();
}

inline pid_t readPid(const std::string& path) {
    std::ifstream in(path);
    long pid = 0;
    in >> pid;
    return static_cast<pid_t>(pid);
}

// True if the process exists and is not a zombie. (kill(pid, 0) alone also
// succeeds for zombies, which an orphan stays until init reaps it.)
inline bool processIsRunning(pid_t pid) {
    if (pid <= 0 || ::kill(pid, 0) != 0) {
        return false;
    }
    const std::string cmd = "ps -o stat= -p " + std::to_string(pid) + " 2>/dev/null";
    FILE* ps = ::popen(cmd.c_str(), "r");
    if (ps == nullptr) {
        return true;
    }
    char state[16] = {};
    const bool got = std::fgets(state, sizeof state, ps) != nullptr;
    ::pclose(ps);
    if (!got) {
        return false;  // ps no longer knows it
    }
    const std::string s = state;
    const auto first = s.find_first_not_of(' ');
    return first != std::string::npos && s[first] != 'Z';
}

inline void killIfRunning(pid_t pid) {
    if (pid > 0) {
        ::kill(pid, SIGKILL);
    }
}

} // namespace testutil

#endif // DEPLOYGUARD_TEST_HELPERS_H
