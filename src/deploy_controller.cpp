#include "deployguard/deploy_controller.h"

#include "deployguard/file_lock.h"
#include "deployguard/logger.h"
#include "posix_util.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <thread>

#include <sys/stat.h>

namespace DeployGuard {

namespace {

using Clock = std::chrono::steady_clock;

std::string formatSeconds(Clock::duration d) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << std::chrono::duration<double>(d).count() << "s";
    return os.str();
}

// Is `path` an existing regular file we may execute?
bool isExecutableFile(const std::string& path, std::string* why) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        *why = detail::errnoMessage(errno);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        *why = "not a regular file";
        return false;
    }
    if (::access(path.c_str(), X_OK) != 0) {
        *why = "not executable (fix with: chmod +x " + path + ")";
        return false;
    }
    return true;
}

// Mirrors how execvp() finds a program: a name containing '/' is a path,
// anything else is looked up in $PATH.
bool checkExecutable(const std::string& program, std::string* error) {
    std::string why;
    if (program.find('/') != std::string::npos) {
        if (isExecutableFile(program, &why)) {
            return true;
        }
        *error = "'" + program + "': " + why;
        return false;
    }
    const char* path_env = std::getenv("PATH");
    std::istringstream dirs(path_env != nullptr ? path_env : "/usr/bin:/bin");
    std::string dir;
    while (std::getline(dirs, dir, ':')) {
        if (isExecutableFile((dir.empty() ? "." : dir) + "/" + program, &why)) {
            return true;
        }
    }
    *error = "'" + program + "' was not found in PATH";
    return false;
}

std::string detailSuffix(const StateSnapshot& snapshot) {
    return snapshot.detail.empty() ? "" : " (" + snapshot.detail + ")";
}

} // namespace

const char* outcomeName(DeploymentOutcome outcome) {
    switch (outcome) {
        case DeploymentOutcome::SUCCESS:          return "SUCCESS";
        case DeploymentOutcome::ROLLED_BACK:      return "ROLLED_BACK";
        case DeploymentOutcome::CRITICAL_FAILURE: return "CRITICAL_FAILURE";
        case DeploymentOutcome::ABORTED:          return "ABORTED";
    }
    return "UNKNOWN";
}

int toExitCode(DeploymentOutcome outcome) {
    return static_cast<int>(outcome);
}

DeployController::DeployController(std::shared_ptr<Logger> logger,
                                   std::shared_ptr<ProcessManager> proc_mgr,
                                   Config config,
                                   std::function<bool()> cancel_requested)
    : logger_(std::move(logger)),
      proc_mgr_(std::move(proc_mgr)),
      config_(std::move(config)),
      cancel_requested_(std::move(cancel_requested)),
      paths_(deriveTargetPaths(config_.target_path)),
      state_(paths_.state_file) {}

bool DeployController::cancelled() const {
    return cancel_requested_ && cancel_requested_();
}

