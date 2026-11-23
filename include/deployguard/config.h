#ifndef DEPLOYGUARD_CONFIG_H
#define DEPLOYGUARD_CONFIG_H

#include <memory>
#include <optional>
#include <string>

namespace DeployGuard {

class Logger;

struct Config {
    std::string target_path;
    std::string deploy_script;
    std::string rollback_script;
    std::string health_check_script;   // optional; empty = no health check

    // Per hook invocation. 0 = no timeout. The default matches Kubernetes'
    // progressDeadlineSeconds: long enough for real deployments, short
    // enough that a hung script does not block a pipeline forever.
    int timeout_seconds = 600;

    int health_check_attempts = 3;          // >= 1
    int health_check_interval_seconds = 2;  // pause between failed attempts
};

// ------------------------------------------------------------------------
// ConfigParser - reads deploy.yaml:
//
//   deployment:
//     target_path: ./test_app_target
//     scripts:
//       deploy: ./scripts/deploy.sh
//       rollback: ./scripts/rollback.sh
//       health_check: ./scripts/health_check.sh   # optional
//     timeout_seconds: 600                        # optional
//     health_check_attempts: 3                    # optional
//     health_check_interval_seconds: 2            # optional
//
// Relative paths are resolved against the current working directory.
// Unknown keys produce a warning (they are usually typos).
// ------------------------------------------------------------------------
class ConfigParser {
public:
    // Logs the reason and returns std::nullopt if the file is missing,
    // malformed, or fails validate().
    static std::optional<Config> parseYaml(const std::string& filepath, const std::shared_ptr<Logger>& logger);

    // Semantic checks shared by the YAML and --path code paths.
    // Returns an error message, or an empty string if the config is usable.
    static std::string validate(const Config& config);
};

} // namespace DeployGuard

#endif // DEPLOYGUARD_CONFIG_H
