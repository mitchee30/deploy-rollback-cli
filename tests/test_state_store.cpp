#include <doctest/doctest.h>

#include "deployguard/state_store.h"
#include "test_helpers.h"

using namespace DeployGuard;
using namespace testutil;

TEST_CASE("deriveTargetPaths: state, lock and backup live next to the target, not inside it") {
    const TargetPaths p = deriveTargetPaths("/srv/myapp");
    CHECK(p.target == std::string("/srv/myapp"));
    CHECK(p.state_file == std::string("/srv/.deploy_guard_myapp.state"));
    CHECK(p.lock_file == std::string("/srv/.deploy_guard_myapp.lock"));
    CHECK(p.backup_dir == std::string("/srv/.deploy_guard_myapp.backup"));
}

TEST_CASE("deriveTargetPaths: trailing slashes and .. are normalised") {
    CHECK(deriveTargetPaths("/srv/myapp/").state_file == std::string("/srv/.deploy_guard_myapp.state"));
    CHECK(deriveTargetPaths("/srv/other/../myapp").target == std::string("/srv/myapp"));
}

TEST_CASE("deriveTargetPaths: relative paths become absolute") {
    const TargetPaths p = deriveTargetPaths("./test_app_target");
    REQUIRE_FALSE(p.target.empty());
    CHECK(p.target.front() == '/');
    CHECK(contains(p.state_file, "/.deploy_guard_test_app_target.state"));
}

TEST_CASE("StateStore: a missing file means STABLE") {
    TempDir dir;
    const StateSnapshot s = StateStore(dir / "state").read();
    CHECK(s.valid);
    CHECK(s.state == DeploymentState::STABLE);
}

TEST_CASE("StateStore: write then read round-trips, with diagnostic details") {
    TempDir dir;
    StateStore store(dir / "state");

    std::string error;
    REQUIRE(store.write(DeploymentState::INSTALLING, &error));
    StateSnapshot s = store.read();
    CHECK(s.valid);
    CHECK(s.state == DeploymentState::INSTALLING);
    CHECK(contains(s.detail, "pid=" + std::to_string(::getpid())));
    CHECK(contains(s.detail, "updated_at="));

    REQUIRE(store.write(DeploymentState::ROLLING_BACK, &error));
    s = store.read();
    CHECK(s.state == DeploymentState::ROLLING_BACK);
    // Replaced, not appended to.
    CHECK(readFile(dir / "state").rfind("ROLLING_BACK\n", 0) == 0);
    CHECK_FALSE(contains(readFile(dir / "state"), "INSTALLING"));
}

TEST_CASE("StateStore: no temporary file is left behind after a write") {
    TempDir dir;
    StateStore store(dir / "state");
    REQUIRE(store.write(DeploymentState::INSTALLING));
    CHECK(exists(dir / "state"));
    CHECK_FALSE(exists(dir / "state.tmp"));
}

TEST_CASE("StateStore: clear() removes the file, is idempotent, and cleans a stale .tmp") {
    TempDir dir;
    StateStore store(dir / "state");
    REQUIRE(store.write(DeploymentState::INSTALLING));
    writeFile(dir / "state.tmp", "INSTALLING\n");  // as if a write had been interrupted

    CHECK(store.clear());
    CHECK_FALSE(exists(dir / "state"));
    CHECK_FALSE(exists(dir / "state.tmp"));
    CHECK(store.read().state == DeploymentState::STABLE);
    CHECK(store.clear());  // nothing to remove: still fine
}

TEST_CASE("StateStore: writing STABLE is the same as clear()") {
    TempDir dir;
    StateStore store(dir / "state");
    REQUIRE(store.write(DeploymentState::INSTALLING));
    REQUIRE(store.write(DeploymentState::STABLE));
    CHECK_FALSE(exists(dir / "state"));
}

TEST_CASE("StateStore: unrecognised or empty content is reported as invalid, not as STABLE") {
    // Treating garbage as "no deployment in progress" would silently skip
    // crash recovery. The caller refuses to continue instead.
    TempDir dir;
    StateStore store(dir / "state");

    writeFile(dir / "state", "HALF_WRITTEN_GARBAGE\n");
    StateSnapshot s = store.read();
    CHECK_FALSE(s.valid);
    CHECK(contains(s.detail, "HALF_WRITTEN_GARBAGE"));

    writeFile(dir / "state", "");
    s = store.read();
    CHECK_FALSE(s.valid);
    CHECK(contains(s.detail, "empty"));
}

TEST_CASE("StateStore: reads the v0.1 single-word format") {
    TempDir dir;
    writeFile(dir / "state", "INSTALLING");  // no newline, no details
    const StateSnapshot s = StateStore(dir / "state").read();
    CHECK(s.valid);
    CHECK(s.state == DeploymentState::INSTALLING);
}

TEST_CASE("StateStore: write() reports failure instead of pretending it worked") {
    TempDir dir;
    StateStore store(dir / "missing_dir/state");
    std::string error;
    CHECK_FALSE(store.write(DeploymentState::INSTALLING, &error));
    CHECK(contains(error, "missing_dir"));
}

TEST_CASE("stateName(DeploymentState)") {
    CHECK(std::string(stateName(DeploymentState::STABLE)) == std::string("STABLE"));
    CHECK(std::string(stateName(DeploymentState::INSTALLING)) == std::string("INSTALLING"));
    CHECK(std::string(stateName(DeploymentState::ROLLING_BACK)) == std::string("ROLLING_BACK"));
}

TEST_CASE("StateStore: a path that cannot be stat'ed is invalid, not STABLE") {
    TempDir dir;
    writeFile(dir / "a_file", "x");
    const StateSnapshot s = StateStore(dir / "a_file/state").read();  // ENOTDIR
    CHECK_FALSE(s.valid);
    CHECK(contains(s.detail, "cannot stat"));
}

TEST_CASE("StateStore: clear() reports a state path it cannot remove") {
    TempDir dir;
    fs::create_directory(dir / "state");  // unlink() fails on a directory
    std::string error;
    CHECK_FALSE(StateStore(dir / "state").clear(&error));
    CHECK(contains(error, "cannot remove"));
}

TEST_CASE("StateStore: blank lines in the details are ignored") {
    TempDir dir;
    writeFile(dir / "state", "ROLLING_BACK\n\npid=1\n\n");
    const StateSnapshot s = StateStore(dir / "state").read();
    CHECK(s.valid);
    CHECK(s.state == DeploymentState::ROLLING_BACK);
    CHECK(s.detail == std::string("pid=1"));
}

TEST_CASE("deriveTargetPaths: the root directory gets a usable name") {
    const TargetPaths p = deriveTargetPaths("/");
    CHECK(p.state_file == std::string("/.deploy_guard_root.state"));
}
