#ifndef DEPLOY_CLI_H
#define DEPLOY_CLI_H

#include <string>
#include <vector>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <optional>

namespace DeployGuard {

    // ------------------------------------------------------------------------
    // Enums
    // ------------------------------------------------------------------------
    enum class LogLevel {
        INFO,
        WARNING,
        ERROR,
        SUCCESS
    };

    // ------------------------------------------------------------------------
    // Logger Class
    // Handles persistent logging to a file and colored console output.
    // ------------------------------------------------------------------------
    class Logger {
    public:
        explicit Logger(const std::string& log_file_path);
        ~Logger();

        // Non-copyable
        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        void log(LogLevel level, const std::string& message);
        void streamLog(const std::string& raw_output);

    private:
        std::ofstream log_file_;
        std::string getCurrentTimestamp() const;
        std::string getLevelString(LogLevel level) const;
        std::string getColorCode(LogLevel level) const;
        std::string getResetCode() const;
    };

    // ------------------------------------------------------------------------
    // ProcessManager Class
    // Encapsulates popen logic using RAII principles and realtime streaming.
    // ------------------------------------------------------------------------
    class ProcessManager {
    public:
        explicit ProcessManager(std::shared_ptr<Logger> logger);
        
        // Executes a command, streams output to logger, and returns the exit code.
        int execute(const std::string& command);

    private:
        std::shared_ptr<Logger> logger_;
    };

    // ------------------------------------------------------------------------
    // DeployController Class
    // Acts as a state machine governing the deployment and rollback logic.
    // ------------------------------------------------------------------------
    class DeployController {
    public:
        DeployController(std::shared_ptr<Logger> logger, std::shared_ptr<ProcessManager> proc_mgr);

        // Initiates the deployment sequence
        bool runDeployment(const std::string& target_path);

    private:
        std::shared_ptr<Logger> logger_;
        std::shared_ptr<ProcessManager> proc_mgr_;

        bool initiateRollback(const std::string& target_path);
    };

    // ------------------------------------------------------------------------
    // ArgumentParser Struct
    // Cleanly parses and validates command line inputs.
    // ------------------------------------------------------------------------
    struct ArgumentParser {
        static std::optional<std::string> parsePath(int argc, char* argv[]);
    };

} // namespace DeployGuard

#endif // DEPLOY_CLI_H
