#include <doctest/doctest.h>

#include "deployguard/logger.h"
#include "test_helpers.h"

#include <cstdlib>
#include <iostream>
#include <sstream>

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

namespace {

// Temporarily redirects std::cout or std::cerr into a string.
class CaptureStream {
public:
    explicit CaptureStream(std::ostream& stream) : stream_(stream), old_(stream.rdbuf(buffer_.rdbuf())) {}
    ~CaptureStream() { stream_.rdbuf(old_); }
    CaptureStream(const CaptureStream&) = delete;
    CaptureStream& operator=(const CaptureStream&) = delete;
    std::string str() const { return buffer_.str(); }

private:
    std::ostream& stream_;
    std::ostringstream buffer_;
    std::streambuf* old_;
};

} // namespace

TEST_CASE("Logger: Plain console mode prints messages without color codes") {
    TempDir dir;
    CaptureStream out(std::cout);
    {
        Logger logger(dir / "run.log", ConsoleMode::Plain);
        logger.log(LogLevel::SUCCESS, "all good");
    }
    CHECK(contains(out.str(), "[SUCCESS] all good"));
    CHECK_FALSE(contains(out.str(), "\033["));
}

TEST_CASE("Logger: hook output is passed through to the matching console stream, unchanged") {
    CaptureStream out(std::cout);
    CaptureStream err(std::cerr);
    Logger logger("", ConsoleMode::Plain);
    logger.childOutput(ChildStream::Stdout, "s", "progress 50%");  // no newline: shown immediately
    logger.childOutput(ChildStream::Stderr, "s", "oops\n");
    CHECK(out.str() == std::string("progress 50%"));
    CHECK(err.str() == std::string("oops\n"));
}

TEST_CASE("Logger: Off mode prints nothing") {
    CaptureStream out(std::cout);
    CaptureStream err(std::cerr);
    Logger logger("", ConsoleMode::Off);
    logger.log(LogLevel::ERROR, "quiet");
    logger.childOutput(ChildStream::Stderr, "s", "quiet\n");
    CHECK(out.str().empty());
    CHECK(err.str().empty());
}

TEST_CASE("Logger: a log file that cannot be opened is reported on stderr") {
    TempDir dir;
    CaptureStream err(std::cerr);
    Logger logger(dir / "no/such/dir/run.log", ConsoleMode::Plain);
    CHECK(contains(err.str(), "cannot open log file"));
}

TEST_CASE("Logger: TERM=dumb disables color") {
    const char* old = std::getenv("TERM");
    const std::string saved = old != nullptr ? old : "";
    ::setenv("TERM", "dumb", 1);
    CHECK_FALSE(Logger::consoleSupportsColor());
    if (old != nullptr) {
        ::setenv("TERM", saved.c_str(), 1);
    } else {
        ::unsetenv("TERM");
    }
}

TEST_CASE("Logger: a partial stderr line is flushed at the end") {
    TempDir dir;
    {
        Logger logger(dir / "run.log", ConsoleMode::Off);
        logger.childOutput(ChildStream::Stderr, "s", "dying words without newline");
        logger.flushChildOutput("s");
    }
    CHECK(contains(readFile(dir / "run.log"), "[STDERR]  [s] dying words without newline\n"));
}


TEST_CASE("Logger: a log file past the size limit is rotated to .1") {
    TempDir dir;
    const std::string path = dir / "run.log";
    writeFile(path, std::string(2000, 'x') + "\nOLD RUN\n");
    writeFile(path + ".1", "even older\n");
    {
        Logger logger(path, ConsoleMode::Off, /*rotate_at_bytes=*/1000);
        logger.log(LogLevel::INFO, "new run");
    }
    CHECK(contains(readFile(path + ".1"), "OLD RUN"));  // replaced the even older one
    CHECK_FALSE(contains(readFile(path + ".1"), "even older"));
    CHECK_FALSE(contains(readFile(path), "OLD RUN"));
    CHECK(contains(readFile(path), "new run"));
    CHECK(contains(readFile(path), "moved to"));
}

TEST_CASE("Logger: a log file under the limit is appended to, not rotated") {
    TempDir dir;
    const std::string path = dir / "run.log";
    writeFile(path, "OLD RUN\n");
    { Logger(path, ConsoleMode::Off, 1000).log(LogLevel::INFO, "new run"); }
    CHECK(contains(readFile(path), "OLD RUN"));
    CHECK_FALSE(exists(path + ".1"));
}
