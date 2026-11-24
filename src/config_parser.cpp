#include "deployguard/config.h"

#include "deployguard/logger.h"

#include <set>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace DeployGuard {

namespace {

struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

const std::set<std::string> kDeploymentKeys = {
    "target_path", "scripts", "timeout_seconds", "health_check_attempts", "health_check_interval_seconds",
};
const std::set<std::string> kScriptKeys = {"deploy", "rollback", "health_check"};

std::optional<std::string> optionalString(const YAML::Node& parent, const std::string& key,
                                          const std::string& where) {
    const YAML::Node node = parent[key];
    if (!node || node.IsNull()) {
        return std::nullopt;
    }
    if (!node.IsScalar()) {
        throw ConfigError("'" + where + key + "' must be a string");
    }
    std::string value = node.as<std::string>();  // not const: allows the move on return
    if (value.empty()) {
        throw ConfigError("'" + where + key + "' must not be empty");
    }
    return value;
}

std::string requiredString(const YAML::Node& parent, const std::string& key, const std::string& where) {
    auto value = optionalString(parent, key, where);
    if (!value) {
        throw ConfigError("missing required key '" + where + key + "'");
    }
    return *value;
}

std::optional<int> optionalInt(const YAML::Node& parent, const std::string& key, const std::string& where) {
    const YAML::Node node = parent[key];
    if (!node || node.IsNull()) {
        return std::nullopt;
    }
    if (!node.IsScalar()) {
        throw ConfigError("'" + where + key + "' must be a whole number");
    }
    try {
        return node.as<int>();
    } catch (const YAML::BadConversion&) {
        throw ConfigError("'" + where + key + "' must be a whole number, got '" + node.Scalar() + "'");
    }
}

void warnAboutUnknownKeys(const YAML::Node& map, const std::set<std::string>& known, const std::string& where,
                          Logger& logger) {
    for (const auto& entry : map) {
        const std::string key = entry.first.as<std::string>();
        if (known.count(key) == 0) {
            logger.log(LogLevel::WARNING, "Ignoring unknown configuration key '" + where + key + "' (typo?)");
        }
    }
}

} // namespace

std::optional<Config> ConfigParser::parseYaml(const std::string& filepath, const std::shared_ptr<Logger>& logger) {
    try {
        const YAML::Node root = YAML::LoadFile(filepath);
        if (!root.IsMap() || !root["deployment"]) {
            throw ConfigError("missing top-level 'deployment' section");
        }
        const YAML::Node deployment = root["deployment"];
        if (!deployment.IsMap()) {
            throw ConfigError("'deployment' must be a mapping");
        }
        warnAboutUnknownKeys(deployment, kDeploymentKeys, "deployment.", *logger);

        Config config;
        config.target_path = requiredString(deployment, "target_path", "deployment.");

        const YAML::Node scripts = deployment["scripts"];
        if (!scripts || !scripts.IsMap()) {
            throw ConfigError("missing 'deployment.scripts' section (needs 'deploy' and 'rollback')");
        }
        warnAboutUnknownKeys(scripts, kScriptKeys, "deployment.scripts.", *logger);
        config.deploy_script = requiredString(scripts, "deploy", "deployment.scripts.");
        config.rollback_script = requiredString(scripts, "rollback", "deployment.scripts.");
        config.health_check_script = optionalString(scripts, "health_check", "deployment.scripts.").value_or("");

        if (auto v = optionalInt(deployment, "timeout_seconds", "deployment.")) {
            config.timeout_seconds = *v;
        }
        if (auto v = optionalInt(deployment, "health_check_attempts", "deployment.")) {
            config.health_check_attempts = *v;
        }
        if (auto v = optionalInt(deployment, "health_check_interval_seconds", "deployment.")) {
            config.health_check_interval_seconds = *v;
        }

        if (const std::string problem = validate(config); !problem.empty()) {
            throw ConfigError(problem);
        }

        logger->log(LogLevel::INFO, "Loaded configuration from " + filepath);
        return config;
    } catch (const ConfigError& e) {
        logger->log(LogLevel::ERROR, "Invalid configuration in " + filepath + ": " + e.what());
    } catch (const YAML::BadFile&) {
        logger->log(LogLevel::ERROR, "Cannot read configuration file " + filepath);
    } catch (const YAML::Exception& e) {
        logger->log(LogLevel::ERROR, "Cannot parse " + filepath + ": " + e.what());
    }
    return std::nullopt;
}

std::string ConfigParser::validate(const Config& config) {
    if (config.target_path.empty()) {
        return "target_path must not be empty";
    }
    if (config.deploy_script.empty()) {
        return "a deploy script is required";
    }
    if (config.rollback_script.empty()) {
        return "a rollback script is required";
    }
    constexpr int kMaxTimeoutSeconds = 7 * 24 * 3600;
    if (config.timeout_seconds < 0 || config.timeout_seconds > kMaxTimeoutSeconds) {
        return "timeout_seconds must be between 0 (no limit) and " + std::to_string(kMaxTimeoutSeconds);
    }
    if (config.health_check_attempts < 1 || config.health_check_attempts > 100) {
        return "health_check_attempts must be between 1 and 100";
    }
    if (config.health_check_interval_seconds < 0 || config.health_check_interval_seconds > 3600) {
        return "health_check_interval_seconds must be between 0 and 3600";
    }
    return "";
}

} // namespace DeployGuard
