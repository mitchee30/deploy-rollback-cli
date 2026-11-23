#include "deployguard/state_store.h"

#include "posix_util.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <sys/stat.h>

namespace DeployGuard {

namespace fs = std::filesystem;
using detail::UniqueFd;
using detail::systemError;

const char* stateName(DeploymentState state) {
    switch (state) {
        case DeploymentState::STABLE:       return "STABLE";
        case DeploymentState::INSTALLING:   return "INSTALLING";
        case DeploymentState::ROLLING_BACK: return "ROLLING_BACK";
    }
    return "UNKNOWN";
}

TargetPaths deriveTargetPaths(const std::string& target_path) {
    std::error_code ec;
    fs::path target = fs::absolute(fs::path(target_path), ec);
    if (ec) {
        target = fs::path(target_path);
    }
    target = target.lexically_normal();
    // "/srv/app/" normalises to "/srv/app/" (empty filename): drop the slash.
    if (!target.has_filename() && target != target.root_path()) {
        target = target.parent_path();
    }

    std::string name = target.filename().string();
    if (name.empty() || name == "." || name == "..") {
        name = "root";
    }
    fs::path parent = target.parent_path();
    if (parent.empty()) {
        parent = ".";
    }

    const std::string base = ".deploy_guard_" + name;
    TargetPaths paths;
    paths.target = target.string();
    paths.state_file = (parent / (base + ".state")).string();
    paths.lock_file = (parent / (base + ".lock")).string();
    paths.backup_dir = (parent / (base + ".backup")).string();
    return paths;
}

namespace {

std::string parentDirectory(const std::string& path) {
    const fs::path parent = fs::path(path).parent_path();
    return parent.empty() ? std::string(".") : parent.string();
}

std::string isoTimestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::ostringstream os;
    os << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

// fsync() only promises the data reached the drive; on macOS the drive may
// still hold it in a volatile cache. F_FULLFSYNC asks it to flush that too.
bool syncFd(int fd) {
#if defined(__APPLE__)
    if (::fcntl(fd, F_FULLFSYNC) == 0) {
        return true;
    }
#endif
    return ::fsync(fd) == 0;
}

// After rename()/unlink(), the *directory* entry must be flushed too, or the
// change can be lost on power failure even though the file data is on disk.
// Some file systems do not support fsync on directories (EINVAL); that is
// best effort, not an error.
bool syncDirectory(const std::string& dir) {
    UniqueFd fd(::open(dir.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd) {
        return false;
    }
    if (syncFd(fd.get())) {
        return true;
    }
    return errno == EINVAL || errno == ENOTSUP;
}

bool fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

} // namespace

StateStore::StateStore(std::string state_file_path) : path_(std::move(state_file_path)) {}

bool StateStore::write(DeploymentState state, std::string* error) {
    if (state == DeploymentState::STABLE) {
        return clear(error);
    }

    // First line: the state (this is all read() needs). The rest is for
    // humans inspecting a leftover file.
    std::ostringstream content;
    content << stateName(state) << '\n'
            << "pid=" << ::getpid() << '\n'
            << "updated_at=" << isoTimestamp() << '\n';

    const std::string tmp = path_ + ".tmp";
    {
        UniqueFd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
        if (!fd) {
            return fail(error, systemError("cannot create " + tmp));
        }
        if (!detail::writeAll(fd.get(), content.str())) {
            const int err = errno;
            ::unlink(tmp.c_str());
            return fail(error, systemError("cannot write " + tmp, err));
        }
        if (!syncFd(fd.get())) {
            const int err = errno;
            ::unlink(tmp.c_str());
            return fail(error, systemError("cannot fsync " + tmp, err));
        }
        if (::close(fd.release()) != 0) {
            const int err = errno;
            ::unlink(tmp.c_str());
            return fail(error, systemError("cannot close " + tmp, err));
        }
    }

    // rename() atomically replaces the old file: a reader (or a crash) sees
    // the old content or the new content, never a mix or an empty file.
    if (::rename(tmp.c_str(), path_.c_str()) != 0) {
        const int err = errno;
        ::unlink(tmp.c_str());
        return fail(error, systemError("cannot rename " + tmp + " to " + path_, err));
    }
    if (!syncDirectory(parentDirectory(path_))) {
        return fail(error, systemError("cannot fsync directory of " + path_));
    }
    return true;
}

StateSnapshot StateStore::read() const {
    StateSnapshot snapshot;

    struct stat st;
    if (::stat(path_.c_str(), &st) != 0) {
        if (errno != ENOENT) {
            snapshot.valid = false;
            snapshot.detail = systemError("cannot stat " + path_);
        }
        return snapshot;  // no file: STABLE
    }

    std::ifstream in(path_);
    if (!in.is_open()) {
        snapshot.valid = false;
        snapshot.detail = "cannot open " + path_;
        return snapshot;
    }

    std::string first_line;
    std::getline(in, first_line);
    const std::string token = trim(first_line);
    if (token == "INSTALLING") {
        snapshot.state = DeploymentState::INSTALLING;
    } else if (token == "ROLLING_BACK") {
        snapshot.state = DeploymentState::ROLLING_BACK;
    } else {
        snapshot.valid = false;
        snapshot.detail = token.empty() ? "file is empty" : "unrecognised content '" + token.substr(0, 40) + "'";
        return snapshot;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        if (!snapshot.detail.empty()) {
            snapshot.detail += ", ";
        }
        snapshot.detail += line;
    }
    return snapshot;
}

bool StateStore::clear(std::string* error) {
    if (::unlink(path_.c_str()) != 0 && errno != ENOENT) {
        return fail(error, systemError("cannot remove " + path_));
    }
    ::unlink((path_ + ".tmp").c_str());  // leftover of an interrupted write(), if any
    if (!syncDirectory(parentDirectory(path_))) {
        return fail(error, systemError("cannot fsync directory of " + path_));
    }
    return true;
}

} // namespace DeployGuard
