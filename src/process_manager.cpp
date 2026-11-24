#include "deployguard/process_manager.h"

#include "deployguard/logger.h"
#include "posix_util.h"

#include <array>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace DeployGuard {

using detail::UniqueFd;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kPollIntervalMs = 50;

struct Pipe {
    UniqueFd read;
    UniqueFd write;
};

// Both ends are created close-on-exec. In the child, the ends it needs are
// dup2()'d onto stdout/stderr (dup2 clears the flag on the copy), so no pipe
// end ever leaks into a program the hook script starts.
// (pipe2(O_CLOEXEC) would do this atomically but does not exist on macOS;
// the gap between pipe() and fcntl() only matters in multi-threaded programs.)
Pipe makePipe() {
    int fds[2];
    if (::pipe(fds) == -1) {
        throw std::system_error(errno, std::generic_category(), "pipe");
    }
    Pipe p{UniqueFd(fds[0]), UniqueFd(fds[1])};
    for (const int fd : fds) {
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
            throw std::system_error(errno, std::generic_category(), "fcntl(FD_CLOEXEC)");
        }
    }
    return p;
}

void setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::system_error(errno, std::generic_category(), "fcntl(O_NONBLOCK)");
    }
}

std::string baseName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// ---------------------------------------------------------------------------
// Child side (runs between fork() and exec()).
// ---------------------------------------------------------------------------

// Makes `fd` available as `target` (e.g. STDOUT_FILENO), without the
// close-on-exec flag.
void moveFdTo(int fd, int target) {
    if (fd == target) {
        ::fcntl(fd, F_SETFD, 0);
    } else {
        ::dup2(fd, target);
    }
}

// Closes every descriptor >= 3 except `keep`. This is a second line of
// defence: we mark our own descriptors close-on-exec, but a descriptor opened
// by a library (e.g. the std::ofstream behind the log file) is not, and a
// daemon started by the hook would otherwise hold it open forever.
void closeInheritedFds(int keep) {
#if defined(__linux__) && defined(SYS_close_range)
    // close_range(2), Linux >= 5.9: one syscall instead of a loop.
    bool ok = true;
    if (keep > 3) {
        ok = ::syscall(SYS_close_range, 3u, static_cast<unsigned>(keep - 1), 0u) == 0;
    }
    if (ok && ::syscall(SYS_close_range, static_cast<unsigned>(keep + 1), ~0u, 0u) == 0) {
        return;
    }
#endif
    long max_fd = ::sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) {
        max_fd = 65536;
    }
    for (int fd = 3; fd < static_cast<int>(max_fd); ++fd) {
        if (fd != keep) {
            ::close(fd);
        }
    }
}

[[noreturn]] void execChild(std::vector<char*>& argv,
                            int stdout_fd,
                            int stderr_fd,
                            int exec_status_fd,
                            const ExecOptions& options) {
    // A new process group led by this process: the parent can then signal
    // the script *and* everything it spawns with a single kill(-pgid, sig).
    ::setpgid(0, 0);

    // Handlers are reset by exec automatically, but *ignored* signals stay
    // ignored. deploy-cli ignores SIGPIPE for itself; hook scripts must get
    // the normal behaviour (e.g. `producer | head` terminating the producer).
    (void)::signal(SIGPIPE, SIG_DFL);
    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    ::sigprocmask(SIG_SETMASK, &empty_mask, nullptr);

    // Non-interactive: a script that unexpectedly asks for input reads EOF
    // immediately instead of blocking the deployment forever.
    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
        moveFdTo(devnull, STDIN_FILENO);
    } else {
        ::close(STDIN_FILENO);
    }
    moveFdTo(stdout_fd, STDOUT_FILENO);
    moveFdTo(stderr_fd, STDERR_FILENO);
    closeInheritedFds(exec_status_fd);

    for (const auto& [name, value] : options.extra_env) {
        ::setenv(name.c_str(), value.c_str(), 1);
    }

    // No shell: argv elements reach the program literally.
    ::execvp(argv[0], argv.data());

    // Only reached if exec failed. Tell the parent why through the status
    // pipe; on success exec closes that pipe (close-on-exec) and the parent
    // reads EOF instead. This is what lets the parent tell "the script could
    // not be started" apart from "the script ran and exited with 127".
    const int err = errno;
    const ssize_t ignored = ::write(exec_status_fd, &err, sizeof err);
    (void)ignored;
    ::_exit(127);
}

