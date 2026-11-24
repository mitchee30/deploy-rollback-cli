#include <doctest/doctest.h>

#include "deployguard/deploy_controller.h"
#include "deployguard/file_lock.h"
#include "deployguard/state_store.h"
#include "test_helpers.h"

#include <atomic>

using namespace DeployGuard;
using namespace testutil;
using std::chrono::seconds;

namespace {

// A target directory plus deploy/rollback/health scripts in a temp dir.
// Every script appends its name to `events`, so tests can check which hooks
// ran and in which order.
struct Fixture {
    TempDir dir;
    std::string target = dir / "app";
    std::string deploy = dir / "deploy.sh";
    std::string rollback = dir / "rollback.sh";
    std::string health = dir / "health.sh";
    std::string events = dir / "events";
    std::shared_ptr<Logger> logger = makeLogger(dir);
    std::shared_ptr<ProcessManager> pm = std::make_shared<ProcessManager>(logger);

    Fixture() {
        fs::create_directory(target);
        setDeploy("exit 0");
        setRollback("exit 0");
        setHealth("exit 0");
    }

    void setDeploy(const std::string& body) const { writeScript(deploy, "echo deploy >> '" + events + "'\n" + body + "\n"); }
    void setRollback(const std::string& body) const { writeScript(rollback, "echo rollback >> '" + events + "'\n" + body + "\n"); }
    void setHealth(const std::string& body) const { writeScript(health, "echo health >> '" + events + "'\n" + body + "\n"); }

    Config config(bool with_health_check = false) const {
        Config c;
        c.target_path = target;
        c.deploy_script = deploy;
        c.rollback_script = rollback;
        if (with_health_check) {
            c.health_check_script = health;
        }
        c.timeout_seconds = 30;
        c.health_check_attempts = 3;
        c.health_check_interval_seconds = 0;
        c.health_check_timeout_seconds = 30;
        c.rollback_attempts = 1;               // the retry tests opt in
        c.rollback_retry_interval_seconds = 0;
        return c;
    }

    DeployController controller(const Config& c, std::function<bool()> cancel = {}) const {
        return DeployController(logger, pm, c, std::move(cancel));
    }

    std::string ranHooks() const { return exists(events) ? readFile(events) : ""; }
    std::string stateFile() const { return deriveTargetPaths(target).state_file; }
    std::string log() const { return readFile(dir / "test.log"); }
};

} // namespace

// ---------------------------------------------------------------------------
// Outcomes / exit codes
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: deploy succeeds -> SUCCESS (exit 0), rollback never runs") {
    Fixture f;
    CHECK(f.controller(f.config()).run() == DeploymentOutcome::SUCCESS);
    CHECK(f.ranHooks() == std::string("deploy\n"));
    CHECK_FALSE(exists(f.stateFile()));
}

TEST_CASE("DeployController: deploy fails, rollback succeeds -> ROLLED_BACK (exit 1)") {
    Fixture f;
    f.setDeploy("exit 1");
    CHECK(f.controller(f.config()).run() == DeploymentOutcome::ROLLED_BACK);
    CHECK(f.ranHooks() == std::string("deploy\nrollback\n"));
    CHECK_FALSE(exists(f.stateFile()));
}

TEST_CASE("DeployController: rollback fails too -> CRITICAL_FAILURE (exit 2), state kept for the next run") {
    Fixture f;
    f.setDeploy("exit 1");
    f.setRollback("exit 1");
    CHECK(f.controller(f.config()).run() == DeploymentOutcome::CRITICAL_FAILURE);
    REQUIRE(exists(f.stateFile()));
    CHECK(StateStore(f.stateFile()).read().state == DeploymentState::ROLLING_BACK);
}

TEST_CASE("DeployController: exit codes match the documented contract") {
    CHECK(toExitCode(DeploymentOutcome::SUCCESS) == 0);
    CHECK(toExitCode(DeploymentOutcome::ROLLED_BACK) == 1);
    CHECK(toExitCode(DeploymentOutcome::CRITICAL_FAILURE) == 2);
    CHECK(toExitCode(DeploymentOutcome::ABORTED) == 3);
    CHECK(std::string(outcomeName(DeploymentOutcome::ABORTED)) == std::string("ABORTED"));
}

