#include "deployguard/file_lock.h"

#include "posix_util.h"

#include <cctype>

#include <sys/file.h>

namespace DeployGuard {

using detail::systemError;

namespace {

// The lock holder writes its pid into the lock file (for error messages
// only - the lock itself is the flock(), not the file content).
std::string readHolderPid(int fd) {
    char buf[32] = {};
    const ssize_t n = ::pread(fd, buf, sizeof buf - 1, 0);
    if (n <= 0) {
        return "";
    }
    std::string pid;
    for (ssize_t i = 0; i < n && std::isdigit(static_cast<unsigned char>(buf[i])); ++i) {
        pid += buf[i];
    }
    return pid;
}

// Human readable reason why flock() failed with `err`.
std::string describeLockFailure(int err, int fd, const std::string& path) {
    if (err == EWOULDBLOCK) {
        std::string message = "another deploy-cli is already running against this target";
        const std::string holder = readHolderPid(fd);
        if (!holder.empty()) {
            message += " (pid " + holder + ")";
        }
        return message;
    }
    return systemError("cannot lock " + path, err);
}

} // namespace

std::optional<FileLock> FileLock::tryAcquire(const std::string& path, std::string* error) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd == -1) {
        if (error != nullptr) {
            *error = systemError("cannot open lock file " + path);
        }
        return std::nullopt;
    }

    while (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EINTR) {
            continue;
        }
        if (error != nullptr) {
            *error = describeLockFailure(errno, fd, path);
        }
        ::close(fd);
        return std::nullopt;
    }

    const std::string pid = std::to_string(::getpid()) + "\n";
    if (::ftruncate(fd, 0) == 0) {
        const ssize_t ignored = ::pwrite(fd, pid.data(), pid.size(), 0);
        (void)ignored;
    }
    return FileLock(fd, path);
}

bool FileLock::isAvailable(const std::string& path, std::string* error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        if (errno == ENOENT) {
            return true;  // nobody ever locked it
        }
        if (error != nullptr) {
            *error = systemError("cannot open lock file " + path);
        }
        return false;
    }
    int res;
    do {
        res = ::flock(fd, LOCK_EX | LOCK_NB);
    } while (res != 0 && errno == EINTR);
    if (res == 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return true;
    }
    if (error != nullptr) {
        *error = describeLockFailure(errno, fd, path);
    }
    ::close(fd);
    return false;
}

FileLock::FileLock(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}

FileLock::FileLock(FileLock&& other) noexcept : fd_(other.fd_), path_(std::move(other.path_)) {
    other.fd_ = -1;
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
    if (this != &other) {
        release();
        fd_ = other.fd_;
        path_ = std::move(other.path_);
        other.fd_ = -1;
    }
    return *this;
}

FileLock::~FileLock() {
    release();
}

void FileLock::release() noexcept {
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace DeployGuard
