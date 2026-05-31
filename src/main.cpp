#include <iostream>
#include <string>
#include <string_view>
#include <cstdlib>
#include <cstdio>
#include <array>
#include <sys/wait.h>

// Executes a shell command using popen, streams its stdout in real-time,
// and returns the exit status code of the command.
int ExecuteProcess(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        std::cerr << "[ERROR] Failed to run command: " << command << std::endl;
        return -1;
    }

    std::array<char, 256> buffer;
    // Read and print stdout buffer-by-buffer in real-time
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        std::cout << buffer.data() << std::flush;
    }

    int pclose_status = pclose(pipe);
    if (pclose_status == -1) {
        std::cerr << "[ERROR] pclose failed." << std::endl;
        return -1;
    }

    int exit_code = -1;
    if (WIFEXITED(pclose_status)) {
        exit_code = WEXITSTATUS(pclose_status);
    } else if (WIFSIGNALED(pclose_status)) {
        exit_code = 128 + WTERMSIG(pclose_status);
    }

    return exit_code;
}

int main(int argc, char* argv[]) {
    std::string path;
    bool path_found = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--path") {
            if (i + 1 < argc) {
                path = argv[i + 1];
                path_found = true;
                ++i;
            } else {
                std::cerr << "Error: --path option requires a value." << std::endl;
                return 1;
            }
        }
    }

    if (path_found) {
        std::cout << "Target path: " << path << std::endl;
    } else {
        std::cout << "No target path provided. Use --path <path> to specify one." << std::endl;
    }

    std::cout << "Starting deployment..." << std::endl;

    // Execute deploy.sh using the custom ExecuteProcess function
    std::string deploy_cmd = "./scripts/deploy.sh '" + path + "'";
    int deploy_res = ExecuteProcess(deploy_cmd);

    if (deploy_res != 0) {
        std::cout << "[ERROR] Deployment failed" << std::endl;
        std::cout << "Initiating rollback..." << std::endl;

        // Execute rollback.sh using the custom ExecuteProcess function
        std::string rollback_cmd = "./scripts/rollback.sh '" + path + "'";
        int rollback_res = ExecuteProcess(rollback_cmd);
        if (rollback_res == 0) {
            std::cout << "[SUCCESS] Rollback completed" << std::endl;
        } else {
            std::cout << "[ERROR] Rollback failed" << std::endl;
        }
        return 1;
    }

    std::cout << "[SUCCESS] Deployment completed" << std::endl;
    return 0;
}