// ---------------------------------------------------------------------------
// The state file is written before each hook runs
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: INSTALLING is on disk before the deploy hook runs") {
    Fixture f;
    f.setDeploy("head -n1 '" + f.stateFile() + "' > '" + (f.dir / "seen") + "'");
    REQUIRE(f.controller(f.config()).run() == DeploymentOutcome::SUCCESS);
    CHECK(readFile(f.dir / "seen") == std::string("INSTALLING\n"));
}

TEST_CASE("DeployController: ROLLING_BACK is on disk before the rollback hook runs") {
    Fixture f;
    f.setDeploy("exit 1");
    f.setRollback("head -n1 '" + f.stateFile() + "' > '" + (f.dir / "seen") + "'");
    REQUIRE(f.controller(f.config()).run() == DeploymentOutcome::ROLLED_BACK);
    CHECK(readFile(f.dir / "seen") == std::string("ROLLING_BACK\n"));
}

TEST_CASE("DeployController: the state file survives a deploy hook that replaces the whole target") {
    // It lives next to the target, so `rm -rf target && mv new target`
    // cannot destroy the crash-recovery marker.
    Fixture f;
    f.setDeploy("rm -rf \"$1\" && mkdir \"$1\" && test -f '" + f.stateFile() + "'");
    CHECK(f.controller(f.config()).run() == DeploymentOutcome::SUCCESS);
}

TEST_CASE("DeployController: refuses to deploy if the crash-recovery marker cannot be written") {
    Fixture f;
    fs::create_directory(f.stateFile());  // a directory where the file should go: rename() fails
    CHECK(f.controller(f.config()).runDeployment() == DeploymentOutcome::ABORTED);
    CHECK(f.ranHooks().empty());
    CHECK(contains(f.log(), "crash-recovery marker"));
}

// ---------------------------------------------------------------------------
// Crash recovery
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: recovery rolls back an interrupted deployment (INSTALLING)") {
    Fixture f;
    REQUIRE(StateStore(f.stateFile()).write(DeploymentState::INSTALLING));
    CHECK(f.controller(f.config()).checkAndRecoverState() == DeploymentOutcome::ROLLED_BACK);
    CHECK(f.ranHooks() == std::string("rollback\n"));
    CHECK_FALSE(exists(f.stateFile()));
}

TEST_CASE("DeployController: recovery resumes an interrupted rollback (ROLLING_BACK)") {
    Fixture f;
    REQUIRE(StateStore(f.stateFile()).write(DeploymentState::ROLLING_BACK));
    CHECK(f.controller(f.config()).checkAndRecoverState() == DeploymentOutcome::ROLLED_BACK);
    CHECK(f.ranHooks() == std::string("rollback\n"));
}

TEST_CASE("DeployController: nothing to recover -> no hook runs") {
    Fixture f;
    CHECK(f.controller(f.config()).checkAndRecoverState() == DeploymentOutcome::SUCCESS);
    CHECK(f.ranHooks().empty());
}

TEST_CASE("DeployController: failed recovery is CRITICAL and stops run() before deploying") {
    // The bug fixed in 13a63f6: recovery used to ignore the rollback result
    // and deploy anyway, on top of a broken target.
    Fixture f;
    f.setRollback("exit 1");
    REQUIRE(StateStore(f.stateFile()).write(DeploymentState::INSTALLING));
    CHECK(f.controller(f.config()).run() == DeploymentOutcome::CRITICAL_FAILURE);
    CHECK(f.ranHooks() == std::string("rollback\n"));  // deploy never ran
    CHECK(StateStore(f.stateFile()).read().state == DeploymentState::ROLLING_BACK);
}

TEST_CASE("DeployController: successful recovery, then the requested deployment") {
    Fixture f;
    REQUIRE(StateStore(f.stateFile()).write(DeploymentState::INSTALLING));
    CHECK(f.controller(f.config()).run() == DeploymentOutcome::SUCCESS);
    CHECK(f.ranHooks() == std::string("rollback\ndeploy\n"));
}

