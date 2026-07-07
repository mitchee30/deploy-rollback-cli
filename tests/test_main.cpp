#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../include/deploy-cli.h"
#include <cstdio>
#include <fstream>
#include <memory>
#include <chrono>
#include <sys/stat.h>
#include <unistd.h>

using namespace DeployGuard;

namespace {

// argv entries must be non-const char* per the C main() convention that
// ArgumentParser::parseArgs mirrors; the string literals themselves are
// never mutated.
char* toArg(const char* s) {
    return const_cast<char*>(s);
}

// Writes a chmod'd-executable shell script to `path`. Used to stand in for
// deploy.sh/rollback.sh in tests so ProcessManager/DeployController are
// exercised without depending on the real project scripts and fixtures.
void writeExecutableScript(const std::string& path, const std::string& body) {
    {
        std::ofstream ofs(path);
        ofs << "#!/bin/sh\n" << body;
    }
    ::chmod(path.c_str(), 0755);
}

bool fileExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

} // namespace

TEST_CASE("ArgumentParser: --config sets config_file and short-circuits to true") {
    char* argv[] = {toArg("deploy-cli"), toArg("--config"), toArg("deploy.yaml")};
    std::string config_file, fallback_path;
    bool has_config = ArgumentParser::parseArgs(3, argv, config_file, fallback_path);

    CHECK(has_config == true);
    CHECK(config_file == "deploy.yaml");
    CHECK(fallback_path.empty());
}

TEST_CASE("ArgumentParser: --path alone sets fallback_path and returns false") {
    char* argv[] = {toArg("deploy-cli"), toArg("--path"), toArg("/tmp/target")};
    std::string config_file, fallback_path;
    bool has_config = ArgumentParser::parseArgs(3, argv, config_file, fallback_path);

    CHECK(has_config == false);
    CHECK(fallback_path == "/tmp/target");
}

TEST_CASE("ArgumentParser: --path correctly skips its value so a later flag still parses") {
    // Regression test: parseArgs used to forget to advance past --path's value,
    // relying on the value never colliding with a real flag token.
    char* argv[] = {
        toArg("deploy-cli"), toArg("--path"), toArg("/tmp/target"),
        toArg("--config"), toArg("deploy.yaml")
    };
    std::string config_file, fallback_path;
    bool has_config = ArgumentParser::parseArgs(5, argv, config_file, fallback_path);

    CHECK(fallback_path == "/tmp/target");
    CHECK(config_file == "deploy.yaml");
    CHECK(has_config == true);
}

TEST_CASE("ConfigParser: parses a well-formed deploy.yaml") {
    const std::string path = "test_valid_deploy.yaml";
    {
        std::ofstream ofs(path);
        ofs << "deployment:\n"
               "  target_path: \"./some_target\"\n"
               "  scripts:\n"
               "    deploy: \"./scripts/deploy.sh\"\n"
               "    rollback: \"./scripts/rollback.sh\"\n";
    }
    auto logger = std::make_shared<Logger>("test_config_parser.log");

    auto config = ConfigParser::parseYaml(path, logger);

    REQUIRE(config.has_value());
    CHECK(config->target_path == "./some_target");
    CHECK(config->deploy_script == "./scripts/deploy.sh");
    CHECK(config->rollback_script == "./scripts/rollback.sh");

    std::remove(path.c_str());
    std::remove("test_config_parser.log");
}

TEST_CASE("ConfigParser: missing target_path returns nullopt") {
    const std::string path = "test_invalid_deploy.yaml";
    {
        std::ofstream ofs(path);
        ofs << "deployment:\n"
               "  scripts:\n"
               "    deploy: \"./scripts/deploy.sh\"\n"
               "    rollback: \"./scripts/rollback.sh\"\n";
    }
    auto logger = std::make_shared<Logger>("test_config_parser.log");

    auto config = ConfigParser::parseYaml(path, logger);

    CHECK_FALSE(config.has_value());

    std::remove(path.c_str());
    std::remove("test_config_parser.log");
}

