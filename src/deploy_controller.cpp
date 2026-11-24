#include "deployguard/deploy_controller.h"

#include "deployguard/file_lock.h"
#include "deployguard/logger.h"
#include "posix_util.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <system_error>
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

LogLevel levelFor(DeploymentOutcome outcome) {
    switch (outcome) {
        case DeploymentOutcome::SUCCESS:     return LogLevel::SUCCESS;
        case DeploymentOutcome::ROLLED_BACK: return LogLevel::WARNING;
        default:                             return LogLevel::ERROR;
    }
}

// Scripts given as paths are recorded as absolute paths, so recovery finds
// them even when the next run starts in another working directory. Bare
// program names (looked up in PATH) are kept as they are.
std::string absoluteIfPath(const std::string& program) {
    if (program.find('/') == std::string::npos) {
        return program;
    }
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(program, ec);
    return ec ? program : absolute.lexically_normal().string();
}

// "20261124T054012Z-4242-1": UTC start time, pid, and a per-process counter.
// The lock guarantees one run per target at a time, so this is unique enough
// to tell one deployment's backup from another's.
std::string makeRunId() {
    static std::atomic<int> counter{0};
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::ostringstream os;
    os << std::put_time(&utc, "%Y%m%dT%H%M%SZ") << '-' << ::getpid() << '-' << ++counter;
    return os.str();
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
      state_(paths_.state_file) {
    beginSummary();
}

bool DeployController::cancelled() const {
    return cancel_requested_ && cancel_requested_();
}

void DeployController::beginSummary() {
    summary_ = RunSummary{};
    summary_.target = paths_.target;
    summary_.started_at = utcTimestamp();
}

DeploymentOutcome DeployController::finish(DeploymentOutcome outcome, Clock::time_point started) {
    summary_.outcome = outcomeName(outcome);
    summary_.exit_code = toExitCode(outcome);
    summary_.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    if (outcome == DeploymentOutcome::SUCCESS) {
        summary_.reason.clear();
    }
    return outcome;
}

DeployController::RollbackPlan DeployController::currentRollbackPlan() const {
    return RollbackPlan{absoluteIfPath(config_.rollback_script), run_id_};
}

bool DeployController::recordState(DeploymentState state, const RollbackPlan& plan, std::string* error) {
    StateFields fields{{"rollback", plan.script}};
    if (!plan.run_id.empty()) {
        fields["run_id"] = plan.run_id;
    }
    return state_.write(state, fields, error);
}

// ---------------------------------------------------------------------------
// run(): lock -> recover -> preflight -> deploy -> verify -> commit
// ---------------------------------------------------------------------------
DeploymentOutcome DeployController::run() {
    const auto started = Clock::now();
    beginSummary();
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
            noteReason("target directory '" + config_.target_path + "' does not exist");
        } else {
            noteReason(lock_error);
        }
        logger_->log(LogLevel::ERROR, "Cannot start: " + summary_.reason + ".");
        return finish(DeploymentOutcome::ABORTED, started);
    }

    // 2. Recover from an interrupted previous run, if any.
    const DeploymentOutcome recovery = checkAndRecoverState();
    if (recovery == DeploymentOutcome::CRITICAL_FAILURE) {
        logger_->log(LogLevel::ERROR, "Recovery failed; not starting a new deployment.");
        return finish(recovery, started);
    }
    if (recovery == DeploymentOutcome::ROLLED_BACK) {
        logger_->log(LogLevel::INFO, "Recovery complete. Continuing with the requested deployment.");
    }

    // 3. Pre-flight: everything we will need, checked before changing anything.
    std::string problem;
    if (!preflight(&problem)) {
        noteReason("pre-flight check failed: " + problem);
        logger_->log(LogLevel::ERROR, "Pre-flight check failed: " + problem);
        logger_->log(LogLevel::ERROR, "Deployment not started.");
        return finish(DeploymentOutcome::ABORTED, started);
    }

    // 4-6. Deploy, verify, commit (or roll back).
    const DeploymentOutcome outcome = finish(runDeployment(), started);
    logger_->log(levelFor(outcome), std::string("Finished: ") + outcomeName(outcome) + " (exit code " +
                                        std::to_string(toExitCode(outcome)) + ") in " +
                                        formatSeconds(Clock::now() - started));
    return outcome;
}