TEST_CASE("DeployController: a corrupt state file is CRITICAL - never guessed to be STABLE") {
    Fixture f;
    writeFile(f.stateFile(), "???\n");
    CHECK(f.controller(f.config()).run() == DeploymentOutcome::CRITICAL_FAILURE);
    CHECK(f.ranHooks().empty());
    CHECK(contains(f.log(), "corrupt"));
}

// ---------------------------------------------------------------------------
// Locking
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: refuses to run while another run holds the lock") {
    // Before v0.2 the second instance read the first one's live INSTALLING
    // marker, decided it had crashed, and rolled back underneath it.
    Fixture f;
    REQUIRE(StateStore(f.stateFile()).write(DeploymentState::INSTALLING));  // the other run's marker
    const auto other_run = FileLock::tryAcquire(deriveTargetPaths(f.target).lock_file);
    REQUIRE(other_run.has_value());

    CHECK(f.controller(f.config()).run() == DeploymentOutcome::ABORTED);
    CHECK(f.ranHooks().empty());  // in particular: no rollback
    CHECK(contains(f.log(), "already running"));
}

TEST_CASE("DeployController: the lock is released when run() returns") {
    Fixture f;
    REQUIRE(f.controller(f.config()).run() == DeploymentOutcome::SUCCESS);
    CHECK(FileLock::tryAcquire(deriveTargetPaths(f.target).lock_file).has_value());
}

// ---------------------------------------------------------------------------
// Pre-flight checks: problems found before anything is changed
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: missing target directory -> ABORTED (exit 3), no hook runs") {
    // Before v0.2 this ran deploy (failed), then rollback (failed, no backup)
    // and reported CRITICAL_FAILURE although nothing had been touched.
    Fixture f;
    Config c = f.config();
    c.target_path = f.dir / "does_not_exist";
    CHECK(f.controller(c).run() == DeploymentOutcome::ABORTED);
    CHECK(f.ranHooks().empty());
    CHECK(contains(f.log(), "does not exist"));
}

TEST_CASE("DeployController: target that is a file -> ABORTED") {
    Fixture f;
    writeFile(f.dir / "a_file", "x");
    Config c = f.config();
    c.target_path = f.dir / "a_file";
    CHECK(f.controller(c).run() == DeploymentOutcome::ABORTED);
    CHECK(contains(f.log(), "not a directory"));
}

TEST_CASE("DeployController: a missing rollback hook is found BEFORE deploying") {
    Fixture f;
    Config c = f.config();
    c.rollback_script = f.dir / "no_such_rollback.sh";
    CHECK(f.controller(c).run() == DeploymentOutcome::ABORTED);
    CHECK(f.ranHooks().empty());  // the deploy hook did not run
    CHECK(contains(f.log(), "rollback hook"));
}

TEST_CASE("DeployController: a non-executable hook is reported with the fix") {
    Fixture f;
    ::chmod(f.rollback.c_str(), 0644);
    CHECK(f.controller(f.config()).run() == DeploymentOutcome::ABORTED);
    CHECK(contains(f.log(), "chmod +x"));
}

TEST_CASE("DeployController: a missing health check hook is a pre-flight failure") {
    Fixture f;
    Config c = f.config(true);
    c.health_check_script = f.dir / "no_such_health.sh";
    CHECK(f.controller(c).run() == DeploymentOutcome::ABORTED);
    CHECK(f.ranHooks().empty());
}

TEST_CASE("DeployController: hooks may be programs found in PATH") {
    Fixture f;
    Config c = f.config();
    c.deploy_script = "true";
    c.rollback_script = "true";
    CHECK(f.controller(c).run() == DeploymentOutcome::SUCCESS);

    c.rollback_script = "definitely-not-a-real-program-xyz";
    CHECK(f.controller(c).run() == DeploymentOutcome::ABORTED);
    CHECK(contains(f.log(), "not found in PATH"));
}

// ---------------------------------------------------------------------------
// Failure modes of the deploy hook
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: a hung deploy hook times out and is rolled back") {
    Fixture f;
    f.setDeploy("sleep 30");
    Config c = f.config();
    c.timeout_seconds = 1;
    const auto start = std::chrono::steady_clock::now();
    CHECK(f.controller(c).run() == DeploymentOutcome::ROLLED_BACK);
    CHECK(std::chrono::steady_clock::now() - start < seconds(10));
    CHECK(f.ranHooks() == std::string("deploy\nrollback\n"));
    CHECK(contains(f.log(), "timed out"));
}

