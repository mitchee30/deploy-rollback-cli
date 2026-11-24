#ifndef DEPLOYGUARD_LOGGER_H
#define DEPLOYGUARD_LOGGER_H

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

namespace DeployGuard {

enum class LogLevel { INFO, WARNING, ERROR, SUCCESS };

// Which pipe a chunk of hook-script output arrived on.
enum class ChildStream { Stdout, Stderr };

// How the logger writes to the terminal.
enum class ConsoleMode {
    Auto,   // colored if stdout is a terminal and NO_COLOR is not set
    Plain,  // never colored
    Off     // no console output at all (used by unit tests)
};

// ------------------------------------------------------------------------
// Logger
//
// Writes our own messages and the output of the hook scripts to the console
// and (optionally) appends them to a log file.
//
// Console: hook output is passed through unchanged and immediately (stdout to
// stdout, stderr to stderr), so progress output from long scripts shows up
// live.
//
// Log file: every line - ours *and* the scripts' stdout and stderr - gets a
// timestamp and a tag, so the file alone is enough for a post-mortem. (Before
// v0.2 only the scripts' stdout reached the file, which meant the actual error
// message, usually printed on stderr, was missing from it.)
// ------------------------------------------------------------------------
class Logger {
public:
    // Log files are appended to across runs. One that has grown past this
    // size is renamed to "<file>.1" (replacing the previous one) first.
    static constexpr std::uintmax_t kDefaultRotateBytes = std::uintmax_t{10} * 1024 * 1024;

    // An empty `log_file_path` disables file logging.
    explicit Logger(const std::string& log_file_path, ConsoleMode console = ConsoleMode::Auto,
                    std::uintmax_t rotate_at_bytes = kDefaultRotateBytes);
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(LogLevel level, const std::string& message);

    // Called by ProcessManager as output arrives from a hook script. Chunks
    // can end mid-line; partial lines are buffered until the newline arrives
    // (or until flushChildOutput()).
    void childOutput(ChildStream stream, const std::string& source, std::string_view chunk);

    // Writes out any buffered partial line once the child has finished.
    void flushChildOutput(const std::string& source);

    bool fileLoggingEnabled() const { return log_file_.is_open(); }

    // True if stdout is a terminal, NO_COLOR is unset and TERM is not "dumb".
    static bool consoleSupportsColor();

private:
    std::ofstream log_file_;
    ConsoleMode console_;
    bool use_color_;
    std::string pending_stdout_;
    std::string pending_stderr_;

    void writeChildLineToFile(ChildStream stream, const std::string& source, const std::string& line);
};

} // namespace DeployGuard

#endif // DEPLOYGUARD_LOGGER_H