// ---------------------------------------------------------------------------
// run(): lock -> recover -> preflight -> deploy -> verify -> commit
// ---------------------------------------------------------------------------
DeploymentOutcome DeployController::run() {
    const auto started = Clock::now();
    logger_->log(LogLevel::INFO, std::string("deploy-cli ") + DEPLOYGUARD_VERSION + " - target: " + paths_.target);

    // 1. Lock. Held until this function returns (RAII), and released by the
    //    kernel even if we are killed - so if we get it, nobody else is
    //    deploying, and any leftover state file belongs to a dead process.
    std::string lock_error;
    const std::optional<FileLock> lock = FileLock::tryAcquire(paths_.lock_file, &lock_error);
    if (!lock) {
        struct stat st;
        if (::stat(paths_.target.c_str(), &st) != 0 && errno == ENOENT) {
            // The lock lives next to the target, so a missing parent directory
            // shows up here first; report the real problem.
            logger_->log(LogLevel::ERROR, "Target directory '" + config_.target_path + "' does not exist.");
        } else {
            logger_->log(LogLevel::ERROR, "Cannot start: " + lock_error + ".");
        }
        return DeploymentOutcome::ABORTED;
    }

    // 2. Recover from an interrupted previous run, if any.
    const DeploymentOutcome recovery = checkAndRecoverState();
    if (recovery == DeploymentOutcome::CRITICAL_FAILURE) {
        logger_->log(LogLevel::ERROR, "Recovery failed; not starting a new deployment.");
        return recovery;
    }
    if (recovery == DeploymentOutcome::ROLLED_BACK) {
        logger_->log(LogLevel::INFO, "Recovery complete. Continuing with the requested deployment.");
    }

    // 3. Pre-flight: everything we will need, checked before changing anything.
    std::string problem;
    if (!preflight(&problem)) {
        logger_->log(LogLevel::ERROR, "Pre-flight check failed: " + problem);
        logger_->log(LogLevel::ERROR, "Deployment not started.");
        return DeploymentOutcome::ABORTED;
    }

    // 4-6. Deploy, verify, commit (or roll back).
    const DeploymentOutcome outcome = runDeployment();

    const LogLevel level = outcome == DeploymentOutcome::SUCCESS       ? LogLevel::SUCCESS
                           : outcome == DeploymentOutcome::ROLLED_BACK ? LogLevel::WARNING
                                                                       : LogLevel::ERROR;
    logger_->log(level, std::string("Finished: ") + outcomeName(outcome) + " (exit code " +
                            std::to_string(toExitCode(outcome)) + ") in " + formatSeconds(Clock::now() - started));
    return outcome;
}

DeploymentOutcome DeployController::checkAndRecoverState() {
    const StateSnapshot snapshot = state_.read();
    if (!snapshot.valid) {
        logger_->log(LogLevel::ERROR, "State file " + state_.path() + " is unreadable or corrupt (" +
                                          snapshot.detail + "). Refusing to guess whether a deployment was " +
                                          "interrupted: check the target, then delete the state file.");
        return DeploymentOutcome::CRITICAL_FAILURE;
    }

    switch (snapshot.state) {
        case DeploymentState::STABLE:
            return DeploymentOutcome::SUCCESS;
        case DeploymentState::INSTALLING:
            logger_->log(LogLevel::ERROR, "Found an interrupted deployment" + detailSuffix(snapshot) +
                                              ": the previous run stopped before finishing "
                                              "(crash, kill -9, power loss...).");
            return initiateRollback("recovering from the interrupted deployment");
        case DeploymentState::ROLLING_BACK:
            logger_->log(LogLevel::ERROR, "Found an interrupted rollback" + detailSuffix(snapshot) + ".");
            return initiateRollback("resuming the interrupted rollback");
    }
    return DeploymentOutcome::CRITICAL_FAILURE;
}

bool DeployController::preflight(std::string* error) const {
    struct stat st;
    if (::stat(paths_.target.c_str(), &st) != 0) {
        *error = "target directory '" + config_.target_path + "' " +
                 (errno == ENOENT ? std::string("does not exist") : detail::errnoMessage(errno));
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        *error = "target '" + config_.target_path + "' is not a directory";
        return false;
    }

    struct Hook {
        const char* role;
        const std::string& script;
    };
    for (const Hook& hook : {Hook{"deploy", config_.deploy_script}, Hook{"rollback", config_.rollback_script},
                             Hook{"health_check", config_.health_check_script}}) {
        if (hook.script.empty()) {
            continue;  // only the health check is optional; validate() guarantees the others
        }
        std::string why;
        if (!checkExecutable(hook.script, &why)) {
            *error = std::string(hook.role) + " hook " + why;
            return false;
        }
    }
    return true;
}

