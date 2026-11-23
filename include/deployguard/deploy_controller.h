#ifndef DEPLOYGUARD_DEPLOY_CONTROLLER_H
#define DEPLOYGUARD_DEPLOY_CONTROLLER_H

#include "deployguard/config.h"
#include "deployguard/process_manager.h"
#include "deployguard/state_store.h"

#include <functional>
#include <memory>
#include <string>

namespace DeployGuard {

class Logger;

// The process exit code contract (see README "Exit codes"):
enum class DeploymentOutcome {
    SUCCESS = 0,           // deployment succeeded (and passed its health check)
    ROLLED_BACK = 1,       // deployment failed, rollback succeeded: system is stable
    CRITICAL_FAILURE = 2,  // rollback failed: system state unknown, needs a human
    ABORTED = 3            // deployment not attempted (pre-flight failed, target
                           // locked by another run, cancelled before starting)
};

const char* outcomeName(DeploymentOutcome outcome);
int toExitCode(DeploymentOutcome outcome);

// ------------------------------------------------------------------------
// DeployController - the deployment state machine.
//
// run() performs, in order:
//
//   1. lock      take an exclusive lock on the target (refuse if another
//                deploy-cli is running against it)
//   2. recover   if the state file says a previous run died mid-deployment
//                or mid-rollback, finish that rollback first
//   3. preflight check the target exists and every hook is executable -
//                before anything is modified (a missing rollback script is
//                found now, not when it is needed)
//   4. deploy    state := INSTALLING, run the deploy hook
//   5. verify    run the health check hook (with retries), if configured
//   6. commit    clear the state file
//
// Any failure in 4-5 (non-zero exit, crash, timeout, Ctrl+C/SIGTERM, failed
// health check) triggers the rollback hook with state := ROLLING_BACK. The
// rollback itself is deliberately not cancellable.
// ------------------------------------------------------------------------
class DeployController {
public:
    DeployController(std::shared_ptr<Logger> logger,
                     std::shared_ptr<ProcessManager> proc_mgr,
                     Config config,
                     std::function<bool()> cancel_requested = {});

    // The whole pipeline (steps 1-6). This is what main() calls.
    DeploymentOutcome run();

    // Validates everything run() would validate and prints the plan, without
    // running any hook or changing any file. SUCCESS if run() would start.
    DeploymentOutcome dryRun();

    // Individual stages, public for testing. They assume the caller holds
    // the lock (run() does).
    DeploymentOutcome checkAndRecoverState();
    DeploymentOutcome runDeployment();
    bool preflight(std::string* error) const;

    const TargetPaths& paths() const { return paths_; }

private:
    std::shared_ptr<Logger> logger_;
    std::shared_ptr<ProcessManager> proc_mgr_;
    Config config_;
    std::function<bool()> cancel_requested_;
    TargetPaths paths_;
    StateStore state_;

    DeploymentOutcome initiateRollback(const std::string& reason);
    bool runHealthCheck();
    ExecResult runHook(const char* phase, const std::string& script, bool cancellable,
                       int attempt = 0);
    bool cancelled() const;
    bool clearStateOrReport(const char* context);
};

} // namespace DeployGuard

#endif // DEPLOYGUARD_DEPLOY_CONTROLLER_H
