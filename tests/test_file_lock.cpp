#include <doctest/doctest.h>

#include "deployguard/file_lock.h"
#include "test_helpers.h"

#include <cstdlib>

using namespace DeployGuard;
using namespace testutil;

TEST_CASE("FileLock: the first acquire succeeds and records our pid") {
    TempDir dir;
    std::string error;
    const auto lock = FileLock::tryAcquire(dir / "app.lock", &error);
    REQUIRE(lock.has_value());
    CHECK(lock->path() == dir / "app.lock");
    CHECK(contains(readFile(dir / "app.lock"), std::to_string(::getpid())));
}

TEST_CASE("FileLock: a second acquire fails while the first is held, and says who holds it") {
    // flock() locks belong to an open file description, so a second open()
    // of the same file conflicts even inside one process - which lets us
    // test the two-instances case without starting a second process.
    TempDir dir;
    const auto first = FileLock::tryAcquire(dir / "app.lock");
    REQUIRE(first.has_value());

    std::string error;
    const auto second = FileLock::tryAcquire(dir / "app.lock", &error);
    CHECK_FALSE(second.has_value());
    CHECK(contains(error, "already running"));
    CHECK(contains(error, "pid " + std::to_string(::getpid())));
}

TEST_CASE("FileLock: released by the destructor") {
    TempDir dir;
    {
        const auto lock = FileLock::tryAcquire(dir / "app.lock");
        REQUIRE(lock.has_value());
    }
    CHECK(FileLock::tryAcquire(dir / "app.lock").has_value());
}

TEST_CASE("FileLock: moving transfers ownership without releasing") {
    TempDir dir;
    auto original = FileLock::tryAcquire(dir / "app.lock");
    REQUIRE(original.has_value());

    FileLock moved = std::move(*original);
    original.reset();  // destroys the moved-from object: must not unlock
    CHECK_FALSE(FileLock::tryAcquire(dir / "app.lock").has_value());

    auto other = FileLock::tryAcquire(dir / "other.lock");
    REQUIRE(other.has_value());
    moved = std::move(*other);  // move-assignment releases app.lock
    CHECK(FileLock::tryAcquire(dir / "app.lock").has_value());
}

TEST_CASE("FileLock: a missing directory is an error, not a crash") {
    TempDir dir;
    std::string error;
    CHECK_FALSE(FileLock::tryAcquire(dir / "no/such/dir/app.lock", &error).has_value());
    CHECK(contains(error, "cannot open lock file"));
}

TEST_CASE("FileLock::isAvailable never creates or locks the file") {
    TempDir dir;
    const std::string path = dir / "app.lock";
    CHECK(FileLock::isAvailable(path));
    CHECK_FALSE(exists(path));

    {
        const auto lock = FileLock::tryAcquire(path);
        REQUIRE(lock.has_value());
        std::string error;
        CHECK_FALSE(FileLock::isAvailable(path, &error));
        CHECK(contains(error, "already running"));
    }
    CHECK(FileLock::isAvailable(path));
    CHECK(FileLock::tryAcquire(path).has_value());  // isAvailable left it unlocked
}

TEST_CASE("FileLock: the lock is not inherited by processes we start (O_CLOEXEC)") {
    // Regression scenario: the deploy hook starts a daemon in the background.
    // If the lock descriptor were inherited, the daemon would keep the lock
    // held after deploy-cli exits, and every later run would be refused.
    TempDir dir;
    const std::string path = dir / "app.lock";
    const std::string pid_file = dir / "daemon.pid";

    auto lock = FileLock::tryAcquire(path);
    REQUIRE(lock.has_value());

    // system() forks and execs /bin/sh, which starts a background "daemon".
    const std::string cmd = "sleep 30 >/dev/null 2>&1 & echo $! > '" + pid_file + "'";
    REQUIRE(std::system(cmd.c_str()) == 0);
    const pid_t daemon = readPid(pid_file);
    REQUIRE(processIsRunning(daemon));

    lock.reset();  // we release; the daemon is still running
    CHECK(FileLock::tryAcquire(path).has_value());

    killIfRunning(daemon);
}
