#include "../include/deploy-cli.h"
#include <iostream>
#include <string_view>

namespace DeployGuard {

bool ArgumentParser::parseArgs(int argc, char* argv[], std::string& config_file, std::string& fallback_path) {
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--config") {
            if (i + 1 < argc) {
                config_file = argv[i + 1];
                return true;
            } else {
                std::cerr << "Error: --config option requires a value." << std::endl;
                exit(1);
            }
        } else if (arg == "--path") {
            if (i + 1 < argc) {
                fallback_path = argv[i + 1];
                ++i; // consume the value token so it isn't re-parsed as a flag
            } else {
                std::cerr << "Error: --path option requires a value." << std::endl;
                exit(1);
            }
        }
    }
    return !config_file.empty();
}

} // namespace DeployGuard