TEST_CASE("DeployController: a deploy hook killed by a signal is rolled back") {
    Fixture f;
    f.setDeploy("kill -KILL $$");
    CHECK(f.controller(f.config()).run() == DeploymentOutcome::ROLLED_BACK);
    CHECK(contains(f.log(), "killed by signal"));
}

TEST_CASE("DeployController: a deploy hook that cannot start is rolled back") {
    Fixture f;
    Config c = f.config();
    c.deploy_script = f.dir / "vanished.sh";  // bypass preflight: call runDeployment directly
    CHECK(f.controller(c).runDeployment() == DeploymentOutcome::ROLLED_BACK);
    CHECK(contains(f.log(), "could not be started"));
}

// ---------------------------------------------------------------------------
// Cancellation (Ctrl+C / SIGTERM)
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: cancelled before start -> ABORTED, nothing runs or changes") {
    Fixture f;
    CHECK(f.controller(f.config(), [] { return true; }).run() == DeploymentOutcome::ABORTED);
    CHECK(f.ranHooks().empty());
    CHECK_FALSE(exists(f.stateFile()));
}

TEST_CASE("DeployController: cancelled during deploy -> deploy stopped, rollback runs to completion") {
    Fixture f;
    const std::string started = f.dir / "deploy_started";
    f.setDeploy("touch '" + started + "'; sleep 30");
    // The rollback takes a moment and must not be cut short even though
    // cancellation stays requested the whole time.
    f.setRollback("sleep 0.5; touch '" + (f.dir / "rollback_finished") + "'");

    auto cancel = [&started] { return exists(started); };
    const auto begin = std::chrono::steady_clock::now();
    CHECK(f.controller(f.config(), cancel).run() == DeploymentOutcome::ROLLED_BACK);
    CHECK(std::chrono::steady_clock::now() - begin < seconds(10));
    CHECK(exists(f.dir / "rollback_finished"));
    CHECK(contains(f.log(), "never interrupted"));
}

// ---------------------------------------------------------------------------
// Health check
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: healthy on the first attempt -> SUCCESS") {
    Fixture f;
    CHECK(f.controller(f.config(true)).run() == DeploymentOutcome::SUCCESS);
    CHECK(f.ranHooks() == std::string("deploy\nhealth\n"));
}

TEST_CASE("DeployController: never healthy -> retried N times, then rolled back") {
    Fixture f;
    f.setHealth("exit 1");
    CHECK(f.controller(f.config(true)).run() == DeploymentOutcome::ROLLED_BACK);
    CHECK(f.ranHooks() == std::string("deploy\nhealth\nhealth\nhealth\nrollback\n"));
}

TEST_CASE("DeployController: healthy on a retry -> SUCCESS; attempt number is passed to the hook") {
    Fixture f;
    f.setHealth("[ \"$DEPLOYGUARD_ATTEMPT\" -ge 2 ]");
    CHECK(f.controller(f.config(true)).run() == DeploymentOutcome::SUCCESS);
    CHECK(f.ranHooks() == std::string("deploy\nhealth\nhealth\n"));
}

// ---------------------------------------------------------------------------
// The hook contract
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: hooks get the target as $1 and context in DEPLOYGUARD_* variables") {
    Fixture f;
    const std::string out = f.dir / "env";
    f.setDeploy("printf '%s|%s|%s|%s|%s' \"$1\" \"$DEPLOYGUARD_PHASE\" \"$DEPLOYGUARD_TARGET\" "
                "\"$DEPLOYGUARD_BACKUP_DIR\" \"$DEPLOYGUARD_RUN_ID\" > '" + out + "'");
    DeployController c = f.controller(f.config());
    REQUIRE(c.run() == DeploymentOutcome::SUCCESS);
    REQUIRE_FALSE(c.summary().run_id.empty());
    CHECK(readFile(out) == c.paths().target + "|deploy|" + c.paths().target + "|" + c.paths().backup_dir + "|" +
                               c.summary().run_id);
}

