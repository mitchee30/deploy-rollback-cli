#include "deployguard/argument_parser.h"
#include "deployguard/config.h"
#include "deployguard/deploy_controller.h"
#include "deployguard/logger.h"
#include "deployguard/process_manager.h"
#include "deployguard/report.h"
#include "deployguard/signals.h"

#include <cerrno>
#include <csignal>
#include <iostream>
#include <memory>

#include <fcntl.h>
#include <unistd.h>

using namespace DeployGuard;

namespace {

// If we are started with stdin/stdout/stderr closed, the next open() would
// reuse descriptor 0, 1 or 2 - and e.g. log output could end up in the lock
// file. Point any closed standard descriptor at /dev/null first.
void ensureStandardFdsOpen() {
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
        if (::fcntl(fd, F_GETFD) == -1 && errno == EBADF) {
            const int null_fd = ::open("/dev/null", O_RDWR);
            if (null_fd != -1 && null_fd != fd) {
                ::dup2(null_fd, fd);
                ::close(null_fd);
            }
        }
    }
}

const char* signalDescription(int sig) {
    switch (sig) {
        case SIGINT:  return "SIGINT (Ctrl+C)";
        case SIGTERM: return "SIGTERM";
        case SIGHUP:  return "SIGHUP (terminal closed)";
        default:      return "a termination signal";
    }
}

} // namespace

int main(int argc, char* argv[]) {
    ensureStandardFdsOpen();

    const CliParseResult parsed = ArgumentParser::parse(argc, argv);
    if (!parsed.ok()) {
        std::cerr << "deploy-cli: " << parsed.error << "\n"
                  << "Try 'deploy-cli --help' for more information.\n";
        return toExitCode(DeploymentOutcome::ABORTED);
    }
    const CliOptions& options = parsed.options;
    if (options.show_help) {
        std::cout << ArgumentParser::usage();
        return 0;
    }
    if (options.show_version) {
        std::cout << "deploy-cli " << DEPLOYGUARD_VERSION << "\n";
        return 0;
    }

    installSignalHandlers();
    auto logger = std::make_shared<Logger>(options.log_file);

    // --report: written for every outcome from here on, including "invalid
    // configuration", so CI always finds a result file. A failure to write
    // it is logged but does not change the exit code: the deployment's
    // outcome is what matters.
    auto finishWith = [&](const RunSummary& summary) {
        if (!options.report_file.empty()) {
            std::string error;
            if (writeReport(options.report_file, summary, &error)) {
                logger->log(LogLevel::INFO, "Wrote report to " + options.report_file);
            } else {
                logger->log(LogLevel::ERROR, "Could not write the report: " + error);
            }
        }
        return summary.exit_code;
    };
    auto abortedBeforeStart = [&](const std::string& reason) {
        RunSummary summary;
        summary.reason = reason;
        summary.started_at = utcTimestamp();
        return finishWith(summary);
    };

    Config config;
    if (!options.config_file.empty()) {
        std::string error;
        std::optional<Config> loaded = ConfigParser::parseYaml(options.config_file, logger, &error);
        if (!loaded) {
            return abortedBeforeStart(error);  // already logged
        }
        config = *loaded;
    } else {
        logger->log(LogLevel::INFO, "No --config given: using the bundled hooks in ./scripts/");
        config.target_path = options.target_path;
        config.deploy_script = "./scripts/deploy.sh";
        config.rollback_script = "./scripts/rollback.sh";
        config.health_check_script = "./scripts/health_check.sh";
    }
    if (options.timeout_seconds) {
        config.timeout_seconds = *options.timeout_seconds;
    }
    if (const std::string problem = ConfigParser::validate(config); !problem.empty()) {
        logger->log(LogLevel::ERROR, "Invalid configuration: " + problem);
        return abortedBeforeStart("invalid configuration: " + problem);
    }

    // Polled by the deployment loop. Logs which signal arrived, once.
    auto announced = std::make_shared<bool>(false);
    auto cancel_requested = [logger, announced]() {
        if (!cancellationRequested()) {
            return false;
        }
        if (!*announced) {
            *announced = true;
            logger->log(LogLevel::WARNING, std::string("Received ") + signalDescription(cancellationSignal()) +
                                               ": cancelling the deployment.");
        }
        return true;
    };

    auto proc_mgr = std::make_shared<ProcessManager>(logger);
    DeployController controller(logger, proc_mgr, config, cancel_requested);
    if (options.dry_run) {
        controller.dryRun();
    } else {
        controller.run();
    }
    return finishWith(controller.summary());
}
