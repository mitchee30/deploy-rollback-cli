#include "../include/deploy-cli.h"
#include <cstdio>
#include <array>
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <stdexcept>

namespace DeployGuard {

ProcessManager::ProcessManager(std::shared_ptr<Logger> logger) : logger_(std::move(logger)) {}

namespace {

void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Drains whatever is currently available on `fd` (non-blocking) and hands
// each chunk to `sink`. Returns false once the fd has hit true EOF (every
// writer closed it) so the caller can stop polling it.
template <typename Sink>
bool drainAvailable(int fd, std::array<char, 4096>& buffer, const Sink& sink) {
    ssize_t n;
    while ((n = read(fd, buffer.data(), buffer.size())) > 0) {
        sink(std::string(buffer.data(), static_cast<size_t>(n)));
    }
    return n != 0;
}

} // namespace

int ProcessManager::execute(const std::vector<std::string>& args) {
    if (args.empty()) {
        throw std::invalid_argument("ProcessManager::execute called with no command.");
    }

    int out_pipe[2];
    int err_pipe[2];
    if (pipe(out_pipe) == -1 || pipe(err_pipe) == -1) {
        logger_->log(LogLevel::ERROR, "pipe() failed while preparing to execute: " + args[0]);
        throw std::runtime_error("Failed to create pipe for child process.");
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        logger_->log(LogLevel::ERROR, "fork() failed while executing: " + args[0]);
        throw std::runtime_error("Failed to fork process.");
    }

    if (pid == 0) {
        // Child: redirect stdout/stderr to their own pipes.
        //
        // Both streams (not just stdout) get their own pipe here: leaving
        // stderr as a bare inherited fd means any detached grandchild the
        // script backgrounds (e.g. "app.sh --daemon &" without redirecting
        // its own output) inherits *our* real stderr all the way up the
        // process tree. If whatever invoked us is itself capturing our
        // stderr through a pipe (CI log capture, `docker logs`, `ctest`,
        // `cmd | tee`, ...), that outer reader then blocks on EOF forever
        // waiting for the orphaned grandchild to exit - reproducing the
        // exact same hang one level higher, invisible to this process.
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        // execvp takes the argument vector directly - no shell is involved,
        // so values like target_path can never be reinterpreted as shell syntax.
        execvp(argv[0], argv.data());

        // Only reached if execvp failed (e.g. script not found/executable).
        _exit(127);
    }

    // Parent
    close(out_pipe[1]);
    close(err_pipe[1]);

    // Non-blocking read ends: we must never let a detached grandchild that
    // the script backgrounds hold either pipe's write end open forever.
    // Reading until EOF (as popen+fgets did, and as a naive fork/exec
    // rewrite would still do) hangs in that case, because EOF only fires
    // once *every* fd referencing the write end is closed - which may be
    // never. Instead we poll both pipes while also polling the *direct*
    // child's exit status via waitpid(WNOHANG), and stop as soon as the
    // direct child is done, independent of any orphaned grandchild still
    // holding either fd.
    int out_fd = out_pipe[0];
    int err_fd = err_pipe[0];
    setNonBlocking(out_fd);
    setNonBlocking(err_fd);

    std::array<char, 4096> buffer;
    bool child_reaped = false;
    bool out_open = true;
    bool err_open = true;
    int status = 0;

    auto stdoutSink = [this](const std::string& chunk) { logger_->streamLog(chunk); };
    auto stderrSink = [](const std::string& chunk) { std::cerr << chunk << std::flush; };

    while (!child_reaped && (out_open || err_open)) {
        struct pollfd pfds[2] = {
            {out_fd, POLLIN, 0},
            {err_fd, POLLIN, 0}
        };
        int poll_res = poll(pfds, 2, 50); // 50ms slices

        if (poll_res > 0) {
            if (out_open && (pfds[0].revents & (POLLIN | POLLHUP))) {
                out_open = drainAvailable(out_fd, buffer, stdoutSink);
            }
            if (err_open && (pfds[1].revents & (POLLIN | POLLHUP))) {
                err_open = drainAvailable(err_fd, buffer, stderrSink);
            }
        }

        pid_t wait_res = waitpid(pid, &status, WNOHANG);
        if (wait_res == pid) {
            child_reaped = true;
        } else if (wait_res == -1) {
            close(out_fd);
            close(err_fd);
            logger_->log(LogLevel::ERROR, "waitpid() failed for: " + args[0]);
            throw std::runtime_error("Failed to wait for child process.");
        }
    }

    // Drain whatever the direct child already wrote before it exited.
    drainAvailable(out_fd, buffer, stdoutSink);
    drainAvailable(err_fd, buffer, stderrSink);
    close(out_fd);
    close(err_fd);

    if (!child_reaped && waitpid(pid, &status, 0) == -1) {
        logger_->log(LogLevel::ERROR, "waitpid() failed for: " + args[0]);
        throw std::runtime_error("Failed to wait for child process.");
    }

    int exit_code = -1;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    }

    return exit_code;
}

} // namespace DeployGuard
