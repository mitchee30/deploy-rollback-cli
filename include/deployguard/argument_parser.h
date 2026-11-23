#ifndef DEPLOYGUARD_ARGUMENT_PARSER_H
#define DEPLOYGUARD_ARGUMENT_PARSER_H

#include <optional>
#include <string>

namespace DeployGuard {

struct CliOptions {
    std::string config_file;                // --config
    std::string target_path;                // --path
    std::optional<int> timeout_seconds;     // --timeout (overrides the config file)
    std::string log_file = "deploy_guard.log";  // --log-file ("" disables)
    bool dry_run = false;                   // --dry-run
    bool show_help = false;                 // --help
    bool show_version = false;              // --version
};

struct CliParseResult {
    CliOptions options;
    std::string error;  // empty on success
    bool ok() const { return error.empty(); }
};

// ------------------------------------------------------------------------
// ArgumentParser
//
// Pure function of argv: it never prints and never calls exit(), so every
// case is unit-testable and main() decides what to do with errors.
//
// Accepts "--opt value", "--opt=value" and short forms ("-c value").
// Unknown options, stray positional arguments, a missing value, or giving
// both --config and --path are reported as errors instead of being ignored.
// ------------------------------------------------------------------------
struct ArgumentParser {
    static CliParseResult parse(int argc, const char* const argv[]);
    static std::string usage(const std::string& program_name = "deploy-cli");
};

} // namespace DeployGuard

#endif // DEPLOYGUARD_ARGUMENT_PARSER_H
