#include "../include/deploy-cli.h"
#include <iostream>
#include <fstream>
#include <cstdio>

namespace DeployGuard {

DeployController::DeployController(std::shared_ptr<Logger> logger, std::shared_ptr<ProcessManager> proc_mgr, Config config)
    : logger_(std::move(logger)), proc_mgr_(std::move(proc_mgr)), config_(std::move(config)) {}

bool DeployController::runDeployment() {
    logger_->log(LogLevel::INFO, "Initiating deployment state machine for target: " + config_.target_path);
    
    // Set 'InstallFailed' protection flag
    writeState(DeploymentState::INSTALLING);
    
    // Inject the target path into the deployment script command
    std::string deploy_cmd = config_.deploy_script + " '" + config_.target_path + "'";
    
    try {
        int deploy_res = proc_mgr_->execute(deploy_cmd);
        
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
    return true;
}

bool DeployController::initiateRollback() {
    // Set 'RollbackInProgress' flag
    writeState(DeploymentState::ROLLING_BACK);
    
    logger_->log(LogLevel::INFO, "Initiating rollback sequence for target: " + config_.target_path);
    
    std::string rollback_cmd = config_.rollback_script + " '" + config_.target_path + "'";
    
    try {
        int rollback_res = proc_mgr_->execute(rollback_cmd);
        
        if (rollback_res == 0) {
            clearState(); // Cleanup rollback flag
            logger_->log(LogLevel::SUCCESS, "System successfully rolled back to a stable state.");
            return false; // returning false overall because the original deployment failed
        } else {
            logger_->log(LogLevel::ERROR, "CRITICAL FAILURE: Rollback script exited with non-zero status: " + std::to_string(rollback_res));
            return false;
        }
        
    } catch (const std::exception& e) {
        logger_->log(LogLevel::ERROR, std::string("Rollback subsystem catastrophic failure: ") + e.what());
        return false;
    }
}

std::string DeployController::getStateFilePath() const {
    return config_.target_path + "/.deploy_guard.state";
}

void DeployController::writeState(DeploymentState state) {
    std::ofstream ofs(getStateFilePath());
    if (state == DeploymentState::INSTALLING) {
        ofs << "INSTALLING";
    } else if (state == DeploymentState::ROLLING_BACK) {
        ofs << "ROLLING_BACK";
    }
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