TEST_CASE("ConfigParser: missing scripts node returns nullopt") {
    const std::string path = "test_invalid_scripts.yaml";
    {
        std::ofstream ofs(path);
        ofs << "deployment:\n"
               "  target_path: \"./some_target\"\n";
    }
    auto logger = std::make_shared<Logger>("test_config_parser.log");

    auto config = ConfigParser::parseYaml(path, logger);

    CHECK_FALSE(config.has_value());

    std::remove(path.c_str());
    std::remove("test_config_parser.log");
}

TEST_CASE("ConfigParser: missing file returns nullopt instead of throwing") {
    auto logger = std::make_shared<Logger>("test_config_parser.log");

    auto config = ConfigParser::parseYaml("this_file_does_not_exist.yaml", logger);

    CHECK_FALSE(config.has_value());

    std::remove("test_config_parser.log");
}

// ---------------------------------------------------------------------------
// ArgumentParser: flag-order and value-collision edge cases
// ---------------------------------------------------------------------------

TEST_CASE("ArgumentParser: --config before --path yields the same result as --path before --config") {
    // --path then --config (existing coverage lives in the test above this one).
    char* argv_path_first[] = {
        toArg("deploy-cli"), toArg("--path"), toArg("/tmp/target"),
        toArg("--config"), toArg("deploy.yaml")
    };
    std::string config_file_a, fallback_path_a;
    bool has_config_a = ArgumentParser::parseArgs(5, argv_path_first, config_file_a, fallback_path_a);

    // --config then --path.
    char* argv_config_first[] = {
        toArg("deploy-cli"), toArg("--config"), toArg("deploy.yaml"),
        toArg("--path"), toArg("/tmp/target")
    };
    std::string config_file_b, fallback_path_b;
    bool has_config_b = ArgumentParser::parseArgs(5, argv_config_first, config_file_b, fallback_path_b);

    // Order must not matter: config wins in both cases with the same config_file.
    CHECK(has_config_a == true);
    CHECK(has_config_b == true);
    CHECK(config_file_a == config_file_b);
    CHECK(config_file_a == "deploy.yaml");
}

TEST_CASE("ArgumentParser: --path value equal to the literal string \"--config\" is not misparsed as a flag") {
    // Regression trigger for the pre-fix bug: parseArgs used to not advance
    // past --path's value, so a value that happened to equal another flag's
    // spelling would be re-examined as that flag on the next loop iteration.
    char* argv[] = {
        toArg("deploy-cli"), toArg("--path"), toArg("--config"), toArg("deploy.yaml")
    };
    std::string config_file, fallback_path;
    bool has_config = ArgumentParser::parseArgs(4, argv, config_file, fallback_path);

    CHECK(fallback_path == "--config");
    CHECK(config_file.empty());
    CHECK(has_config == false);
}

// ---------------------------------------------------------------------------
// ProcessManager / DeployController: shell-injection safety
//
// The fix (commit 7383795) replaced popen("script 'target_path'", "r")
// (target_path shell-quoted and concatenated into a string handed to
// /bin/sh -c) with fork()+execvp(script, {script, target_path}) - target_path
// is always a single, literal argv element and is never parsed by a shell,
// regardless of its contents. These tests drive that real code path
// (DeployController::runDeployment -> ProcessManager::execute) with the
// exact malicious target_path payloads a command-injection attempt would use,
// and assert the injected side-effect command never actually ran.
// ---------------------------------------------------------------------------