// ---------------------------------------------------------------------------
// Parent side.
// ---------------------------------------------------------------------------

// Blocks until the child has either exec'd (EOF) or reported an exec error.
// Returns the child's errno in the second case.
std::optional<int> readExecStatus(int fd) {
    int child_errno = 0;
    auto* buf = reinterpret_cast<char*>(&child_errno);
    size_t got = 0;
    while (got < sizeof child_errno) {
        const ssize_t n = ::read(fd, buf + got, sizeof child_errno - got);
        if (n == 0) {
            return std::nullopt;  // EOF: exec succeeded
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::nullopt;
        }
        got += static_cast<size_t>(n);
    }
    return child_errno;
}

pid_t waitpidRetry(pid_t pid, int* status, int flags) {
    pid_t res;
    do {
        res = ::waitpid(pid, status, flags);
    } while (res == -1 && errno == EINTR);
    return res;
}

// Reads everything currently available from a non-blocking pipe. Returns
// false once the pipe reached EOF (every writer closed it).
bool drainPipe(int fd, ChildStream stream, Logger& logger, const std::string& source) {
    std::array<char, 4096> buffer;
    for (;;) {
        const ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n > 0) {
            logger.childOutput(stream, source, std::string_view(buffer.data(), static_cast<size_t>(n)));
            continue;
        }
        if (n == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
}

// Signals the child's whole process group. ESRCH (nobody left) is fine.
void signalGroup(pid_t pgid, int sig) {
    ::kill(-pgid, sig);
}

std::string formatSeconds(std::chrono::milliseconds ms) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << static_cast<double>(ms.count()) / 1000.0 << "s";
    return os.str();
}

} // namespace

std::string ExecResult::describe() const {
    std::ostringstream os;
    switch (status) {
        case Status::Exited:
            os << "exited with status " << exit_code;
            break;
        case Status::Signaled:
            os << "was killed by signal " << term_signal << " (" << ::strsignal(term_signal) << ")";
            break;
        case Status::TimedOut:
            os << "timed out after " << formatSeconds(duration) << " and was terminated";
            break;
        case Status::Cancelled:
            os << "was cancelled and terminated";
            break;
        case Status::SpawnFailed:
            os << "could not be started: " << detail::errnoMessage(spawn_errno);
            break;
    }
    return os.str();
}

ProcessManager::ProcessManager(std::shared_ptr<Logger> logger) : logger_(std::move(logger)) {}

