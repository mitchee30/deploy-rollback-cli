#include <doctest/doctest.h>

#include "deployguard/process_manager.h"
#include "test_helpers.h"

#include <atomic>
#include <cerrno>
#include <csignal>

#include <fcntl.h>

using namespace DeployGuard;
using namespace testutil;
using std::chrono::milliseconds;
using std::chrono::seconds;
using Status = ExecResult::Status;

namespace {

struct PmFixture {
    TempDir dir;
    std::shared_ptr<Logger> logger = makeLogger(dir);
    ProcessManager pm{logger};

    ExecResult sh(const std::string& script, const ExecOptions& options = {}) const {
        return pm.execute({"/bin/sh", "-c", script}, options);
    }
    std::string log() const { return readFile(dir / "test.log"); }
};

} // namespace

// ---------------------------------------------------------------------------
// Exit status
// ---------------------------------------------------------------------------

TEST_CASE("ProcessManager: exit codes are reported as-is") {
    PmFixture f;
    for (int code : {0, 1, 42}) {
        CAPTURE(code);
        const ExecResult r = f.sh("exit " + std::to_string(code));
        CHECK(r.status == Status::Exited);
        CHECK(r.exit_code == code);
        CHECK(r.succeeded() == (code == 0));
    }
}

TEST_CASE("ProcessManager: a child killed by a signal is reported as Signaled with 128+N") {
    PmFixture f;
    const ExecResult r = f.sh("kill -USR1 $$");
    CHECK(r.status == Status::Signaled);
    CHECK(r.term_signal == SIGUSR1);
    CHECK(r.exit_code == 128 + SIGUSR1);
    CHECK_FALSE(r.succeeded());
    CHECK(contains(r.describe(), "killed by signal"));
}

