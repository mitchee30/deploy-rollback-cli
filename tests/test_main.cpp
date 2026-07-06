#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../include/deploy-cli.h"
#include <cstdio>
#include <fstream>
#include <memory>

using namespace DeployGuard;

namespace {

// argv entries must be non-const char* per the C main() convention that
// ArgumentParser::parseArgs mirrors; the string literals themselves are
// never mutated.
char* toArg(const char* s) {
    return const_cast<char*>(s);
}

} // namespace

TEST_CASE("ArgumentParser: --config sets config_file and short-circuits to true") {
    char* argv[] = {toArg("deploy-cli"), toArg("--config"), toArg("deploy.yaml")};
    std::string config_file, fallback_path;
    bool has_config = ArgumentParser::parseArgs(3, argv, config_file, fallback_path);

    CHECK(has_config == true);
    CHECK(config_file == "deploy.yaml");
    CHECK(fallback_path.empty());
}

TEST_CASE("ArgumentParser: --path alone sets fallback_path and returns false") {
    char* argv[] = {toArg("deploy-cli"), toArg("--path"), toArg("/tmp/target")};
    std::string config_file, fallback_path;
    bool has_config = ArgumentParser::parseArgs(3, argv, config_file, fallback_path);

    CHECK(has_config == false);
    CHECK(fallback_path == "/tmp/target");
}

TEST_CASE("ArgumentParser: --path correctly skips its value so a later flag still parses") {
    // Regression test: parseArgs used to forget to advance past --path's value,
    // relying on the value never colliding with a real flag token.
    char* argv[] = {
        toArg("deploy-cli"), toArg("--path"), toArg("/tmp/target"),
        toArg("--config"), toArg("deploy.yaml")
    };
    std::string config_file, fallback_path;
    bool has_config = ArgumentParser::parseArgs(5, argv, config_file, fallback_path);

    CHECK(fallback_path == "/tmp/target");
    CHECK(config_file == "deploy.yaml");
    CHECK(has_config == true);
}

TEST_CASE("ConfigParser: parses a well-formed deploy.yaml") {
    const std::string path = "test_valid_deploy.yaml";
    {
        std::ofstream ofs(path);
        ofs << "deployment:\n"
               "  target_path: \"./some_target\"\n"
               "  scripts:\n"
               "    deploy: \"./scripts/deploy.sh\"\n"
               "    rollback: \"./scripts/rollback.sh\"\n";
    }
    auto logger = std::make_shared<Logger>("test_config_parser.log");

    auto config = ConfigParser::parseYaml(path, logger);

    REQUIRE(config.has_value());
    CHECK(config->target_path == "./some_target");
    CHECK(config->deploy_script == "./scripts/deploy.sh");
    CHECK(config->rollback_script == "./scripts/rollback.sh");

    std::remove(path.c_str());
    std::remove("test_config_parser.log");
}

TEST_CASE("ConfigParser: missing target_path returns nullopt") {
    const std::string path = "test_invalid_deploy.yaml";
    {
        std::ofstream ofs(path);
        ofs << "deployment:\n"
               "  scripts:\n"
               "    deploy: \"./scripts/deploy.sh\"\n"
               "    rollback: \"./scripts/rollback.sh\"\n";
    }
    auto logger = std::make_shared<Logger>("test_config_parser.log");

    auto config = ConfigParser::parseYaml(path, logger);

    CHECK_FALSE(config.has_value());

    std::remove(path.c_str());
    std::remove("test_config_parser.log");
}

TEST_CASE("ConfigParser: missing scripts node returns nullopt") {
    const std::string path = "test_invalid_scripts.yaml";
    {
        std::ofstream ofs(path);
        ofs << "deployment:\n"
               "  target_path: \"./some_target\"\n";
    }
    auto logger = std::make_shared<Logger>("test_config_parser.log");

    auto config = ConfigParser::parseYaml(path, logger);

    CHECK_FALSE(config.has_value());

    std::remove(path.c_str());
    std::remove("test_config_parser.log");
}

TEST_CASE("ConfigParser: missing file returns nullopt instead of throwing") {
    auto logger = std::make_shared<Logger>("test_config_parser.log");

    auto config = ConfigParser::parseYaml("this_file_does_not_exist.yaml", logger);

    CHECK_FALSE(config.has_value());

    std::remove("test_config_parser.log");
}
