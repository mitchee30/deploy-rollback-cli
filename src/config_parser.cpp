#include "../include/deploy-cli.h"
#include <yaml-cpp/yaml.h>

namespace DeployGuard {

std::optional<Config> ConfigParser::parseYaml(const std::string& filepath, std::shared_ptr<Logger> logger) {
    try {
        YAML::Node yaml = YAML::LoadFile(filepath);
        
        if (!yaml["deployment"]) {
            logger->log(LogLevel::ERROR, "YAML syntax error: Missing root 'deployment' node in " + filepath);
            return std::nullopt;
        }
        
        YAML::Node deployment = yaml["deployment"];
        Config config;
        
        if (!deployment["target_path"]) {
            logger->log(LogLevel::ERROR, "YAML syntax error: Missing 'target_path' in " + filepath);
            return std::nullopt;
        }
        config.target_path = deployment["target_path"].as<std::string>();
        
        if (!deployment["scripts"] || !deployment["scripts"]["deploy"] || !deployment["scripts"]["rollback"]) {
            logger->log(LogLevel::ERROR, "YAML syntax error: Missing scripts (deploy or rollback) in " + filepath);
            return std::nullopt;
        }
        config.deploy_script = deployment["scripts"]["deploy"].as<std::string>();
        config.rollback_script = deployment["scripts"]["rollback"].as<std::string>();
        
        logger->log(LogLevel::INFO, "Successfully loaded configuration from " + filepath);
        return config;
        
    } catch (const YAML::Exception& e) {
        logger->log(LogLevel::ERROR, std::string("Failed to parse YAML file: ") + e.what());
        return std::nullopt;
    }
}

} // namespace DeployGuard
