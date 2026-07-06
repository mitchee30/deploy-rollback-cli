#include "../include/deploy-cli.h"
#include <cstdio>
#include <array>
#include <unistd.h>
#include <sys/wait.h>
#include <stdexcept>

namespace DeployGuard {

ProcessManager::ProcessManager(std::shared_ptr<Logger> logger) : logger_(std::move(logger)) {}

int ProcessManager::execute(const std::vector<std::string>& args) {
    if (args.empty()) {
        throw std::invalid_argument("ProcessManager::execute called with no command.");
    }

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        logger_->log(LogLevel::ERROR, "pipe() failed while preparing to execute: " + args[0]);
        throw std::runtime_error("Failed to create pipe for child process.");
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        logger_->log(LogLevel::ERROR, "fork() failed while executing: " + args[0]);
        throw std::runtime_error("Failed to fork process.");
    }

    if (pid == 0) {
        // Child: redirect stdout to the pipe, leave stderr inherited (same as before).
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

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
    close(pipefd[1]);
    FILE* pipe_read = fdopen(pipefd[0], "r");
    if (!pipe_read) {
        close(pipefd[0]);
        logger_->log(LogLevel::ERROR, "fdopen() failed for child pipe of: " + args[0]);
        throw std::runtime_error("Failed to open pipe for reading.");
    }

    std::array<char, 256> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe_read) != nullptr) {
        logger_->streamLog(buffer.data());
    }
    fclose(pipe_read); // also closes pipefd[0]

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
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
