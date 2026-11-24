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
    "target_path",
    "scripts",
    "timeout_seconds",
    "health_check_timeout_seconds",
    "health_check_attempts",
    "health_check_interval_seconds",
    "rollback_attempts",
    "rollback_retry_interval_seconds",
    "kill_grace_seconds",
};

// Each optional integer key, the Config member it sets, and its valid range.
struct IntKey {
    const char* name;
    int Config::*member;
    int min;
    int max;
};
constexpr int kWeek = 7 * 24 * 3600;
const IntKey kIntKeys[] = {
    {"timeout_seconds", &Config::timeout_seconds, 0, kWeek},
    {"health_check_timeout_seconds", &Config::health_check_timeout_seconds, 0, kWeek},
    {"health_check_attempts", &Config::health_check_attempts, 1, 100},
    {"health_check_interval_seconds", &Config::health_check_interval_seconds, 0, 3600},
    {"rollback_attempts", &Config::rollback_attempts, 1, 20},
    {"rollback_retry_interval_seconds", &Config::rollback_retry_interval_seconds, 0, 3600},
    {"kill_grace_seconds", &Config::kill_grace_seconds, 0, 600},
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

std::optional<Config> ConfigParser::parseYaml(const std::string& filepath, const std::shared_ptr<Logger>& logger,
                                              std::string* error) {
    std::string problem;
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

        for (const IntKey& key : kIntKeys) {
            if (auto value = optionalInt(deployment, key.name, "deployment.")) {
                config.*key.member = *value;
            }
        }

        if (const std::string invalid = validate(config); !invalid.empty()) {
            throw ConfigError(invalid);
        }

        logger->log(LogLevel::INFO, "Loaded configuration from " + filepath);
        return config;
    } catch (const ConfigError& e) {
        problem = "Invalid configuration in " + filepath + ": " + e.what();
    } catch (const YAML::BadFile&) {
        problem = "Cannot read configuration file " + filepath;
    } catch (const YAML::Exception& e) {
        problem = "Cannot parse " + filepath + ": " + e.what();
    }
    logger->log(LogLevel::ERROR, problem);
    if (error != nullptr) {
        *error = problem;
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
    for (const IntKey& key : kIntKeys) {
        const int value = config.*key.member;
        if (value < key.min || value > key.max) {
            return std::string(key.name) + " must be between " + std::to_string(key.min) + " and " +
                   std::to_string(key.max) + " (got " + std::to_string(value) + ")";
        }
    }
    return "";
}

} // namespace DeployGuard
