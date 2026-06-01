#include "../include/deploy-cli.h"
#include <iostream>
#include <string_view>

namespace DeployGuard {

std::optional<std::string> ArgumentParser::parsePath(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--path") {
            if (i + 1 < argc) {
                return std::string(argv[i + 1]);
            } else {
                std::cerr << "Error: --path option requires a value." << std::endl;
                return std::nullopt;
            }
        }
    }
    std::cerr << "Error: No target path provided. Usage: ./deploy-cli --path <target_path>" << std::endl;
    return std::nullopt;
}

} // namespace DeployGuard

int main(int argc, char* argv[]) {
    // 1. Parse Arguments
    auto path_opt = DeployGuard::ArgumentParser::parsePath(argc, argv);
    if (!path_opt.has_value()) {
        return 1;
    }
    std::string target_path = path_opt.value();

    // 2. Initialize Dependency Injection
    // The logger writes to deploy_guard.log persistently
    auto logger = std::make_shared<DeployGuard::Logger>("deploy_guard.log");
    
    // Process manager utilizes the logger for stream logging
    auto proc_mgr = std::make_shared<DeployGuard::ProcessManager>(logger);
    
    // The controller is injected with its dependencies
    DeployGuard::DeployController controller(logger, proc_mgr);

    // 3. Execute State Machine
    bool success = controller.runDeployment(target_path);

    // 4. Return correct application exit code
    return success ? 0 : 1;
}
