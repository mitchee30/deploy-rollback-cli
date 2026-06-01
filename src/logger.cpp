#include "../include/deploy-cli.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace DeployGuard {

Logger::Logger(const std::string& log_file_path) {
    log_file_.open(log_file_path, std::ios::app);
    if (!log_file_.is_open()) {
        std::cerr << "Failed to open log file: " << log_file_path << std::endl;
    }
}

Logger::~Logger() {
    if (log_file_.is_open()) {
        log_file_.close();
    }
}

std::string Logger::getCurrentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

std::string Logger::getLevelString(LogLevel level) const {
    switch (level) {
        case LogLevel::INFO:    return "[INFO]   ";
        case LogLevel::WARNING: return "[WARN]   ";
        case LogLevel::ERROR:   return "[ERROR]  ";
        case LogLevel::SUCCESS: return "[SUCCESS]";
        default:                return "[UNKNOWN]";
    }
}

std::string Logger::getColorCode(LogLevel level) const {
    switch (level) {
        case LogLevel::INFO:    return "\033[36m"; // Cyan
        case LogLevel::WARNING: return "\033[33m"; // Yellow
        case LogLevel::ERROR:   return "\033[31m"; // Red
        case LogLevel::SUCCESS: return "\033[32m"; // Green
        default:                return "\033[0m";  // Reset
    }
}

std::string Logger::getResetCode() const {
    return "\033[0m";
}

void Logger::log(LogLevel level, const std::string& message) {
    std::string timestamp = "[" + getCurrentTimestamp() + "]";
    std::string level_str = getLevelString(level);

    // File output (no color)
    if (log_file_.is_open()) {
        log_file_ << timestamp << " " << level_str << " " << message << std::endl;
    }

    // Console output (with color)
    std::cout << getColorCode(level) << timestamp << " " << level_str << " " << message << getResetCode() << std::endl;
}

void Logger::streamLog(const std::string& raw_output) {
    // For raw output from popen, we just pass it through, maybe log to file too
    if (log_file_.is_open()) {
        log_file_ << raw_output << std::flush;
    }
    std::cout << raw_output << std::flush;
}

} // namespace DeployGuard
