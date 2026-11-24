#include <doctest/doctest.h>

#include "deployguard/argument_parser.h"

#include <initializer_list>
#include <vector>

using namespace DeployGuard;

namespace {

// parse({"--config", "x"}) == parse of `deploy-cli --config x`
CliParseResult parse(std::initializer_list<const char*> args) {
    std::vector<const char*> argv{"deploy-cli"};
    argv.insert(argv.end(), args.begin(), args.end());
    return ArgumentParser::parse(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST_CASE("ArgumentParser: --config sets the config file") {
    const auto r = parse({"--config", "deploy.yaml"});
    REQUIRE(r.ok());
    CHECK(r.options.config_file == std::string("deploy.yaml"));
    CHECK(r.options.target_path.empty());
}

TEST_CASE("ArgumentParser: --path sets the target directory") {
    const auto r = parse({"--path", "/srv/app"});
    REQUIRE(r.ok());
    CHECK(r.options.target_path == std::string("/srv/app"));
    CHECK(r.options.config_file.empty());
}

TEST_CASE("ArgumentParser: --name=value and short options") {
    const auto a = parse({"--config=deploy.yaml", "--timeout=30", "--log-file=run.log"});
    REQUIRE(a.ok());
    CHECK(a.options.config_file == std::string("deploy.yaml"));
    CHECK(a.options.timeout_seconds == 30);
    CHECK(a.options.log_file == std::string("run.log"));

    const auto b = parse({"-p", "/srv/app", "-t", "5", "-n"});
    REQUIRE(b.ok());
    CHECK(b.options.target_path == std::string("/srv/app"));
    CHECK(b.options.timeout_seconds == 5);
    CHECK(b.options.dry_run);
}

TEST_CASE("ArgumentParser: defaults") {
    const auto r = parse({"--path", "x"});
    REQUIRE(r.ok());
    CHECK_FALSE(r.options.timeout_seconds.has_value());
    CHECK(r.options.log_file == std::string("deploy_guard.log"));
    CHECK_FALSE(r.options.dry_run);
    CHECK_FALSE(r.options.show_help);
    CHECK_FALSE(r.options.show_version);
}

TEST_CASE("ArgumentParser: an empty --log-file disables the log file") {
    const auto r = parse({"--path", "x", "--log-file", ""});
    REQUIRE(r.ok());
    CHECK(r.options.log_file.empty());
}

TEST_CASE("ArgumentParser: --timeout accepts 0 and rejects garbage") {
    CHECK(parse({"--path", "x", "--timeout", "0"}).options.timeout_seconds == 0);
    for (const char* bad : {"-1", "abc", "10s", "1.5", "", "99999999999999"}) {
        CAPTURE(bad);
        const auto r = parse({"--path", "x", "--timeout", bad});
        CHECK_FALSE(r.ok());
        CHECK(r.error.find("--timeout") != std::string::npos);
    }
}

TEST_CASE("ArgumentParser: --help and --version need no other options") {
    CHECK(parse({"--help"}).ok());
    CHECK(parse({"--help"}).options.show_help);
    CHECK(parse({"-h"}).options.show_help);
    CHECK(parse({"--version"}).options.show_version);
    CHECK(parse({"-V"}).options.show_version);
}

TEST_CASE("ArgumentParser: unknown options are errors, not silently ignored") {
    // Before v0.2 a typo such as --confg was skipped and the user got a
    // misleading "you must provide --config" message.
    const auto r = parse({"--confg", "deploy.yaml"});
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("unknown option '--confg'") != std::string::npos);
}

TEST_CASE("ArgumentParser: stray positional arguments are errors") {
    const auto r = parse({"--path", "x", "extra"});
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("unexpected argument 'extra'") != std::string::npos);
}

TEST_CASE("ArgumentParser: --config and --path together are rejected in either order") {
    const auto a = parse({"--path", "/srv/app", "--config", "deploy.yaml"});
    const auto b = parse({"--config", "deploy.yaml", "--path", "/srv/app"});
    CHECK_FALSE(a.ok());
    CHECK_FALSE(b.ok());
    CHECK(a.error == b.error);
}

TEST_CASE("ArgumentParser: neither --config nor --path is an error") {
    const auto r = parse({"--dry-run"});
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("missing") != std::string::npos);
}

TEST_CASE("ArgumentParser: options missing their value are errors") {
    CHECK_FALSE(parse({"--config"}).ok());
    CHECK_FALSE(parse({"--path"}).ok());
    CHECK_FALSE(parse({"--path", "x", "--timeout"}).ok());
    CHECK_FALSE(parse({"--config="}).ok());
}

TEST_CASE("ArgumentParser: flags reject an =value") {
    const auto r = parse({"--path", "x", "--dry-run=yes"});
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("does not take a value") != std::string::npos);
}

TEST_CASE("ArgumentParser: a value is taken literally even if it looks like an option") {
    // Regression: the parser used to forget to skip --path's value, so a
    // value spelled like a flag was parsed a second time as that flag.
    const auto r = parse({"--path", "--config"});
    REQUIRE(r.ok());
    CHECK(r.options.target_path == std::string("--config"));
    CHECK(r.options.config_file.empty());
}

TEST_CASE("ArgumentParser: usage text documents every exit code") {
    const std::string usage = ArgumentParser::usage();
    for (const char* code : {"0  ", "1  ", "2  ", "3  "}) {
        CAPTURE(code);
        CHECK(usage.find(code) != std::string::npos);
    }
    CHECK(usage.find("--dry-run") != std::string::npos);
}

TEST_CASE("ArgumentParser: --log-file without a value is an error") {
    const auto r = parse({"--path", "x", "--log-file"});
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("--log-file") != std::string::npos);
}


TEST_CASE("ArgumentParser: --report") {
    const auto r = parse({"--path", "x", "--report", "result.json"});
    REQUIRE(r.ok());
    CHECK(r.options.report_file == std::string("result.json"));
    CHECK(parse({"--path", "x"}).options.report_file.empty());
    CHECK_FALSE(parse({"--path", "x", "--report"}).ok());
    CHECK_FALSE(parse({"--path", "x", "--report="}).ok());
}