TEST_CASE("DeployController: $1 is the absolute target path even when the config is relative") {
    // A relative path would point somewhere else if recovery ran from a
    // different working directory.
    Fixture f;
    const std::string out = f.dir / "arg";
    f.setDeploy("printf '%s' \"$1\" > '" + out + "'");
    const fs::path previous = fs::current_path();
    fs::current_path(f.dir.path());
    Config c = f.config();
    c.target_path = "./app/";
    const DeploymentOutcome outcome = f.controller(c).run();
    fs::current_path(previous);
    CHECK(outcome == DeploymentOutcome::SUCCESS);
    const std::string received = readFile(out);
    REQUIRE_FALSE(received.empty());
    CHECK(received.front() == '/');
    // The same directory - compared with equivalent(), not as strings: on
    // macOS /var is a symlink to /private/var, so the spelling may differ.
    CHECK(fs::equivalent(received, f.target));
}

TEST_CASE("DeployController: a target path full of shell metacharacters is passed literally") {
    Fixture f;
    const std::string weird = f.dir / "my app's dir; $(touch pwned) `touch pwned2` && x";
    fs::create_directory(weird);
    const std::string out = f.dir / "received";
    f.setDeploy("printf '%s' \"$1\" > '" + out + "'");

    Config c = f.config();
    c.target_path = weird;
    CHECK(f.controller(c).run() == DeploymentOutcome::SUCCESS);
    CHECK(readFile(out) == weird);
    CHECK_FALSE(exists(f.dir / "pwned"));
    CHECK_FALSE(exists("pwned"));
    CHECK_FALSE(exists("pwned2"));
}

// ---------------------------------------------------------------------------
// Dry run
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: --dry-run validates without running hooks or creating files") {
    Fixture f;
    DeployController c = f.controller(f.config(true));
    CHECK(c.dryRun() == DeploymentOutcome::SUCCESS);
    CHECK(f.ranHooks().empty());
    CHECK_FALSE(exists(c.paths().state_file));
    CHECK_FALSE(exists(c.paths().lock_file));
    CHECK(contains(f.log(), "Plan:"));
}

TEST_CASE("DeployController: --dry-run reports problems as ABORTED") {
    Fixture f;
    Config c = f.config();
    c.rollback_script = f.dir / "missing.sh";
    CHECK(f.controller(c).dryRun() == DeploymentOutcome::ABORTED);

    Fixture g;
    const auto lock = FileLock::tryAcquire(deriveTargetPaths(g.target).lock_file);
    REQUIRE(lock.has_value());
    CHECK(g.controller(g.config()).dryRun() == DeploymentOutcome::ABORTED);
}

// ---------------------------------------------------------------------------
// Backup directory lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: a stale backup from an earlier deployment is deleted before the deploy hook runs") {
    // Otherwise a crash right after INSTALLING would make recovery restore
    // that old backup and roll the target back two versions.
    Fixture f;
    const std::string backup = deriveTargetPaths(f.target).backup_dir;
    fs::create_directories(backup + "/nested");
    writeFile(backup + "/nested/old_version", "v0");
    f.setDeploy("[ ! -e \"$DEPLOYGUARD_BACKUP_DIR\" ]");
    CHECK(f.controller(f.config()).run() == DeploymentOutcome::SUCCESS);
    CHECK_FALSE(exists(backup));
}

TEST_CASE("DeployController: recovery still gets the backup of the interrupted deployment") {
    Fixture f;
    const std::string backup = deriveTargetPaths(f.target).backup_dir;
    fs::create_directory(backup);
    writeFile(backup + "/version", "v1");
    REQUIRE(StateStore(f.stateFile()).write(DeploymentState::INSTALLING));
    f.setRollback("[ -f \"$DEPLOYGUARD_BACKUP_DIR/version\" ]");
    CHECK(f.controller(f.config()).run() == DeploymentOutcome::SUCCESS);
    CHECK(f.ranHooks() == std::string("rollback\ndeploy\n"));
}

