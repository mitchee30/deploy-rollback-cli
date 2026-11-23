#ifndef DEPLOYGUARD_PROCESS_MANAGER_H
#define DEPLOYGUARD_PROCESS_MANAGER_H

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace DeployGuard {

class Logger;

struct ExecOptions {
    // Maximum run time. Zero means "wait forever".
    std::chrono::milliseconds timeout{0};

    // After a timeout or cancellation the child's whole process group gets
    // SIGTERM; whatever is still alive this long afterwards gets SIGKILL.
    std::chrono::milliseconds kill_grace_period{std::chrono::seconds(5)};

    // Polled roughly every 50 ms while the child runs. Returning true stops
    // the child the same way a timeout does. Empty = not cancellable.
    std::function<bool()> should_cancel;

    // Extra environment variables for the child (added to, or overriding, ours).
    std::vector<std::pair<std::string, std::string>> extra_env;
};

struct ExecResult {
    enum class Status {
        Exited,      // ran to completion; see exit_code
        Signaled,    // killed by a signal we did not send (e.g. it crashed)
        TimedOut,    // we stopped it because ExecOptions::timeout expired
        Cancelled,   // we stopped it because ExecOptions::should_cancel said so
        SpawnFailed  // could not be started at all (not found, not executable, ...)
    };

    Status status = Status::SpawnFailed;

    // Shell convention: the exit status, or 128 + signal number if the child
    // was killed by a signal, or 127 if it could not be started.
    int exit_code = -1;

    int term_signal = 0;  // signal that ended the child, if any
    int spawn_errno = 0;  // errno from execvp() when status == SpawnFailed
    std::chrono::milliseconds duration{0};

    bool succeeded() const { return status == Status::Exited && exit_code == 0; }

    // Human readable, e.g. "exited with status 1", "timed out after 5.0s".
    std::string describe() const;
};

// ------------------------------------------------------------------------
// ProcessManager
//
// Runs one external program (a hook script) and waits for it, streaming its
// stdout/stderr to the Logger as it runs.
//
// The command is executed with fork() + execvp() - never through a shell - so
// arguments such as the target path are passed literally and can never be
// interpreted as shell syntax.
//
// The child is set up to be a well-behaved, non-interactive job:
//   * it runs in its own process group, so a timeout or cancellation can
//     stop the script *and everything it started* (kill(-pgid, ...));
//   * stdin is /dev/null, so a script that unexpectedly prompts for input
//     gets EOF instead of hanging the deployment forever;
//   * file descriptors other than 0/1/2 are closed, so our lock file and log
//     file are never inherited by long-running daemons the script starts;
//   * signal dispositions and the signal mask are reset to defaults.
//
// Not thread-safe: it assumes the calling program is single-threaded at the
// time of fork() (deploy-cli is).
// ------------------------------------------------------------------------
class ProcessManager {
public:
    explicit ProcessManager(std::shared_ptr<Logger> logger);

    // Runs args[0] with args[1..] as its literal argument vector.
    // Throws std::invalid_argument if args is empty and std::system_error if
    // the OS refuses to create pipes or processes. A program that cannot be
    // executed is *not* an exception: it is reported as Status::SpawnFailed.
    ExecResult execute(const std::vector<std::string>& args, const ExecOptions& options = {}) const;

private:
    std::shared_ptr<Logger> logger_;
};

} // namespace DeployGuard

#endif // DEPLOYGUARD_PROCESS_MANAGER_H
