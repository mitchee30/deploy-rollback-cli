#include "../include/deploy-cli.h"
#include <iostream>
#include <string_view>

namespace DeployGuard {

bool ArgumentParser::parseArgs(int argc, char* argv[], std::string& config_file, std::string& fallback_path) {
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--config") {
            if (i + 1 < argc) {
                config_file = argv[i + 1];
                return true;
            } else {
                std::cerr << "Error: --config option requires a value." << std::endl;
                exit(1);
            }
        } else if (arg == "--path") {
            if (i + 1 < argc) {
                fallback_path = argv[i + 1];
            } else {
                std::cerr << "Error: --path option requires a value." << std::endl;
                exit(1);
            }
        }
    }
    return !config_file.empty();
}

} // namespace DeployGuard

int main(int argc, char* argv[]) {
    // 1. Initialize global logger
    auto logger = std::make_shared<DeployGuard::Logger>("deploy_guard.log");

    // 2. Parse Arguments
    std::string config_file;
    std::string fallback_path;
    bool has_config = DeployGuard::ArgumentParser::parseArgs(argc, argv, config_file, fallback_path);

    DeployGuard::Config config;

    if (has_config) {
        // Parse YAML configuration
        auto parsed_config = DeployGuard::ConfigParser::parseYaml(config_file, logger);
        if (!parsed_config.has_value()) {
            return 1; // Logger already printed the error
        }
        config = parsed_config.value();
    } else {
        // Fallback to basic path with default scripts
        if (fallback_path.empty()) {
            std::cerr << "Error: You must provide either --config <yaml> or --path <target_path>" << std::endl;
            return 1;
        }
        logger->log(DeployGuard::LogLevel::INFO, "No config provided. Using fallback hardcoded script paths.");
        config.target_path = fallback_path;
        config.deploy_script = "./scripts/deploy.sh";
        config.rollback_script = "./scripts/rollback.sh";
    }

    // 3. Initialize Dependency Injection
    auto proc_mgr = std::make_shared<DeployGuard::ProcessManager>(logger);
    DeployGuard::DeployController controller(logger, proc_mgr, config);

    // 4. Pre-flight Self-Healing Check
    controller.checkAndRecoverState();

    // 5. Execute State Machine
    bool success = controller.runDeployment();

    return success ? 0 : 1;

}
