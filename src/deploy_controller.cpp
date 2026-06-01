#include "../include/deploy-cli.h"
#include <iostream>

namespace DeployGuard {

DeployController::DeployController(std::shared_ptr<Logger> logger, std::shared_ptr<ProcessManager> proc_mgr)
    : logger_(std::move(logger)), proc_mgr_(std::move(proc_mgr)) {}

bool DeployController::runDeployment(const std::string& target_path) {
    logger_->log(LogLevel::INFO, "Initiating deployment state machine for target: " + target_path);
    
    std::string deploy_cmd = "./scripts/deploy.sh '" + target_path + "'";
    
    try {
        int deploy_res = proc_mgr_->execute(deploy_cmd);
        
        if (deploy_res != 0) {
            logger_->log(LogLevel::ERROR, "Deployment script exited with non-zero status: " + std::to_string(deploy_res));
            logger_->log(LogLevel::WARNING, "Halting deployment and triggering automatic rollback...");
            return initiateRollback(target_path);
        }
        
    } catch (const std::exception& e) {
        logger_->log(LogLevel::ERROR, std::string("Deployment subsystem failure: ") + e.what());
        return initiateRollback(target_path);
    }

    logger_->log(LogLevel::SUCCESS, "Deployment completed successfully. No rollback required.");
    return true;
}

bool DeployController::initiateRollback(const std::string& target_path) {
    logger_->log(LogLevel::INFO, "Initiating rollback sequence for target: " + target_path);
    
    std::string rollback_cmd = "./scripts/rollback.sh '" + target_path + "'";
    
    try {
        int rollback_res = proc_mgr_->execute(rollback_cmd);
        
        if (rollback_res == 0) {
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

} // namespace DeployGuard