// ---------------------------------------------------------------------------
// More edge cases
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: a target whose parent directory is missing is reported as missing") {
    // The lock file lives next to the target, so this fails at the lock;
    // the message must still name the real problem.
    Fixture f;
    Config c = f.config();
    c.target_path = f.dir / "no_parent/app";
    CHECK(f.controller(c).run() == DeploymentOutcome::ABORTED);
    CHECK(contains(f.log(), "does not exist"));
    CHECK(f.ranHooks().empty());
}

TEST_CASE("DeployController: a hook path that is a directory fails pre-flight") {
    Fixture f;
    fs::create_directory(f.dir / "hooks_dir");
    Config c = f.config();
    c.deploy_script = f.dir / "hooks_dir";
    CHECK(f.controller(c).run() == DeploymentOutcome::ABORTED);
    CHECK(contains(f.log(), "not a regular file"));
}

TEST_CASE("DeployController: cancellation noticed right after the deploy hook still rolls back") {
    // No health check: the last chance to honour Ctrl+C is before committing.
    Fixture f;
    int calls = 0;
    auto cancel = [&calls] { return ++calls > 1; };  // false at the very start, then true
    CHECK(f.controller(f.config(), cancel).run() == DeploymentOutcome::ROLLED_BACK);
    CHECK(f.ranHooks() == std::string("deploy\nrollback\n"));
}

TEST_CASE("DeployController: Ctrl+C while waiting between health check attempts is handled promptly") {
    Fixture f;
    f.setHealth("exit 1");
    Config c = f.config(true);
    c.health_check_interval_seconds = 30;  // without a responsive wait this test would take 60s
    auto cancel = [&f] { return contains(f.ranHooks(), "health"); };
    const auto start = std::chrono::steady_clock::now();
    CHECK(f.controller(c, cancel).run() == DeploymentOutcome::ROLLED_BACK);
    CHECK(std::chrono::steady_clock::now() - start < seconds(10));
    CHECK(f.ranHooks() == std::string("deploy\nhealth\nrollback\n"));
    CHECK(contains(f.log(), "cancelled during the health check"));
}

TEST_CASE("DeployController: --dry-run reports a corrupt state file") {
    Fixture f;
    writeFile(f.stateFile(), "garbage\n");
    CHECK(f.controller(f.config()).dryRun() == DeploymentOutcome::ABORTED);
    CHECK(contains(f.log(), "corrupt"));
}

TEST_CASE("DeployController: --dry-run mentions a pending recovery") {
    Fixture f;
    REQUIRE(StateStore(f.stateFile()).write(DeploymentState::INSTALLING));
    CHECK(f.controller(f.config()).dryRun() == DeploymentOutcome::SUCCESS);
    CHECK(contains(f.log(), "would first run the rollback hook"));
    CHECK(f.ranHooks().empty());
}


// ---------------------------------------------------------------------------
// Run id and the recorded rollback hook
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: the state file records the run id and the absolute rollback hook") {
    Fixture f;
    const std::string seen = f.dir / "seen";
    f.setDeploy("cat '" + f.stateFile() + "' > '" + seen + "'; echo \"$DEPLOYGUARD_RUN_ID\" >> '" + seen + "'");
    DeployController c = f.controller(f.config());
    REQUIRE(c.run() == DeploymentOutcome::SUCCESS);
    const std::string content = readFile(seen);
    CHECK(content.rfind("INSTALLING\n", 0) == 0);
    CHECK(contains(content, "rollback=" + f.rollback + "\n"));
    CHECK(contains(content, "run_id=" + c.summary().run_id + "\n"));
    CHECK(contains(content, "\n" + c.summary().run_id + "\n"));  // the hook saw the same id
}

