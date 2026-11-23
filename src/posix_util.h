// Small POSIX helpers shared by the implementation files. Not part of the
// public headers in include/.
#ifndef DEPLOYGUARD_POSIX_UTIL_H
#define DEPLOYGUARD_POSIX_UTIL_H

#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace DeployGuard {
namespace detail {

// Owns a file descriptor and closes it when it goes out of scope (RAII), so
// no early return or exception can leak one.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void reset(int fd = -1) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

inline std::string errnoMessage(int err) {
    return std::strerror(err);
}

// "what: strerror(errno)" - for error messages.
inline std::string systemError(const std::string& what, int err = errno) {
    return what + ": " + errnoMessage(err);
}

// Writes all of `data`, retrying on short writes and EINTR.
inline bool writeAll(int fd, const std::string& data) {
    const char* p = data.data();
    size_t left = data.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

} // namespace detail
} // namespace DeployGuard

#endif // DEPLOYGUARD_POSIX_UTIL_H
