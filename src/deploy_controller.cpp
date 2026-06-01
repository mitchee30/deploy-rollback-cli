#include "../include/deploy-cli.h"
#include <iostream>

namespace DeployGuard {

DeployController::DeployController(std::shared_ptr<Logger> logger, std::shared_ptr<ProcessManager> proc_mgr, Config config)
    : logger_(std::move(logger)), proc_mgr_(std::move(proc_mgr)), config_(std::move(config)) {}

bool DeployController::runDeployment() {
    logger_->log(LogLevel::INFO, "Initiating deployment state machine for target: " + config_.target_path);
    
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

    logger_->log(LogLevel::SUCCESS, "Deployment completed successfully. No rollback required.");
    return true;
}

bool DeployController::initiateRollback() {
    logger_->log(LogLevel::INFO, "Initiating rollback sequence for target: " + config_.target_path);
    
    std::string rollback_cmd = config_.rollback_script + " '" + config_.target_path + "'";
    
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