TEST_CASE("DeployController: one run id for every hook of a deployment, a new one per deployment") {
    Fixture f;
    const std::string ids = f.dir / "ids";
    f.setDeploy("echo \"deploy $DEPLOYGUARD_RUN_ID\" >> '" + ids + "'");
    f.setHealth("echo \"health $DEPLOYGUARD_RUN_ID\" >> '" + ids + "'; exit 1");
    f.setRollback("echo \"rollback $DEPLOYGUARD_RUN_ID\" >> '" + ids + "'");
    DeployController first = f.controller(f.config(true));
    REQUIRE(first.run() == DeploymentOutcome::ROLLED_BACK);
    const std::string id = first.summary().run_id;
    CHECK(readFile(ids) == "deploy " + id + "\nhealth " + id + "\nhealth " + id + "\nhealth " + id +
                               "\nrollback " + id + "\n");

    DeployController second = f.controller(f.config());  // no health check: succeeds
    REQUIRE(second.run() == DeploymentOutcome::SUCCESS);
    CHECK_FALSE(second.summary().run_id.empty());
    CHECK(second.summary().run_id != id);
}

TEST_CASE("DeployController: recovery uses the rollback hook the interrupted run recorded") {
    // deploy.yaml was edited after the crash: today's rollback hook might not
    // know how to undo what the interrupted run did.
    Fixture f;
    const std::string recorded = f.dir / "recorded_rollback.sh";
    const std::string seen = f.dir / "seen";
    writeScript(recorded, "echo recorded-rollback >> '" + f.events + "'; echo \"$DEPLOYGUARD_RUN_ID\" > '" + seen +
                              "'");
    REQUIRE(StateStore(f.stateFile()).write(DeploymentState::INSTALLING,
                                            {{"rollback", recorded}, {"run_id", "old-run-7"}}));

    DeployController c = f.controller(f.config());
    CHECK(c.run() == DeploymentOutcome::SUCCESS);
    CHECK(f.ranHooks() == std::string("recorded-rollback\ndeploy\n"));
    CHECK(readFile(seen) == std::string("old-run-7\n"));
    CHECK(c.summary().recovered_run_id == std::string("old-run-7"));
    CHECK(contains(f.log(), "instead of the one in the current configuration"));
}

TEST_CASE("DeployController: a recorded rollback hook that no longer exists stops recovery (exit 2)") {
    Fixture f;
    REQUIRE(StateStore(f.stateFile()).write(DeploymentState::INSTALLING,
                                            {{"rollback", f.dir / "deleted.sh"}, {"run_id", "old"}}));
    CHECK(f.controller(f.config()).run() == DeploymentOutcome::CRITICAL_FAILURE);
    CHECK(f.ranHooks().empty());  // neither today's rollback hook nor the deploy hook
    CHECK(exists(f.stateFile()));
    CHECK(contains(f.log(), "recorded by the interrupted run cannot be run"));
}

TEST_CASE("DeployController: a state file without a recorded hook (v0.3) uses the configured one") {
    Fixture f;
    writeFile(f.stateFile(), "INSTALLING\n");
    DeployController c = f.controller(f.config());
    CHECK(c.run() == DeploymentOutcome::SUCCESS);
    CHECK(f.ranHooks() == std::string("rollback\ndeploy\n"));
    CHECK(c.summary().recovered_run_id == std::string("unknown"));
}

// ---------------------------------------------------------------------------
// Rollback retries
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: a rollback that fails once is retried and succeeds") {
    Fixture f;
    f.setDeploy("exit 1");
    f.setRollback("[ \"$DEPLOYGUARD_ATTEMPT\" -ge 2 ]");
    Config c = f.config();
    c.rollback_attempts = 3;
    DeployController controller = f.controller(c);
    CHECK(controller.run() == DeploymentOutcome::ROLLED_BACK);
    CHECK(f.ranHooks() == std::string("deploy\nrollback\nrollback\n"));
    CHECK(contains(f.log(), "Rollback attempt 1/3 failed"));
}

TEST_CASE("DeployController: a rollback that keeps failing is CRITICAL after rollback_attempts") {
    Fixture f;
    f.setDeploy("exit 1");
    f.setRollback("exit 1");
    Config c = f.config();
    c.rollback_attempts = 3;
    DeployController controller = f.controller(c);
    CHECK(controller.run() == DeploymentOutcome::CRITICAL_FAILURE);
    CHECK(f.ranHooks() == std::string("deploy\nrollback\nrollback\nrollback\n"));
    CHECK(contains(controller.summary().reason, "3 attempt(s)"));
}

