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

    // Time limit for each run of the deploy and rollback hooks; 0 = none.
    // The default matches Kubernetes' progressDeadlineSeconds: long enough
    // for real deployments, short enough that a hung script does not block
    // a pipeline forever.
    int timeout_seconds = 600;

    // A health check should answer in seconds, so it gets its own, much
    // shorter limit: otherwise one hung check would cost 3 x 10 minutes.
    int health_check_timeout_seconds = 30;
    int health_check_attempts = 3;          // >= 1
    int health_check_interval_seconds = 2;  // pause between failed attempts

    // A failed rollback is retried (the rollback hook must be idempotent),
    // because giving up means paging a human: worth a few seconds' wait in
    // case the failure was transient. A hook that cannot be started at all
    // is not retried.
    int rollback_attempts = 3;              // >= 1
    int rollback_retry_interval_seconds = 5;

    // After a timeout or cancellation, how long a hook's process group gets
    // between SIGTERM and SIGKILL.
    int kill_grace_seconds = 5;
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
//     timeout_seconds: 600                        # optional, and so are
//     health_check_timeout_seconds: 30            # all of these
//     health_check_attempts: 3
//     health_check_interval_seconds: 2
//     rollback_attempts: 3
//     rollback_retry_interval_seconds: 5
//     kill_grace_seconds: 5
//
// Relative paths are resolved against the current working directory.
// Unknown keys produce a warning (they are usually typos).
// ------------------------------------------------------------------------
class ConfigParser {
public:
    // Logs the reason (and copies it to *error) and returns std::nullopt if
    // the file is missing, malformed, or fails validate().
    static std::optional<Config> parseYaml(const std::string& filepath, const std::shared_ptr<Logger>& logger,
                                           std::string* error = nullptr);

    // Semantic checks shared by the YAML and --path code paths.
    // Returns an error message, or an empty string if the config is usable.
    static std::string validate(const Config& config);
};

} // namespace DeployGuard

#endif // DEPLOYGUARD_CONFIG_H
