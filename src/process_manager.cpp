#include "../include/deploy-cli.h"
#include <cstdio>
#include <array>
#include <sys/wait.h>
#include <stdexcept>

namespace DeployGuard {

ProcessManager::ProcessManager(std::shared_ptr<Logger> logger) : logger_(std::move(logger)) {}

int ProcessManager::execute(const std::string& command) {
    // We use a custom deleter with unique_ptr for RAII management of the FILE pointer
    auto pipe_deleter = [](FILE* f) {
        if (f) {
            pclose(f);
        }
    };
    
    std::unique_ptr<FILE, decltype(pipe_deleter)> pipe(popen(command.c_str(), "r"), pipe_deleter);
    
    if (!pipe) {
        logger_->log(LogLevel::ERROR, "popen() failed to execute command: " + command);
        throw std::runtime_error("Failed to execute process.");
    }

    std::array<char, 256> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        logger_->streamLog(buffer.data());
    }

    // Since we wrapped it in unique_ptr, we actually need to capture the pclose status
    // before the unique_ptr goes out of scope and calls pclose again.
    // So we manually release the pointer and close it ourselves to get the exit code safely.
    FILE* raw_pipe = pipe.release();
    int pclose_status = pclose(raw_pipe);

    if (pclose_status == -1) {
        logger_->log(LogLevel::ERROR, "pclose() failed.");
        throw std::runtime_error("Failed to close process pipe.");
    }

    int exit_code = -1;
    if (WIFEXITED(pclose_status)) {
        exit_code = WEXITSTATUS(pclose_status);
    } else if (WIFSIGNALED(pclose_status)) {
        exit_code = 128 + WTERMSIG(pclose_status);
    }

    return exit_code;
}

} // namespace DeployGuard
