#include "deployguard/logger.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <unistd.h>

namespace DeployGuard {

namespace {

// Lines longer than this without a newline are written out in pieces, so a
// script that prints megabytes without '\n' cannot grow our buffer forever.
constexpr size_t kMaxPendingLine = size_t{64} * 1024;

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
    localtime_r(&secs, &local);  // thread-safe, unlike std::localtime
    std::ostringstream os;
    os << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << millis;
    return os.str();
}

// All tags are 9 characters wide so messages line up in the log file.
const char* levelTag(LogLevel level) {
    switch (level) {
        case LogLevel::INFO:    return "[INFO]   ";
        case LogLevel::WARNING: return "[WARN]   ";
        case LogLevel::ERROR:   return "[ERROR]  ";
        case LogLevel::SUCCESS: return "[SUCCESS]";
    }
    return "[?]      ";
}

const char* colorCode(LogLevel level) {
    switch (level) {
        case LogLevel::INFO:    return "\033[36m";  // cyan
        case LogLevel::WARNING: return "\033[33m";  // yellow
        case LogLevel::ERROR:   return "\033[31m";  // red
        case LogLevel::SUCCESS: return "\033[32m";  // green
    }
    return "";
}

constexpr const char* kColorReset = "\033[0m";

} // namespace

bool Logger::consoleSupportsColor() {
    // https://no-color.org: any non-empty NO_COLOR disables color.
    const char* no_color = std::getenv("NO_COLOR");
    if (no_color != nullptr && no_color[0] != '\0') {
        return false;
    }
    const char* term = std::getenv("TERM");
    if (term != nullptr && std::strcmp(term, "dumb") == 0) {
        return false;
    }
    // Escape codes only make sense on a terminal - in a CI log or a
    // redirected file they show up as garbage like "[36m".
    return ::isatty(STDOUT_FILENO) == 1;
}

Logger::Logger(const std::string& log_file_path, ConsoleMode console)
    : console_(console), use_color_(console == ConsoleMode::Auto && consoleSupportsColor()) {
    if (log_file_path.empty()) {
        return;
    }
    log_file_.open(log_file_path, std::ios::app);
    if (!log_file_.is_open()) {
        if (console_ != ConsoleMode::Off) {
            std::cerr << "Warning: cannot open log file '" << log_file_path
                      << "'; logging to the console only." << std::endl;
        }
        return;
    }
    // The file is appended to across runs; mark where this run starts.
    log_file_ << "\n===== deploy-cli " << DEPLOYGUARD_VERSION << " started at " << timestamp()
              << " (pid " << ::getpid() << ") =====" << std::endl;
}

Logger::~Logger() = default;

void Logger::log(LogLevel level, const std::string& message) {
    const std::string line = "[" + timestamp() + "] " + levelTag(level) + " " + message;

    if (log_file_.is_open()) {
        log_file_ << line << std::endl;  // flushed per line: survives a crash
    }

    if (console_ == ConsoleMode::Off) {
        return;
    }
    if (use_color_) {
        std::cout << colorCode(level) << line << kColorReset << std::endl;
    } else {
        std::cout << line << std::endl;
    }
}

void Logger::childOutput(ChildStream stream, const std::string& source, std::string_view chunk) {
    if (console_ != ConsoleMode::Off) {
        std::ostream& os = (stream == ChildStream::Stdout) ? std::cout : std::cerr;
        os.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        os.flush();
    }

    if (!log_file_.is_open()) {
        return;
    }

    std::string& pending = (stream == ChildStream::Stdout) ? pending_stdout_ : pending_stderr_;
    pending.append(chunk.data(), chunk.size());

    size_t start = 0;
    size_t newline;
    while ((newline = pending.find('\n', start)) != std::string::npos) {
        writeChildLineToFile(stream, source, pending.substr(start, newline - start));
        start = newline + 1;
    }
    pending.erase(0, start);

    if (pending.size() >= kMaxPendingLine) {
        writeChildLineToFile(stream, source, pending);
        pending.clear();
    }
}

void Logger::flushChildOutput(const std::string& source) {
    if (!pending_stdout_.empty()) {
        writeChildLineToFile(ChildStream::Stdout, source, pending_stdout_);
        pending_stdout_.clear();
    }
    if (!pending_stderr_.empty()) {
        writeChildLineToFile(ChildStream::Stderr, source, pending_stderr_);
        pending_stderr_.clear();
    }
}

void Logger::writeChildLineToFile(ChildStream stream, const std::string& source, const std::string& line) {
    std::string text = line;
    if (!text.empty() && text.back() == '\r') {
        text.pop_back();
    }
    const char* tag = (stream == ChildStream::Stdout) ? "[STDOUT] " : "[STDERR] ";
    log_file_ << "[" << timestamp() << "] " << tag << " [" << source << "] " << text << std::endl;
}

} // namespace DeployGuard