namespace {

void runInjectionCase(const std::string& malicious_target_path, const std::string& marker_file) {
    std::remove(marker_file.c_str());

    const std::string fake_deploy_script = "/tmp/deploy_guard_test_fake_deploy.sh";
    writeExecutableScript(fake_deploy_script,
        "echo \"fake deploy invoked, arg count check only\"\nexit 0\n");

    auto logger = std::make_shared<Logger>("test_injection.log");
    auto proc_mgr = std::make_shared<ProcessManager>(logger);

    Config config;
    config.target_path = malicious_target_path;
    config.deploy_script = fake_deploy_script;
    config.rollback_script = fake_deploy_script; // unused if deploy succeeds

    DeployController controller(logger, proc_mgr, config);
    DeploymentOutcome outcome = controller.runDeployment();

    CHECK(outcome == DeploymentOutcome::SUCCESS);
    CHECK_FALSE(fileExists(marker_file));

    std::remove(marker_file.c_str());
    std::remove(fake_deploy_script.c_str());
    std::remove("test_injection.log");
}

} // namespace

TEST_CASE("Injection: single-quote breakout in target_path does not execute injected command") {
    runInjectionCase("/tmp/foo'; touch /tmp/pwned_test; echo '", "/tmp/pwned_test");
}

TEST_CASE("Injection: backtick command substitution in target_path does not execute") {
    runInjectionCase("/tmp/foo`touch /tmp/pwned_test2`", "/tmp/pwned_test2");
}

TEST_CASE("Injection: $() command substitution in target_path does not execute") {
    runInjectionCase("/tmp/foo$(touch /tmp/pwned_test3)", "/tmp/pwned_test3");
}

TEST_CASE("Injection: && chained command in target_path does not execute") {
    runInjectionCase("/tmp/foo && touch /tmp/pwned_test4", "/tmp/pwned_test4");
}

TEST_CASE("Regression: target_path containing a space deploys successfully (fix did not break legitimate paths)") {
    const std::string target_path = "/tmp/deploy guard test target with space";
    ::mkdir(target_path.c_str(), 0755);

    const std::string fake_deploy_script = "/tmp/deploy_guard_test_fake_deploy_space.sh";
    // Mirrors the real deploy.sh contract: receives target_path as $1 and
    // must be able to use it (quoted) without word-splitting on the space.
    writeExecutableScript(fake_deploy_script,
        "mkdir -p \"$1/marker_dir\" && exit 0\nexit 1\n");

    auto logger = std::make_shared<Logger>("test_space_regression.log");
    auto proc_mgr = std::make_shared<ProcessManager>(logger);

    Config config;
    config.target_path = target_path;
    config.deploy_script = fake_deploy_script;
    config.rollback_script = fake_deploy_script;

    DeployController controller(logger, proc_mgr, config);
    DeploymentOutcome outcome = controller.runDeployment();

    CHECK(outcome == DeploymentOutcome::SUCCESS);
    CHECK(fileExists(target_path + "/marker_dir"));

    std::remove((target_path + "/.deploy_guard.state").c_str());
    ::rmdir((target_path + "/marker_dir").c_str());
    ::rmdir(target_path.c_str());
    std::remove(fake_deploy_script.c_str());
    std::remove("test_space_regression.log");
}

// ---------------------------------------------------------------------------
// ProcessManager: architecture-level hang test
//
// This does NOT hang on a script's actual behavior - it directly exercises
// ProcessManager::execute() with a command that backgrounds a long-running
// grandchild without redirecting its stdout (the exact shape of
// "app.sh --daemon &" inside deploy.sh/rollback.sh). If execute() still
// blocks on pipe EOF (as popen+fgets and the original fork/exec rewrite both
// did), this hangs for the full 30s of the backgrounded sleep; the fixed
// implementation returns as soon as the *direct* child ("sh -c ...") exits.
// ---------------------------------------------------------------------------

TEST_CASE("ProcessManager does not hang on backgrounded grandchild without redirected stdout") {
    auto logger = std::make_shared<Logger>("test_process_manager_hang.log");
    ProcessManager pm(logger);

    auto start = std::chrono::steady_clock::now();
    int code = pm.execute({"/bin/sh", "-c", "(sleep 30 &) ; exit 0"});
    auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(code == 0);
    CHECK(elapsed < std::chrono::seconds(2));

    std::remove("test_process_manager_hang.log");
}