ExecResult ProcessManager::execute(const std::vector<std::string>& args, const ExecOptions& options) const {
    if (args.empty()) {
        throw std::invalid_argument("ProcessManager::execute called with no command");
    }

    const auto start = Clock::now();
    const std::string source = baseName(args[0]);

    // Build argv before fork(), so the child has as little work as possible
    // to do between fork() and exec().
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    Pipe out = makePipe();
    Pipe err = makePipe();
    Pipe exec_status = makePipe();

    const pid_t pid = ::fork();
    if (pid == -1) {
        throw std::system_error(errno, std::generic_category(), "fork");
    }
    if (pid == 0) {
        execChild(argv, out.write.get(), err.write.get(), exec_status.write.get(), options);
    }

    // The child calls setpgid() too; doing it here as well guarantees the
    // group exists before we could ever signal it, whichever runs first.
    // (Fails harmlessly with EACCES if the child has already exec'd.)
    ::setpgid(pid, pid);

    // Close our copies of the write ends; otherwise we would never see EOF.
    out.write.reset();
    err.write.reset();
    exec_status.write.reset();

    ExecResult result;

    if (const std::optional<int> spawn_errno = readExecStatus(exec_status.read.get())) {
        int status = 0;
        waitpidRetry(pid, &status, 0);
        result.status = ExecResult::Status::SpawnFailed;
        result.exit_code = 127;
        result.spawn_errno = *spawn_errno;
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
        return result;
    }

    // Stream output while waiting. We stop as soon as the *direct* child has
    // exited - not when the pipes reach EOF: a daemon the script starts in the
    // background inherits the pipes and may keep them open forever, and
    // waiting for EOF would hang deploy-cli for as long as that daemon lives.
    const int out_fd = out.read.get();
    const int err_fd = err.read.get();
    setNonBlocking(out_fd);
    setNonBlocking(err_fd);
    bool out_open = true;
    bool err_open = true;

    enum class StopReason { None, Timeout, Cancel };
    StopReason stop_reason = StopReason::None;
    Clock::time_point term_sent_at{};
    bool kill_sent = false;
    const bool has_timeout = options.timeout.count() > 0;
    const auto deadline = start + options.timeout;

    int status = 0;
    for (;;) {
        struct pollfd pfds[2] = {
            {out_open ? out_fd : -1, POLLIN, 0},  // negative fd = ignored by poll()
            {err_open ? err_fd : -1, POLLIN, 0},
        };
        const int ready = ::poll(pfds, 2, kPollIntervalMs);
        if (ready < 0 && errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "poll");
        }
        if (ready > 0) {
            if (out_open && (pfds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
                out_open = drainPipe(out_fd, ChildStream::Stdout, *logger_, source);
            }
            if (err_open && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                err_open = drainPipe(err_fd, ChildStream::Stderr, *logger_, source);
            }
        }

        const pid_t reaped = waitpidRetry(pid, &status, WNOHANG);
        if (reaped == pid) {
            break;
        }
        if (reaped == -1) {
            throw std::system_error(errno, std::generic_category(), "waitpid");
        }

        const auto now = Clock::now();
        if (stop_reason == StopReason::None) {
            if (has_timeout && now >= deadline) {
                stop_reason = StopReason::Timeout;
                logger_->log(LogLevel::WARNING, source + " exceeded its " + formatSeconds(options.timeout) +
                                                    " timeout; sending SIGTERM to its process group");
            } else if (options.should_cancel && options.should_cancel()) {
                stop_reason = StopReason::Cancel;
                logger_->log(LogLevel::WARNING, "Cancelling " + source + "; sending SIGTERM to its process group");
            }
            if (stop_reason != StopReason::None) {
                signalGroup(pid, SIGTERM);
                term_sent_at = now;
            }
        } else if (!kill_sent && now - term_sent_at >= options.kill_grace_period) {
            logger_->log(LogLevel::WARNING, source + " is still running " + formatSeconds(options.kill_grace_period) +
                                                " after SIGTERM; sending SIGKILL");
            signalGroup(pid, SIGKILL);
            kill_sent = true;
        }
    }

    // Collect whatever the child wrote right before exiting.
    if (out_open) {
        drainPipe(out_fd, ChildStream::Stdout, *logger_, source);
    }
    if (err_open) {
        drainPipe(err_fd, ChildStream::Stderr, *logger_, source);
    }
    logger_->flushChildOutput(source);

    // If we stopped the script, make sure nothing it started survives it.
    if (stop_reason != StopReason::None) {
        signalGroup(pid, SIGKILL);
    }

    if (WIFEXITED(status)) {
        result.status = ExecResult::Status::Exited;
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.status = ExecResult::Status::Signaled;
        result.term_signal = WTERMSIG(status);
        result.exit_code = 128 + result.term_signal;
    }
    if (stop_reason == StopReason::Timeout) {
        result.status = ExecResult::Status::TimedOut;
    } else if (stop_reason == StopReason::Cancel) {
        result.status = ExecResult::Status::Cancelled;
    }
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    return result;
}

} // namespace DeployGuard
