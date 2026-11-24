#include "deployguard/argument_parser.h"

#include <charconv>
#include <string_view>

namespace DeployGuard {

namespace {

bool parseNonNegativeInt(std::string_view text, int& out) {
    int value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (text.empty() || ec != std::errc() || ptr != end || value < 0) {
        return false;
    }
    out = value;
    return true;
}

} // namespace

CliParseResult ArgumentParser::parse(int argc, const char* const argv[]) {
    CliParseResult result;
    CliOptions& opts = result.options;

    auto fail = [&result](std::string message) {
        result.error = std::move(message);
        return result;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);

        // Split "--name=value"; short options only take a separate value.
        std::string_view name = arg;
        std::optional<std::string_view> inline_value;
        if (arg.size() > 2 && arg.substr(0, 2) == "--") {
            const auto eq = arg.find('=');
            if (eq != std::string_view::npos) {
                name = arg.substr(0, eq);
                inline_value = arg.substr(eq + 1);
            }
        }
        const std::string name_str(name);

        // Returns the option's value: either the "=value" part or the next
        // argv element (taken literally, even if it starts with "-").
        auto takeValue = [&]() -> std::optional<std::string> {
            if (inline_value) {
                return std::string(*inline_value);
            }
            if (i + 1 >= argc) {
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        const bool is_flag = name == "-h" || name == "--help" || name == "-V" || name == "--version" ||
                             name == "-n" || name == "--dry-run";
        if (is_flag && inline_value) {
            return fail("option '" + name_str + "' does not take a value");
        }

        if (name == "-h" || name == "--help") {
            opts.show_help = true;
        } else if (name == "-V" || name == "--version") {
            opts.show_version = true;
        } else if (name == "-n" || name == "--dry-run") {
            opts.dry_run = true;
        } else if (name == "-c" || name == "--config") {
            const auto value = takeValue();
            if (!value || value->empty()) {
                return fail("option '" + name_str + "' requires a file name");
            }
            opts.config_file = *value;
        } else if (name == "-p" || name == "--path") {
            const auto value = takeValue();
            if (!value || value->empty()) {
                return fail("option '" + name_str + "' requires a directory");
            }
            opts.target_path = *value;
        } else if (name == "-t" || name == "--timeout") {
            const auto value = takeValue();
            if (!value) {
                return fail("option '" + name_str + "' requires a number of seconds");
            }
            int seconds = 0;
            if (!parseNonNegativeInt(*value, seconds)) {
                return fail("invalid value '" + *value + "' for '" + name_str +
                            "' (expected a whole number of seconds, 0 = no timeout)");
            }
            opts.timeout_seconds = seconds;
        } else if (name == "--log-file") {
            const auto value = takeValue();
            if (!value) {
                return fail("option '--log-file' requires a file name (use \"\" to disable the log file)");
            }
            opts.log_file = *value;
        } else if (name == "--report") {
            const auto value = takeValue();
            if (!value || value->empty()) {
                return fail("option '--report' requires a file name");
            }
            opts.report_file = *value;
        } else if (!arg.empty() && arg[0] == '-') {
            return fail("unknown option '" + std::string(arg) + "'");
        } else {
            return fail("unexpected argument '" + std::string(arg) + "'");
        }
    }

    if (opts.show_help || opts.show_version) {
        return result;
    }
    if (!opts.config_file.empty() && !opts.target_path.empty()) {
        return fail("use either --config or --path, not both");
    }
    if (opts.config_file.empty() && opts.target_path.empty()) {
        return fail("missing --config <file> or --path <dir>");
    }
    return result;
}

std::string ArgumentParser::usage(const std::string& program_name) {
    return "Usage: " + program_name + " --config <deploy.yaml> [options]\n"
           "       " + program_name + " --path <target_dir> [options]\n"
           "\n"
           "Runs a deployment hook against a target directory and automatically runs the\n"
           "rollback hook if the deployment fails, crashes, times out, is cancelled, or\n"
           "fails its health check. Interrupted runs (kill -9, power loss) are detected\n"
           "and rolled back on the next start.\n"
           "\n"
           "Options:\n"
           "  -c, --config <file>      YAML configuration (see deploy.yaml)\n"
           "  -p, --path <dir>         Target directory, using the bundled hooks\n"
           "                           ./scripts/{deploy,rollback,health_check}.sh\n"
           "  -t, --timeout <seconds>  Time limit for each deploy/rollback hook run;\n"
           "                           overrides the config file (default 600, 0 = none)\n"
           "      --log-file <file>    Append a timestamped log here (default\n"
           "                           deploy_guard.log; \"\" disables it)\n"
           "      --report <file>      Write a JSON summary of the run (outcome, reason,\n"
           "                           every hook run with its duration) for CI\n"
           "  -n, --dry-run            Validate everything and print the plan, but run no\n"
           "                           hook and change nothing\n"
           "  -h, --help               Show this help\n"
           "  -V, --version            Show the version\n"
           "\n"
           "Exit codes:\n"
           "  0  deployment succeeded\n"
           "  1  deployment failed and was rolled back; the target is stable\n"
           "  2  rollback failed; the target needs manual attention\n"
           "  3  deployment not attempted: invalid arguments or configuration, failed\n"
           "     pre-flight check, or another deploy-cli is running on this target\n";
}

} // namespace DeployGuard