TEST_CASE("ProcessManager: an empty command is rejected") {
    PmFixture f;
    CHECK_THROWS_AS(f.pm.execute({}), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// "Could not start" vs "ran and failed"
// ---------------------------------------------------------------------------

TEST_CASE("ProcessManager: a missing program is SpawnFailed(ENOENT), not a generic exit 127") {
    PmFixture f;
    const ExecResult r = f.pm.execute({f.dir / "no_such_script.sh"});
    CHECK(r.status == Status::SpawnFailed);
    CHECK(r.spawn_errno == ENOENT);
    CHECK(r.exit_code == 127);
    CHECK(contains(r.describe(), "could not be started"));
}

TEST_CASE("ProcessManager: a non-executable file is SpawnFailed(EACCES)") {
    PmFixture f;
    writeFile(f.dir / "not_executable.sh", "#!/bin/sh\nexit 0\n");  // mode 0644
    const ExecResult r = f.pm.execute({f.dir / "not_executable.sh"});
    CHECK(r.status == Status::SpawnFailed);
    CHECK(r.spawn_errno == EACCES);
}

TEST_CASE("ProcessManager: a script that really exits 127 is distinguishable from spawn failure") {
    PmFixture f;
    const ExecResult r = f.sh("exit 127");
    CHECK(r.status == Status::Exited);
    CHECK(r.exit_code == 127);
    CHECK(r.spawn_errno == 0);
}

// ---------------------------------------------------------------------------
// Arguments are never interpreted by a shell
// ---------------------------------------------------------------------------

TEST_CASE("ProcessManager: arguments reach the program literally (no shell injection)") {
    PmFixture f;
    const std::string script = f.dir / "record_arg.sh";
    const std::string received = f.dir / "received";
    // printf, not echo: echo may interpret backslashes.
    writeScript(script, "printf '%s' \"$1\" > '" + received + "'\n");

    const std::string marker = f.dir / "pwned";
    const std::vector<std::string> payloads = {
        "/tmp/foo'; touch " + marker + "; echo '",
        "/tmp/foo`touch " + marker + "`",
        "/tmp/foo$(touch " + marker + ")",
        "/tmp/foo && touch " + marker,
        "/tmp/foo | touch " + marker,
        "/tmp/path with  spaces\tand tab",
        "--looks-like-an-option",
    };
    for (const auto& payload : payloads) {
        CAPTURE(payload);
        const ExecResult r = f.pm.execute({script, payload});
        CHECK(r.succeeded());
        CHECK(readFile(received) == payload);  // received byte-for-byte as one argument
        CHECK_FALSE(exists(marker));            // and nothing was executed
    }
}

TEST_CASE("ProcessManager: extra environment variables reach the child") {
    PmFixture f;
    ExecOptions options;
    options.extra_env = {{"DG_TEST_VALUE", "hello world"}};
    CHECK(f.sh("[ \"$DG_TEST_VALUE\" = 'hello world' ]", options).succeeded());
    CHECK_FALSE(f.sh("[ -n \"$DG_TEST_VALUE\" ]").succeeded());  // not leaked into later runs
}

// ---------------------------------------------------------------------------
// Output handling
// ---------------------------------------------------------------------------

TEST_CASE("ProcessManager: stdout and stderr are both captured in the log file") {
    PmFixture f;
    REQUIRE(f.sh("echo to-stdout; echo to-stderr >&2").succeeded());
    const std::string log = f.log();
    CHECK(contains(log, "[STDOUT]  [sh] to-stdout"));
    CHECK(contains(log, "[STDERR]  [sh] to-stderr"));
}

TEST_CASE("ProcessManager: large output does not deadlock") {
    // If we stopped reading, the child would block on a full pipe (~64 KiB).
    PmFixture f;
    ExecOptions options;
    options.timeout = seconds(30);
    const ExecResult r = f.sh("i=0; while [ $i -lt 20000 ]; do echo \"line $i of output\"; i=$((i+1)); done; "
                              "echo err-done >&2",
                              options);
    CHECK(r.succeeded());
    const std::string log = f.log();
    CHECK(contains(log, "line 19999 of output"));
    CHECK(contains(log, "err-done"));
}

TEST_CASE("ProcessManager: stdin is /dev/null, so a prompting script cannot hang the deployment") {
    PmFixture f;
    ExecOptions options;
    options.timeout = seconds(10);
    const auto start = std::chrono::steady_clock::now();
    const ExecResult r = f.sh("if read answer; then echo GOT-INPUT; else echo GOT-EOF; fi", options);
    CHECK(r.succeeded());
    CHECK(std::chrono::steady_clock::now() - start < seconds(3));
    CHECK(contains(f.log(), "GOT-EOF"));
}

TEST_CASE("ProcessManager: returns when the child exits even if a background grandchild keeps stdout open") {
    // The shape of `app.sh --daemon &` inside a deploy script. Waiting for
    // pipe EOF instead of the child's exit would block for the full 30s.
    PmFixture f;
    const auto start = std::chrono::steady_clock::now();
    const ExecResult r = f.sh("sleep 30 & echo $! > '" + (f.dir / "gc.pid") + "'; exit 0");
    CHECK(r.succeeded());
    CHECK(std::chrono::steady_clock::now() - start < seconds(3));
    killIfRunning(readPid(f.dir / "gc.pid"));
}

TEST_CASE("ProcessManager: same for a background grandchild holding stderr") {
    PmFixture f;
    const auto start = std::chrono::steady_clock::now();
    const ExecResult r = f.sh("sleep 30 >/dev/null & echo $! > '" + (f.dir / "gc.pid") + "'; exit 0");
    CHECK(r.succeeded());
    CHECK(std::chrono::steady_clock::now() - start < seconds(3));
    killIfRunning(readPid(f.dir / "gc.pid"));
}

// ---------------------------------------------------------------------------
// Timeouts and cancellation
// ---------------------------------------------------------------------------

TEST_CASE("ProcessManager: a timeout stops a hung script") {
    PmFixture f;
    ExecOptions options;
    options.timeout = milliseconds(300);
    const ExecResult r = f.sh("sleep 30", options);
    CHECK(r.status == Status::TimedOut);
    CHECK_FALSE(r.succeeded());
    CHECK(r.duration < seconds(5));
    CHECK(contains(r.describe(), "timed out"));
    CHECK(contains(f.log(), "SIGTERM"));
}

TEST_CASE("ProcessManager: a timeout also stops everything the script started (process group)") {
    PmFixture f;
    ExecOptions options;
    options.timeout = milliseconds(300);
    const std::string pid_file = f.dir / "grandchild.pid";
    const ExecResult r = f.sh("sleep 30 & echo $! > '" + pid_file + "'; wait", options);
    CHECK(r.status == Status::TimedOut);

    const pid_t grandchild = readPid(pid_file);
    REQUIRE(grandchild > 0);
    CHECK(waitUntil([&] { return !processIsRunning(grandchild); }, seconds(5)));
    killIfRunning(grandchild);
}

TEST_CASE("ProcessManager: a script that ignores SIGTERM is SIGKILLed after the grace period") {
    PmFixture f;
    ExecOptions options;
    options.timeout = milliseconds(200);
    options.kill_grace_period = milliseconds(300);
    // `trap '' TERM` makes the shell - and the sleep it starts - ignore SIGTERM.
    const ExecResult r = f.sh("trap '' TERM; sleep 30", options);
    CHECK(r.status == Status::TimedOut);
    CHECK(r.term_signal == SIGKILL);
    CHECK(r.duration < seconds(5));
    CHECK(contains(f.log(), "SIGKILL"));
}

TEST_CASE("ProcessManager: should_cancel stops the child") {
    PmFixture f;
    const auto start = std::chrono::steady_clock::now();
    ExecOptions options;
    options.should_cancel = [start] { return std::chrono::steady_clock::now() - start > milliseconds(200); };
    const ExecResult r = f.sh("sleep 30", options);
    CHECK(r.status == Status::Cancelled);
    CHECK(r.duration < seconds(5));
    CHECK(contains(r.describe(), "cancelled"));
}

TEST_CASE("ProcessManager: no timeout by default") {
    PmFixture f;
    const ExecResult r = f.sh("sleep 1; exit 3");
    CHECK(r.status == Status::Exited);
    CHECK(r.exit_code == 3);
    CHECK(r.duration >= milliseconds(900));
}

// ---------------------------------------------------------------------------
// What the child inherits
// ---------------------------------------------------------------------------

TEST_CASE("ProcessManager: our open file descriptors are not inherited by the child") {
    PmFixture f;
    constexpr int kFd = 9;  // the highest fd number every POSIX shell can redirect
    if (::fcntl(kFd, F_GETFD) != -1) {
        MESSAGE("fd 9 already in use in this process; skipping");
        return;
    }
    // Deliberately *without* O_CLOEXEC - like a descriptor opened by a library.
    const int fd = ::open((f.dir / "secret").c_str(), O_WRONLY | O_CREAT, 0644);
    REQUIRE(fd >= 0);
    REQUIRE(::dup2(fd, kFd) == kFd);
    ::close(fd);

    const ExecResult r = f.sh("if (true >&9) 2>/dev/null; then echo FD-LEAKED; else echo FD-CLOSED; fi");
    ::close(kFd);

    CHECK(r.succeeded());
    CHECK(contains(f.log(), "FD-CLOSED"));
    CHECK_FALSE(contains(f.log(), "FD-LEAKED"));
}

TEST_CASE("ProcessManager: the child gets default SIGPIPE even though deploy-cli ignores it") {
    PmFixture f;
    struct sigaction ignore {};
    struct sigaction previous {};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    ::sigaction(SIGPIPE, &ignore, &previous);

    const ExecResult r = f.sh("kill -PIPE $$; echo survived");

    ::sigaction(SIGPIPE, &previous, nullptr);
    CHECK(r.status == Status::Signaled);
    CHECK(r.term_signal == SIGPIPE);
}

TEST_CASE("ProcessManager: the child runs in its own process group") {
    PmFixture f;
    const std::string out = f.dir / "pgid";
    REQUIRE(f.sh("ps -o pgid= -p $$ > '" + out + "'; echo $$ >> '" + out + "'").succeeded());
    std::istringstream in(readFile(out));
    long pgid = 0;
    long pid = 0;
    in >> pgid >> pid;
    CHECK(pgid == pid);               // the shell leads its own group...
    CHECK(pgid != ::getpgrp());       // ...which is not ours
}

// ---------------------------------------------------------------------------
// describe()
// ---------------------------------------------------------------------------

TEST_CASE("ExecResult::describe") {
    ExecResult r;
    r.status = Status::Exited;
    r.exit_code = 3;
    CHECK(r.describe() == std::string("exited with status 3"));

    r.status = Status::Cancelled;
    CHECK(r.describe() == std::string("was cancelled and terminated"));

    r.status = Status::TimedOut;
    r.duration = milliseconds(2500);
    CHECK(r.describe() == std::string("timed out after 2.5s and was terminated"));

    r.status = Status::SpawnFailed;
    r.spawn_errno = ENOENT;
    CHECK(contains(r.describe(), "could not be started"));
}