DeploymentOutcome DeployController::runDeployment() {
    if (cancelled()) {
        logger_->log(LogLevel::WARNING, "Cancelled before the deployment started.");
        return DeploymentOutcome::ABORTED;
    }

    // Written (durably) *before* the deploy hook touches anything. If we die
    // from here on, the next run finds INSTALLING and rolls back.
    std::string error;
    if (!state_.write(DeploymentState::INSTALLING, &error)) {
        logger_->log(LogLevel::ERROR, "Refusing to deploy: cannot record the crash-recovery marker (" + error +
                                          "). Without it an interrupted deployment could not be detected.");
        return DeploymentOutcome::ABORTED;
    }

    const ExecResult deploy = runHook("deploy", config_.deploy_script, /*cancellable=*/true);
    if (!deploy.succeeded()) {
        return initiateRollback("the deploy hook " + deploy.describe());
    }

    if (!config_.health_check_script.empty()) {
        if (!runHealthCheck()) {
            return initiateRollback(cancelled() ? "cancelled during the health check" : "the health check failed");
        }
    } else if (cancelled()) {
        return initiateRollback("cancelled before the deployment was committed");
    }

    if (!clearStateOrReport("deployment")) {
        return DeploymentOutcome::CRITICAL_FAILURE;
    }
    logger_->log(LogLevel::SUCCESS, "Deployment committed.");
    return DeploymentOutcome::SUCCESS;
}

