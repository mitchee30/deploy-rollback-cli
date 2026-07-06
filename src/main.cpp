#include "../include/deploy-cli.h"
#include <iostream>

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
    DeployGuard::DeploymentOutcome outcome = controller.runDeployment();

    // Exit code contract (documented in README):
    //   0 = deployment succeeded
    //   1 = deployment failed, but rollback succeeded and the system is stable
    //   2 = rollback itself failed; system is in an unsafe state, needs a human
    switch (outcome) {
        case DeployGuard::DeploymentOutcome::SUCCESS:
            return 0;
        case DeployGuard::DeploymentOutcome::ROLLED_BACK:
            return 1;
        case DeployGuard::DeploymentOutcome::CRITICAL_FAILURE:
        default:
            return 2;
    }
}