DeploymentOutcome DeployController::checkAndRecoverState() {
    const StateSnapshot snapshot = state_.read();
    if (!snapshot.valid) {
        noteReason("the state file " + state_.path() + " is corrupt: " + snapshot.detail);
        logger_->log(LogLevel::ERROR, "State file " + state_.path() + " is unreadable or corrupt (" +
                                          snapshot.detail + "). Refusing to guess whether a deployment was " +
                                          "interrupted: check the target, then delete the state file.");
        return DeploymentOutcome::CRITICAL_FAILURE;
    }
    if (snapshot.state == DeploymentState::STABLE) {
        return DeploymentOutcome::SUCCESS;
    }

    // Finish the interrupted job the way *it* would have: with the rollback
    // hook it recorded, on behalf of its run id. If deploy.yaml was edited in
    // between, today's rollback hook may not know how to undo that run.
    RollbackPlan plan;
    plan.run_id = snapshot.field("run_id");
    const std::string recorded = snapshot.field("rollback");
    const std::string configured = absoluteIfPath(config_.rollback_script);
    if (recorded.empty()) {
        plan.script = configured;  // state file written before v0.4
    } else {
        plan.script = recorded;
        if (recorded != configured) {
            logger_->log(LogLevel::WARNING, "The interrupted run used the rollback hook " + recorded +
                                                "; using it instead of the one in the current configuration (" +
                                                configured + ").");
        }
        std::string why;
        if (!checkExecutable(recorded, &why)) {
            noteReason("the interrupted run's rollback hook cannot be run: " + why);
            logger_->log(LogLevel::ERROR, "The rollback hook recorded by the interrupted run cannot be run: " + why +
                                              ". Restore it, or repair the target by hand and then delete " +
                                              state_.path() + ".");
            return DeploymentOutcome::CRITICAL_FAILURE;
        }
    }
    summary_.recovered_run_id = plan.run_id.empty() ? "unknown" : plan.run_id;

    if (snapshot.state == DeploymentState::INSTALLING) {
        logger_->log(LogLevel::ERROR, "Found an interrupted deployment" + detailSuffix(snapshot) +
                                          ": the previous run stopped before finishing "
                                          "(crash, kill -9, power loss...).");
        return initiateRollback("recovering from the interrupted deployment", plan);
    }
    logger_->log(LogLevel::ERROR, "Found an interrupted rollback" + detailSuffix(snapshot) + ".");
    return initiateRollback("resuming the interrupted rollback", plan);
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
        noteReason("cancelled before the deployment started");
        logger_->log(LogLevel::WARNING, "Cancelled before the deployment started.");
        return DeploymentOutcome::ABORTED;
    }

    // A backup left in DEPLOYGUARD_BACKUP_DIR by an earlier, *committed*
    // deployment is stale. If it were still there when we wrote INSTALLING
    // and we then crashed before the deploy hook replaced it, recovery would
    // hand the rollback hook that old backup and roll the target back two
    // versions instead of one. Removing it first gives the hooks a simple
    // rule: during INSTALLING/ROLLING_BACK, a backup that exists belongs to
    // the current deployment. (A crash between the two steps leaves state
    // STABLE with nothing changed - harmless.) The run id recorded below
    // lets a hook double-check that.
    std::error_code fs_error;
    std::filesystem::remove_all(paths_.backup_dir, fs_error);
    if (fs_error) {
        noteReason("cannot remove the stale backup " + paths_.backup_dir + ": " + fs_error.message());
        logger_->log(LogLevel::ERROR, "Refusing to deploy: " + summary_.reason + ".");
        return DeploymentOutcome::ABORTED;
    }

    run_id_ = makeRunId();
    summary_.run_id = run_id_;
    const RollbackPlan plan = currentRollbackPlan();

    // Written (durably) *before* the deploy hook touches anything. If we die
    // from here on, the next run finds INSTALLING and rolls back - with this
    // rollback hook, on behalf of this run id.
    std::string error;
    if (!recordState(DeploymentState::INSTALLING, plan, &error)) {
        noteReason("cannot record the crash-recovery marker: " + error);
        logger_->log(LogLevel::ERROR, "Refusing to deploy: cannot record the crash-recovery marker (" + error +
                                          "). Without it an interrupted deployment could not be detected.");
        return DeploymentOutcome::ABORTED;
    }
    logger_->log(LogLevel::INFO, "Deployment " + run_id_ + " started.");

    const ExecResult deploy = runHook("deploy", config_.deploy_script, /*cancellable=*/true, config_.timeout_seconds,
                                      0, run_id_);
    if (!deploy.succeeded()) {
        return initiateRollback("the deploy hook " + deploy.describe(), plan);
    }

    if (!config_.health_check_script.empty()) {
        if (!runHealthCheck()) {
            return initiateRollback(cancelled() ? "cancelled during the health check" : "the health check failed",
                                    plan);
        }
    } else if (cancelled()) {
        return initiateRollback("cancelled before the deployment was committed", plan);
    }

    if (!clearStateOrReport("deployment")) {
        return DeploymentOutcome::CRITICAL_FAILURE;
    }
    logger_->log(LogLevel::SUCCESS, "Deployment " + run_id_ + " committed.");
    return DeploymentOutcome::SUCCESS;
}

