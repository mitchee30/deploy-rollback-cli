#include "../include/deploy-cli.h"
#include <iostream>
#include <fstream>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace DeployGuard {

DeployController::DeployController(std::shared_ptr<Logger> logger, std::shared_ptr<ProcessManager> proc_mgr, Config config)
    : logger_(std::move(logger)), proc_mgr_(std::move(proc_mgr)), config_(std::move(config)) {}

DeploymentOutcome DeployController::runDeployment() {
    logger_->log(LogLevel::INFO, "Initiating deployment state machine for target: " + config_.target_path);

    // Set 'InstallFailed' protection flag
    writeState(DeploymentState::INSTALLING);

    // Pass the target path as a literal argv element (no shell involved),
    // so it can never be reinterpreted as shell syntax regardless of its contents.
    std::vector<std::string> deploy_args = {config_.deploy_script, config_.target_path};

    try {
        int deploy_res = proc_mgr_->execute(deploy_args);

        if (deploy_res != 0) {
            logger_->log(LogLevel::ERROR, "Deployment script exited with non-zero status: " + std::to_string(deploy_res));
            logger_->log(LogLevel::WARNING, "Halting deployment and triggering automatic rollback...");
            return initiateRollback();
        }

    } catch (const std::exception& e) {
        logger_->log(LogLevel::ERROR, std::string("Deployment subsystem failure: ") + e.what());
        return initiateRollback();
    }

    // Clear protection flag
    clearState();
    logger_->log(LogLevel::SUCCESS, "Deployment completed successfully. No rollback required.");
    return DeploymentOutcome::SUCCESS;
}

DeploymentOutcome DeployController::initiateRollback() {
    // Set 'RollbackInProgress' flag
    writeState(DeploymentState::ROLLING_BACK);

    logger_->log(LogLevel::INFO, "Initiating rollback sequence for target: " + config_.target_path);

    std::vector<std::string> rollback_args = {config_.rollback_script, config_.target_path};

    try {
        int rollback_res = proc_mgr_->execute(rollback_args);

        if (rollback_res == 0) {
            clearState(); // Cleanup rollback flag
            logger_->log(LogLevel::SUCCESS, "System successfully rolled back to a stable state.");
            return DeploymentOutcome::ROLLED_BACK; // original deployment failed, but system is stable again
        } else {
            logger_->log(LogLevel::ERROR, "CRITICAL FAILURE: Rollback script exited with non-zero status: " + std::to_string(rollback_res));
            return DeploymentOutcome::CRITICAL_FAILURE;
        }

    } catch (const std::exception& e) {
        logger_->log(LogLevel::ERROR, std::string("Rollback subsystem catastrophic failure: ") + e.what());
        return DeploymentOutcome::CRITICAL_FAILURE;
    }
}

std::string DeployController::getStateFilePath() const {
    return config_.target_path + "/.deploy_guard.state";
}

void DeployController::writeState(DeploymentState state) {
    std::string content;
    if (state == DeploymentState::INSTALLING) {
        content = "INSTALLING";
    } else if (state == DeploymentState::ROLLING_BACK) {
        content = "ROLLING_BACK";
    } else {
        return;
    }

    const std::string path = getStateFilePath();

    // Use low-level POSIX I/O (instead of ofstream) so we can fsync the file
    // descriptor directly: on real power loss, a state write that hasn't been
    // fsync'd may never reach disk, which would silently defeat crash recovery.
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        logger_->log(LogLevel::ERROR, "Failed to open state file for writing: " + path);
        return;
    }

    ssize_t written = ::write(fd, content.data(), content.size());
    if (written != static_cast<ssize_t>(content.size())) {
        logger_->log(LogLevel::ERROR, "Failed to fully write state file: " + path);
    }

    if (::fsync(fd) != 0) {
        logger_->log(LogLevel::ERROR, "fsync() failed on state file: " + path);
    }

    ::close(fd);
}

DeploymentState DeployController::readState() {
    std::ifstream ifs(getStateFilePath());
    if (!ifs.is_open()) return DeploymentState::STABLE;

    std::string state;
    ifs >> state;
    if (state == "INSTALLING") return DeploymentState::INSTALLING;
    if (state == "ROLLING_BACK") return DeploymentState::ROLLING_BACK;
    return DeploymentState::STABLE;
}

void DeployController::clearState() {
    std::remove(getStateFilePath().c_str());
}

void DeployController::checkAndRecoverState() {
    DeploymentState current_state = readState();

    if (current_state == DeploymentState::INSTALLING) {
        logger_->log(LogLevel::ERROR, "CRITICAL: Detected interrupted installation (e.g., power loss or crash)!");
        logger_->log(LogLevel::WARNING, "Initiating self-healing auto-recovery...");
        initiateRollback();
    } else if (current_state == DeploymentState::ROLLING_BACK) {
        logger_->log(LogLevel::ERROR, "CRITICAL: Detected interrupted rollback!");
        logger_->log(LogLevel::WARNING, "Attempting to resume rollback cleanup...");
        initiateRollback();
    }
}

} // namespace DeployGuard