bool DeployController::runHealthCheck() {
    const int attempts = std::max(1, config_.health_check_attempts);
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (cancelled()) {
            return false;
        }
        const ExecResult check = runHook("health_check", config_.health_check_script, /*cancellable=*/true, attempt);
        const std::string progress = "(attempt " + std::to_string(attempt) + "/" + std::to_string(attempts) + ")";
        if (check.succeeded()) {
            logger_->log(LogLevel::SUCCESS, "Health check passed " + progress + ".");
            return true;
        }
        if (check.status == ExecResult::Status::Cancelled) {
            return false;
        }
        if (attempt == attempts) {
            break;
        }

        logger_->log(LogLevel::WARNING, "Health check failed " + progress + "; retrying in " +
                                            std::to_string(config_.health_check_interval_seconds) + "s.");
        // Sleep in small steps so Ctrl+C is still handled promptly.
        const auto wake_up = Clock::now() + std::chrono::seconds(config_.health_check_interval_seconds);
        while (Clock::now() < wake_up) {
            if (cancelled()) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    logger_->log(LogLevel::ERROR, "Health check failed after " + std::to_string(attempts) + " attempt(s).");
    return false;
}

DeploymentOutcome DeployController::initiateRollback(const std::string& reason) {
    logger_->log(LogLevel::WARNING, "Rolling back: " + reason + ".");

    std::string error;
    if (!state_.write(DeploymentState::ROLLING_BACK, &error)) {
        logger_->log(LogLevel::ERROR, "Could not record the ROLLING_BACK state (" + error +
                                          "); attempting the rollback anyway.");
    }
    if (cancelled()) {
        logger_->log(LogLevel::WARNING,
                     "Cancellation was requested, but a rollback is never interrupted: stopping it halfway would "
                     "leave the target in an unknown state. (If deploy-cli is killed anyway, the next run resumes "
                     "the rollback.)");
    }

    const ExecResult rollback = runHook("rollback", config_.rollback_script, /*cancellable=*/false);
    if (!rollback.succeeded()) {
        logger_->log(LogLevel::ERROR, "CRITICAL: the rollback hook " + rollback.describe() +
                                          ". The target may be inconsistent and needs manual attention. The state "
                                          "file " + state_.path() + " is kept, so the next run retries the rollback.");
        return DeploymentOutcome::CRITICAL_FAILURE;
    }
    if (!clearStateOrReport("rollback")) {
        return DeploymentOutcome::CRITICAL_FAILURE;
    }
    logger_->log(LogLevel::SUCCESS, "Rollback succeeded; the target is back in its previous state.");
    return DeploymentOutcome::ROLLED_BACK;
}

bool DeployController::clearStateOrReport(const char* context) {
    std::string error;
    if (state_.clear(&error)) {
        return true;
    }
    logger_->log(LogLevel::ERROR, std::string("The ") + context + " finished, but the state file could not be " +
                                      "removed (" + error + "). The next run would treat this as an interrupted " +
                                      "deployment and roll back; check the target, then delete " + state_.path() +
                                      " by hand.");
    return false;
}

ExecResult DeployController::runHook(const char* phase, const std::string& script, bool cancellable, int attempt) {
    ExecOptions options;
    options.timeout = std::chrono::seconds(config_.timeout_seconds);
    if (cancellable) {
        options.should_cancel = cancel_requested_;
    }
    // Context for the hook scripts, in the style of git hooks / CI systems.
    options.extra_env = {
        {"DEPLOYGUARD_PHASE", phase},
        {"DEPLOYGUARD_TARGET", paths_.target},
        {"DEPLOYGUARD_BACKUP_DIR", paths_.backup_dir},
    };
    if (attempt > 0) {
        options.extra_env.emplace_back("DEPLOYGUARD_ATTEMPT", std::to_string(attempt));
    }

    logger_->log(LogLevel::INFO, std::string("Running ") + phase + " hook: " + script + " " + config_.target_path);

    ExecResult result;
    try {
        result = proc_mgr_->execute({script, config_.target_path}, options);
    } catch (const std::exception& e) {
        // pipe()/fork() failures - the OS is out of resources.
        logger_->log(LogLevel::ERROR, std::string("Could not run the ") + phase + " hook: " + e.what());
        result.status = ExecResult::Status::SpawnFailed;
        result.exit_code = 127;
        return result;
    }

    const LogLevel level = result.succeeded() ? LogLevel::INFO : LogLevel::WARNING;
    logger_->log(level, std::string("The ") + phase + " hook " + result.describe() + " (" +
                            formatSeconds(result.duration) + ").");
    return result;
}

DeploymentOutcome DeployController::dryRun() {
    logger_->log(LogLevel::INFO, "[dry-run] Validating only: no hook will run and nothing will be changed.");
    bool ok = true;

    std::string problem;
    if (!FileLock::isAvailable(paths_.lock_file, &problem)) {
        logger_->log(LogLevel::ERROR, "[dry-run] " + problem);
        ok = false;
    }

    const StateSnapshot snapshot = state_.read();
    if (!snapshot.valid) {
        logger_->log(LogLevel::ERROR, "[dry-run] State file " + state_.path() + " is corrupt (" + snapshot.detail +
                                          "); a real run would refuse to start.");
        ok = false;
    } else if (snapshot.state != DeploymentState::STABLE) {
        logger_->log(LogLevel::WARNING, std::string("[dry-run] State file says ") + stateName(snapshot.state) +
                                            ": a real run would first run the rollback hook to recover.");
    }

    if (preflight(&problem)) {
        logger_->log(LogLevel::INFO, "[dry-run] Pre-flight checks passed.");
    } else {
        logger_->log(LogLevel::ERROR, "[dry-run] Pre-flight check failed: " + problem);
        ok = false;
    }

    const std::string timeout = config_.timeout_seconds == 0
                                    ? "no time limit"
                                    : "time limit " + std::to_string(config_.timeout_seconds) + "s";
    const std::string verify =
        config_.health_check_script.empty()
            ? "no health check configured"
            : config_.health_check_script + " " + config_.target_path + " (up to " +
                  std::to_string(config_.health_check_attempts) + " attempts, " +
                  std::to_string(config_.health_check_interval_seconds) + "s apart)";
    logger_->log(LogLevel::INFO, "[dry-run] Plan:");
    logger_->log(LogLevel::INFO, "[dry-run]   1. lock      " + paths_.lock_file);
    logger_->log(LogLevel::INFO, "[dry-run]   2. mark      INSTALLING in " + paths_.state_file);
    logger_->log(LogLevel::INFO, "[dry-run]   3. deploy    " + config_.deploy_script + " " + config_.target_path +
                                     " (" + timeout + ")");
    logger_->log(LogLevel::INFO, "[dry-run]   4. verify    " + verify);
    logger_->log(LogLevel::INFO, "[dry-run]   5. commit    remove the state file");
    logger_->log(LogLevel::INFO, "[dry-run]   on failure:  " + config_.rollback_script + " " + config_.target_path);

    return ok ? DeploymentOutcome::SUCCESS : DeploymentOutcome::ABORTED;
}

} // namespace DeployGuard