bool DeployController::runHealthCheck() {
    const int attempts = std::max(1, config_.health_check_attempts);
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (cancelled()) {
            return false;
        }
        const ExecResult check = runHook("health_check", config_.health_check_script, /*cancellable=*/true,
                                         config_.health_check_timeout_seconds, attempt, run_id_);
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

DeploymentOutcome DeployController::initiateRollback(const std::string& reason, const RollbackPlan& plan) {
    logger_->log(LogLevel::WARNING, "Rolling back: " + reason + ".");
    noteReason(reason);

    std::string error;
    if (!recordState(DeploymentState::ROLLING_BACK, plan, &error)) {
        logger_->log(LogLevel::ERROR, "Could not record the ROLLING_BACK state (" + error +
                                          "); attempting the rollback anyway.");
    }
    if (cancelled()) {
        logger_->log(LogLevel::WARNING,
                     "Cancellation was requested, but a rollback is never interrupted: stopping it halfway would "
                     "leave the target in an unknown state. (If deploy-cli is killed anyway, the next run resumes "
                     "the rollback.)");
    }

    // Retried, because the alternative is paging a human: a rollback that
    // failed because of something transient (a busy port, a network blip)
    // often succeeds a few seconds later. Rollback hooks must be idempotent
    // anyway, since recovery may run them again after a crash.
    const int attempts = std::max(1, config_.rollback_attempts);
    ExecResult rollback;
    int attempt = 1;
    for (;; ++attempt) {
        rollback = runHook("rollback", plan.script, /*cancellable=*/false, config_.timeout_seconds, attempt,
                           plan.run_id);
        if (rollback.succeeded()) {
            break;
        }
        if (rollback.status == ExecResult::Status::SpawnFailed) {
            logger_->log(LogLevel::ERROR, "Not retrying: the rollback hook cannot be started at all.");
            break;
        }
        if (attempt >= attempts) {
            break;
        }
        logger_->log(LogLevel::WARNING, "Rollback attempt " + std::to_string(attempt) + "/" +
                                            std::to_string(attempts) + " failed; retrying in " +
                                            std::to_string(config_.rollback_retry_interval_seconds) + "s.");
        std::this_thread::sleep_for(std::chrono::seconds(config_.rollback_retry_interval_seconds));
    }

    if (!rollback.succeeded()) {
        noteReason("the rollback hook " + rollback.describe() + " (" + std::to_string(attempt) +
                   " attempt(s)) while " + reason);
        logger_->log(LogLevel::ERROR, "CRITICAL: the rollback hook " + rollback.describe() + " after " +
                                          std::to_string(attempt) + " attempt(s). The target may be inconsistent "
                                          "and needs manual attention. The state file " + state_.path() +
                                          " is kept, so the next run retries the rollback.");
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
    noteReason(std::string("the ") + context + " finished, but the state file could not be removed: " + error);
    logger_->log(LogLevel::ERROR, std::string("The ") + context + " finished, but the state file could not be " +
                                      "removed (" + error + "). The next run would treat this as an interrupted " +
                                      "deployment and roll back; check the target, then delete " + state_.path() +
                                      " by hand.");
    return false;
}

ExecResult DeployController::runHook(const char* phase, const std::string& script, bool cancellable,
                                     int timeout_seconds, int attempt, const std::string& run_id) {
    ExecOptions options;
    options.timeout = std::chrono::seconds(timeout_seconds);
    options.kill_grace_period = std::chrono::seconds(config_.kill_grace_seconds);
    if (cancellable) {
        options.should_cancel = cancel_requested_;
    }
    // Context for the hook scripts, in the style of git hooks / CI systems.
    options.extra_env = {
        {"DEPLOYGUARD_PHASE", phase},
        {"DEPLOYGUARD_TARGET", paths_.target},
        {"DEPLOYGUARD_BACKUP_DIR", paths_.backup_dir},
    };
    if (!run_id.empty()) {
        options.extra_env.emplace_back("DEPLOYGUARD_RUN_ID", run_id);
    }
    if (attempt > 0) {
        options.extra_env.emplace_back("DEPLOYGUARD_ATTEMPT", std::to_string(attempt));
    }

    const std::string attempt_note = attempt > 1 ? " (attempt " + std::to_string(attempt) + ")" : "";
    logger_->log(LogLevel::INFO, std::string("Running ") + phase + " hook" + attempt_note + ": " + script + " " +
                                     paths_.target);

    // $1 is always the absolute target path: a relative path would mean
    // something else if recovery runs from another working directory.
    ExecResult result;
    try {
        result = proc_mgr_->execute({script, paths_.target}, options);
        const LogLevel level = result.succeeded() ? LogLevel::INFO : LogLevel::WARNING;
        logger_->log(level, std::string("The ") + phase + " hook " + result.describe() + " (" +
                                formatSeconds(result.duration) + ").");
    } catch (const std::system_error& e) {
        // pipe()/fork() failed: the OS is out of processes or descriptors.
        logger_->log(LogLevel::ERROR, std::string("Could not run the ") + phase + " hook: " + e.what());
        result.status = ExecResult::Status::SpawnFailed;
        result.exit_code = 127;
        result.spawn_errno = e.code().value();
    }

    summary_.steps.push_back(StepRecord{phase, std::max(attempt, 1), result.describe(), result.exit_code,
                                        static_cast<long long>(result.duration.count())});
    return result;
}

DeploymentOutcome DeployController::dryRun() {
    const auto started = Clock::now();
    beginSummary();
    logger_->log(LogLevel::INFO, "[dry-run] Validating only: no hook will run and nothing will be changed.");
    bool ok = true;

    std::string problem;
    if (!FileLock::isAvailable(paths_.lock_file, &problem)) {
        logger_->log(LogLevel::ERROR, "[dry-run] " + problem);
        noteReason(problem);
        ok = false;
    }

    const StateSnapshot snapshot = state_.read();
    if (!snapshot.valid) {
        logger_->log(LogLevel::ERROR, "[dry-run] State file " + state_.path() + " is corrupt (" + snapshot.detail +
                                          "); a real run would refuse to start.");
        noteReason("the state file is corrupt");
        ok = false;
    } else if (snapshot.state != DeploymentState::STABLE) {
        const std::string recorded = snapshot.field("rollback");
        logger_->log(LogLevel::WARNING,
                     std::string("[dry-run] State file says ") + stateName(snapshot.state) +
                         ": a real run would first run the rollback hook to recover" +
                         (recorded.empty() ? std::string(".") : " (" + recorded + ")."));
    }

    if (preflight(&problem)) {
        logger_->log(LogLevel::INFO, "[dry-run] Pre-flight checks passed.");
    } else {
        logger_->log(LogLevel::ERROR, "[dry-run] Pre-flight check failed: " + problem);
        noteReason("pre-flight check failed: " + problem);
        ok = false;
    }

    auto limit = [](int seconds) {
        return seconds == 0 ? std::string("no time limit") : "time limit " + std::to_string(seconds) + "s";
    };
    const std::string verify =
        config_.health_check_script.empty()
            ? "no health check configured"
            : config_.health_check_script + " " + paths_.target + " (up to " +
                  std::to_string(config_.health_check_attempts) + " attempts, " +
                  std::to_string(config_.health_check_interval_seconds) + "s apart, " +
                  limit(config_.health_check_timeout_seconds) + " each)";
    logger_->log(LogLevel::INFO, "[dry-run] Plan:");
    logger_->log(LogLevel::INFO, "[dry-run]   1. lock      " + paths_.lock_file);
    logger_->log(LogLevel::INFO, "[dry-run]   2. mark      delete stale backup " + paths_.backup_dir +
                                     ", then INSTALLING in " + paths_.state_file);
    logger_->log(LogLevel::INFO, "[dry-run]   3. deploy    " + config_.deploy_script + " " + paths_.target + " (" +
                                     limit(config_.timeout_seconds) + ")");
    logger_->log(LogLevel::INFO, "[dry-run]   4. verify    " + verify);
    logger_->log(LogLevel::INFO, "[dry-run]   5. commit    remove the state file");
    logger_->log(LogLevel::INFO, "[dry-run]   on failure:  " + config_.rollback_script + " " + paths_.target +
                                     " (up to " + std::to_string(config_.rollback_attempts) + " attempts, " +
                                     limit(config_.timeout_seconds) + " each)");

    return finish(ok ? DeploymentOutcome::SUCCESS : DeploymentOutcome::ABORTED, started);
}

} // namespace DeployGuard
