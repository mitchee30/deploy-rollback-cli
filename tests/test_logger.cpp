#include <doctest/doctest.h>

#include "deployguard/logger.h"
#include "test_helpers.h"

#include <cstdlib>

using namespace DeployGuard;
using namespace testutil;

TEST_CASE("Logger: messages go to the file with level tags and a session header") {
    TempDir dir;
    {
        Logger logger(dir / "run.log", ConsoleMode::Off);
        CHECK(logger.fileLoggingEnabled());
        logger.log(LogLevel::INFO, "hello");
        logger.log(LogLevel::ERROR, "boom");
    }
    const std::string log = readFile(dir / "run.log");
    CHECK(contains(log, "===== deploy-cli"));
    CHECK(contains(log, "[INFO]    hello"));
    CHECK(contains(log, "[ERROR]   boom"));
}

TEST_CASE("Logger: the file is appended to, not truncated, across runs") {
    TempDir dir;
    { Logger(dir / "run.log", ConsoleMode::Off).log(LogLevel::INFO, "first run"); }
    { Logger(dir / "run.log", ConsoleMode::Off).log(LogLevel::INFO, "second run"); }
    const std::string log = readFile(dir / "run.log");
    CHECK(contains(log, "first run"));
    CHECK(contains(log, "second run"));
}

TEST_CASE("Logger: hook stdout AND stderr reach the log file, tagged per line") {
    // Before v0.2 only stdout was written to the file, so the actual error
    // message (usually on stderr) was missing from post-mortem logs.
    TempDir dir;
    {
        Logger logger(dir / "run.log", ConsoleMode::Off);
        logger.childOutput(ChildStream::Stdout, "deploy.sh", "installing\n");
        logger.childOutput(ChildStream::Stderr, "deploy.sh", "FATAL: schema mismatch\n");
    }
    const std::string log = readFile(dir / "run.log");
    CHECK(contains(log, "[STDOUT]  [deploy.sh] installing"));
    CHECK(contains(log, "[STDERR]  [deploy.sh] FATAL: schema mismatch"));
}

TEST_CASE("Logger: lines split across chunks are reassembled; partial lines are flushed") {
    TempDir dir;
    {
        Logger logger(dir / "run.log", ConsoleMode::Off);
        logger.childOutput(ChildStream::Stdout, "s", "hel");
        logger.childOutput(ChildStream::Stdout, "s", "lo wor");
        logger.childOutput(ChildStream::Stdout, "s", "ld\nsecond\r\nno newline at end");
        logger.flushChildOutput("s");
    }
    const std::string log = readFile(dir / "run.log");
    CHECK(contains(log, "[s] hello world\n"));
    CHECK(contains(log, "[s] second\n"));  // \r stripped
    CHECK(contains(log, "[s] no newline at end\n"));
}

TEST_CASE("Logger: a huge line without newline does not grow the buffer forever") {
    TempDir dir;
    {
        Logger logger(dir / "run.log", ConsoleMode::Off);
        const std::string chunk(4096, 'x');
        for (int i = 0; i < 40; ++i) {  // 160 KiB, no newline
            logger.childOutput(ChildStream::Stdout, "s", chunk);
        }
        // Written out in pieces before the end, not held until flush.
        CHECK(readFile(dir / "run.log").size() > 64 * 1024);
        logger.flushChildOutput("s");
    }
    CHECK(readFile(dir / "run.log").size() > 160 * 1024);
}

TEST_CASE("Logger: an empty path disables file logging; an unwritable path does not throw") {
    Logger none("", ConsoleMode::Off);
    CHECK_FALSE(none.fileLoggingEnabled());
    none.log(LogLevel::INFO, "goes nowhere");

    TempDir dir;
    Logger bad(dir / "no/such/dir/run.log", ConsoleMode::Off);
    CHECK_FALSE(bad.fileLoggingEnabled());
    bad.log(LogLevel::INFO, "still fine");
}

TEST_CASE("Logger: NO_COLOR disables colored output") {
    const char* old = std::getenv("NO_COLOR");
    const std::string saved = old != nullptr ? old : "";
    ::setenv("NO_COLOR", "1", 1);
    CHECK_FALSE(Logger::consoleSupportsColor());
    if (old != nullptr) {
        ::setenv("NO_COLOR", saved.c_str(), 1);
    } else {
        ::unsetenv("NO_COLOR");
    }
}
