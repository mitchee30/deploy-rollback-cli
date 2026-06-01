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
    // Configuration Structs
    // ------------------------------------------------------------------------
    struct Config {
        std::string target_path;
        std::string deploy_script;
        std::string rollback_script;
    };

    // ------------------------------------------------------------------------
    // Enums
    // ------------------------------------------------------------------------
    enum class LogLevel {
        INFO,
        WARNING,
        ERROR,
        SUCCESS
    };

    enum class DeploymentState {
        STABLE,
        INSTALLING,
        ROLLING_BACK
    };

    // ------------------------------------------------------------------------
    // Logger Class
    // Handles persistent logging to a file and colored console output.
    // ------------------------------------------------------------------------
    class Logger {
    public:
        explicit Logger(const std::string& log_file_path);
        ~Logger();

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
        DeployController(std::shared_ptr<Logger> logger, std::shared_ptr<ProcessManager> proc_mgr, Config config);

        void checkAndRecoverState();
        bool runDeployment();

    private:
        std::shared_ptr<Logger> logger_;
        std::shared_ptr<ProcessManager> proc_mgr_;
        Config config_;

        bool initiateRollback();
        void writeState(DeploymentState state);
        DeploymentState readState();
        void clearState();
        std::string getStateFilePath() const;
    };

    // ------------------------------------------------------------------------
    // ConfigParser Class
    // Reads and parses YAML configuration files into the Config struct.
    // ------------------------------------------------------------------------
    class ConfigParser {
    public:
        static std::optional<Config> parseYaml(const std::string& filepath, std::shared_ptr<Logger> logger);
    };

    // ------------------------------------------------------------------------
    // ArgumentParser Struct
    // ------------------------------------------------------------------------
    struct ArgumentParser {
        // Returns true if a config file is provided, false if fallback --path is used.
        // It populates either config_file or fallback_path.
        static bool parseArgs(int argc, char* argv[], std::string& config_file, std::string& fallback_path);
    };

} // namespace DeployGuard

#endif // DEPLOY_CLI_H