// Same hang, but on stderr instead of stdout. Found empirically: the first
// fix decoupled stdout from pipe-EOF via waitpid, but left stderr as a bare
// inherited fd. A grandchild that backgrounds itself without redirecting its
// own stderr keeps *our* real stderr fd alive; if whatever invokes us is
// itself capturing our stderr through a pipe (CI log capture, `docker logs`,
// ctest, `cmd | tee`), that outer reader hangs waiting for EOF - reproducing
// this exact bug one process-tree level higher, invisible to us. Confirmed
// via `./deploy-cli-tests 2>&1 1>/dev/null | cat` taking 30s before this fix,
// ~1s after.
TEST_CASE("ProcessManager does not hang on backgrounded grandchild without redirected stderr") {
    auto logger = std::make_shared<Logger>("test_process_manager_hang_stderr.log");
    ProcessManager pm(logger);

    auto start = std::chrono::steady_clock::now();
    int code = pm.execute({"/bin/sh", "-c", "(sleep 30 1>&2 &) ; exit 0"});
    auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(code == 0);
    CHECK(elapsed < std::chrono::seconds(2));

    std::remove("test_process_manager_hang_stderr.log");
}

// ---------------------------------------------------------------------------
// DeployController: exit-code / outcome semantics (see README for the
// process exit code contract). Before this test suite existed, only the
// "deploy fails -> rollback succeeds" (ROLLED_BACK) path had ever been
// exercised, manually. The "rollback also fails" (CRITICAL_FAILURE) path had
// never been triggered at all.
// ---------------------------------------------------------------------------

namespace {

DeploymentOutcome runWithScriptExitCodes(int deploy_exit_code, int rollback_exit_code) {
    const std::string deploy_script = "/tmp/deploy_guard_test_outcome_deploy.sh";
    const std::string rollback_script = "/tmp/deploy_guard_test_outcome_rollback.sh";
    const std::string target_path = "/tmp/deploy_guard_test_outcome_target";
    ::mkdir(target_path.c_str(), 0755);

    writeExecutableScript(deploy_script, "exit " + std::to_string(deploy_exit_code) + "\n");
    writeExecutableScript(rollback_script, "exit " + std::to_string(rollback_exit_code) + "\n");

    auto logger = std::make_shared<Logger>("test_outcome.log");
    auto proc_mgr = std::make_shared<ProcessManager>(logger);

    Config config;
    config.target_path = target_path;
    config.deploy_script = deploy_script;
    config.rollback_script = rollback_script;

    DeployController controller(logger, proc_mgr, config);
    DeploymentOutcome outcome = controller.runDeployment();

    std::remove((target_path + "/.deploy_guard.state").c_str());
    ::rmdir(target_path.c_str());
    std::remove(deploy_script.c_str());
    std::remove(rollback_script.c_str());
    std::remove("test_outcome.log");

    return outcome;
}

} // namespace

TEST_CASE("DeployController outcome: deploy succeeds -> SUCCESS (exit code 0)") {
    CHECK(runWithScriptExitCodes(0, 0) == DeploymentOutcome::SUCCESS);
}

TEST_CASE("DeployController outcome: deploy fails, rollback succeeds -> ROLLED_BACK (exit code 1)") {
    CHECK(runWithScriptExitCodes(1, 0) == DeploymentOutcome::ROLLED_BACK);
}

TEST_CASE("DeployController outcome: deploy fails, rollback also fails -> CRITICAL_FAILURE (exit code 2)") {
    DeploymentOutcome outcome = runWithScriptExitCodes(1, 1);
    CHECK(outcome == DeploymentOutcome::CRITICAL_FAILURE);
    CHECK(outcome != DeploymentOutcome::ROLLED_BACK);
}