TEST_CASE("DeployController: a rollback hook that cannot start is not retried") {
    Fixture f;
    f.setDeploy("exit 1");
    Config c = f.config();
    c.rollback_attempts = 5;
    c.rollback_script = f.dir / "missing_rollback.sh";
    CHECK(f.controller(c).runDeployment() == DeploymentOutcome::CRITICAL_FAILURE);
    CHECK(contains(f.log(), "Not retrying"));
    CHECK(f.ranHooks() == std::string("deploy\n"));
}

TEST_CASE("DeployController: the retry interval is respected") {
    Fixture f;
    f.setDeploy("exit 1");
    f.setRollback("[ \"$DEPLOYGUARD_ATTEMPT\" -ge 2 ]");
    Config c = f.config();
    c.rollback_attempts = 2;
    c.rollback_retry_interval_seconds = 1;
    const auto start = std::chrono::steady_clock::now();
    CHECK(f.controller(c).run() == DeploymentOutcome::ROLLED_BACK);
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(900));
}

// ---------------------------------------------------------------------------
// Timeouts per phase
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: the health check has its own, shorter timeout") {
    Fixture f;
    f.setHealth("sleep 30");
    Config c = f.config(true);
    c.timeout_seconds = 600;               // deploy/rollback limit: long
    c.health_check_timeout_seconds = 1;    // health check limit: short
    c.health_check_attempts = 2;
    const auto start = std::chrono::steady_clock::now();
    CHECK(f.controller(c).run() == DeploymentOutcome::ROLLED_BACK);
    CHECK(std::chrono::steady_clock::now() - start < seconds(15));
    CHECK(f.ranHooks() == std::string("deploy\nhealth\nhealth\nrollback\n"));
}

TEST_CASE("DeployController: kill_grace_seconds controls the SIGTERM -> SIGKILL delay") {
    Fixture f;
    f.setDeploy("trap '' TERM; sleep 30");
    Config c = f.config();
    c.timeout_seconds = 1;
    c.kill_grace_seconds = 0;
    const auto start = std::chrono::steady_clock::now();
    CHECK(f.controller(c).run() == DeploymentOutcome::ROLLED_BACK);
    CHECK(std::chrono::steady_clock::now() - start < seconds(4));  // default grace alone is 5s
    CHECK(contains(f.log(), "SIGKILL"));
}

// ---------------------------------------------------------------------------
// The run summary (--report)
// ---------------------------------------------------------------------------

TEST_CASE("DeployController: the summary records the outcome, reason and every hook run") {
    Fixture f;
    f.setHealth("exit 1");
    Config c = f.config(true);
    c.health_check_attempts = 2;
    DeployController controller = f.controller(c);
    REQUIRE(controller.run() == DeploymentOutcome::ROLLED_BACK);

    const RunSummary& s = controller.summary();
    CHECK(s.outcome == std::string("ROLLED_BACK"));
    CHECK(s.exit_code == 1);
    CHECK(s.target == controller.paths().target);
    CHECK(s.reason == std::string("the health check failed"));
    CHECK_FALSE(s.started_at.empty());
    REQUIRE(s.steps.size() == 4);
    CHECK(s.steps[0].phase == std::string("deploy"));
    CHECK(s.steps[1].phase == std::string("health_check"));
    CHECK(s.steps[1].attempt == 1);
    CHECK(s.steps[2].attempt == 2);
    CHECK(s.steps[2].exit_code == 1);
    CHECK(s.steps[3].phase == std::string("rollback"));
    CHECK(s.steps[3].result == std::string("exited with status 0"));
}

TEST_CASE("DeployController: a successful run has no reason; an aborted one says why") {
    Fixture f;
    DeployController ok = f.controller(f.config());
    REQUIRE(ok.run() == DeploymentOutcome::SUCCESS);
    CHECK(ok.summary().reason.empty());
    CHECK(ok.summary().exit_code == 0);

    Config c = f.config();
    c.rollback_script = f.dir / "missing.sh";
    DeployController aborted = f.controller(c);
    REQUIRE(aborted.run() == DeploymentOutcome::ABORTED);
    CHECK(contains(aborted.summary().reason, "pre-flight check failed"));
    CHECK(aborted.summary().steps.empty());
}
