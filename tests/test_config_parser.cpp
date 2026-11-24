#include <doctest/doctest.h>

#include "deployguard/config.h"
#include "test_helpers.h"

using namespace DeployGuard;
using namespace testutil;

namespace {

struct ConfigFixture {
    TempDir dir;
    std::shared_ptr<Logger> logger = makeLogger(dir);

    std::optional<Config> parse(const std::string& yaml) {
        const std::string path = dir / "deploy.yaml";
        writeFile(path, yaml);
        return ConfigParser::parseYaml(path, logger);
    }
    std::string log() const { return readFile(dir / "test.log"); }
};

const char* kMinimal =
    "deployment:\n"
    "  target_path: ./app\n"
    "  scripts:\n"
    "    deploy: ./deploy.sh\n"
    "    rollback: ./rollback.sh\n";

} // namespace

TEST_CASE("ConfigParser: minimal config uses documented defaults") {
    ConfigFixture f;
    const auto config = f.parse(kMinimal);
    REQUIRE(config.has_value());
    CHECK(config->target_path == std::string("./app"));
    CHECK(config->deploy_script == std::string("./deploy.sh"));
    CHECK(config->rollback_script == std::string("./rollback.sh"));
    CHECK(config->health_check_script.empty());
    CHECK(config->timeout_seconds == 600);
    CHECK(config->health_check_attempts == 3);
    CHECK(config->health_check_interval_seconds == 2);
}

TEST_CASE("ConfigParser: all optional keys") {
    ConfigFixture f;
    const auto config = f.parse(
        "deployment:\n"
        "  target_path: \"/srv/my app\"\n"
        "  scripts:\n"
        "    deploy: ./deploy.sh\n"
        "    rollback: ./rollback.sh\n"
        "    health_check: ./health.sh\n"
        "  timeout_seconds: 0\n"
        "  health_check_attempts: 5\n"
        "  health_check_interval_seconds: 1\n");
    REQUIRE(config.has_value());
    CHECK(config->target_path == std::string("/srv/my app"));
    CHECK(config->health_check_script == std::string("./health.sh"));
    CHECK(config->timeout_seconds == 0);
    CHECK(config->health_check_attempts == 5);
    CHECK(config->health_check_interval_seconds == 1);
}

TEST_CASE("ConfigParser: required keys") {
    ConfigFixture f;
    SUBCASE("missing target_path") {
        CHECK_FALSE(f.parse("deployment:\n  scripts:\n    deploy: a\n    rollback: b\n"));
        CHECK(contains(f.log(), "deployment.target_path"));
    }
    SUBCASE("missing scripts section") {
        CHECK_FALSE(f.parse("deployment:\n  target_path: ./app\n"));
        CHECK(contains(f.log(), "deployment.scripts"));
    }
    SUBCASE("missing rollback script") {
        CHECK_FALSE(f.parse("deployment:\n  target_path: ./app\n  scripts:\n    deploy: a\n"));
        CHECK(contains(f.log(), "deployment.scripts.rollback"));
    }
    SUBCASE("empty deploy script") {
        CHECK_FALSE(f.parse("deployment:\n  target_path: ./app\n  scripts:\n    deploy: \"\"\n    rollback: b\n"));
        CHECK(contains(f.log(), "must not be empty"));
    }
    SUBCASE("deploy script given as a list") {
        CHECK_FALSE(f.parse("deployment:\n  target_path: ./app\n  scripts:\n    deploy: [a, b]\n    rollback: b\n"));
        CHECK(contains(f.log(), "must be a string"));
    }
    SUBCASE("no deployment section") {
        CHECK_FALSE(f.parse("something_else: 1\n"));
        CHECK(contains(f.log(), "'deployment'"));
    }
    SUBCASE("document is a plain string") {
        CHECK_FALSE(f.parse("hello\n"));
    }
}

TEST_CASE("ConfigParser: invalid numbers are reported with the key name") {
    ConfigFixture f;
    const std::string base = std::string(kMinimal);
    SUBCASE("not a number") {
        CHECK_FALSE(f.parse(base + "  timeout_seconds: soon\n"));
        CHECK(contains(f.log(), "timeout_seconds"));
        CHECK(contains(f.log(), "soon"));
    }
    SUBCASE("negative timeout") {
        CHECK_FALSE(f.parse(base + "  timeout_seconds: -5\n"));
        CHECK(contains(f.log(), "timeout_seconds"));
    }
    SUBCASE("zero health check attempts") {
        CHECK_FALSE(f.parse(base + "  health_check_attempts: 0\n"));
        CHECK(contains(f.log(), "health_check_attempts"));
    }
}

TEST_CASE("ConfigParser: unknown keys are accepted with a warning") {
    ConfigFixture f;
    const auto config = f.parse(std::string(kMinimal) + "  timeout_secs: 5\n");
    REQUIRE(config.has_value());
    CHECK(config->timeout_seconds == 600);  // the typo did not take effect...
    CHECK(contains(f.log(), "unknown configuration key 'deployment.timeout_secs'"));  // ...and we said so
}

TEST_CASE("ConfigParser: missing file and malformed YAML return nullopt instead of throwing") {
    ConfigFixture f;
    CHECK_FALSE(ConfigParser::parseYaml(f.dir / "does_not_exist.yaml", f.logger));
    CHECK(contains(f.log(), "Cannot read configuration file"));

    CHECK_FALSE(f.parse("deployment: [unclosed\n"));
    CHECK(contains(f.log(), "Cannot parse"));
}

TEST_CASE("ConfigParser::validate") {
    Config c;
    c.target_path = "./app";
    c.deploy_script = "./d.sh";
    c.rollback_script = "./r.sh";
    CHECK(ConfigParser::validate(c).empty());

    Config bad = c;
    bad.target_path.clear();
    CHECK_FALSE(ConfigParser::validate(bad).empty());

    bad = c;
    bad.rollback_script.clear();
    CHECK_FALSE(ConfigParser::validate(bad).empty());

    bad = c;
    bad.timeout_seconds = -1;
    CHECK_FALSE(ConfigParser::validate(bad).empty());

    bad = c;
    bad.health_check_interval_seconds = -1;
    CHECK_FALSE(ConfigParser::validate(bad).empty());
}

TEST_CASE("ConfigParser: structural type errors") {
    ConfigFixture f;
    SUBCASE("deployment is a list") {
        CHECK_FALSE(f.parse("deployment:\n  - a\n  - b\n"));
        CHECK(contains(f.log(), "'deployment' must be a mapping"));
    }
    SUBCASE("a number given as a mapping") {
        CHECK_FALSE(f.parse(std::string(kMinimal) + "  timeout_seconds:\n    value: 5\n"));
        CHECK(contains(f.log(), "must be a whole number"));
    }
}

TEST_CASE("ConfigParser::validate requires a deploy script") {
    Config c;
    c.target_path = "./app";
    c.rollback_script = "./r.sh";
    CHECK(contains(ConfigParser::validate(c), "deploy script"));
}


TEST_CASE("ConfigParser: timeout, retry and grace keys (v0.4)") {
    ConfigFixture f;
    const auto defaults = f.parse(kMinimal);
    REQUIRE(defaults.has_value());
    CHECK(defaults->health_check_timeout_seconds == 30);
    CHECK(defaults->rollback_attempts == 3);
    CHECK(defaults->rollback_retry_interval_seconds == 5);
    CHECK(defaults->kill_grace_seconds == 5);

    const auto config = f.parse(std::string(kMinimal) +
                                "  health_check_timeout_seconds: 7\n"
                                "  rollback_attempts: 4\n"
                                "  rollback_retry_interval_seconds: 0\n"
                                "  kill_grace_seconds: 1\n");
    REQUIRE(config.has_value());
    CHECK(config->health_check_timeout_seconds == 7);
    CHECK(config->rollback_attempts == 4);
    CHECK(config->rollback_retry_interval_seconds == 0);
    CHECK(config->kill_grace_seconds == 1);
}

TEST_CASE("ConfigParser: out-of-range values name the key and the allowed range") {
    ConfigFixture f;
    CHECK_FALSE(f.parse(std::string(kMinimal) + "  rollback_attempts: 0\n"));
    CHECK(contains(f.log(), "rollback_attempts must be between 1 and 20 (got 0)"));
    CHECK_FALSE(f.parse(std::string(kMinimal) + "  kill_grace_seconds: -1\n"));
    CHECK(contains(f.log(), "kill_grace_seconds"));
}

TEST_CASE("ConfigParser: parseYaml reports the reason to the caller") {
    ConfigFixture f;
    std::string error;
    CHECK_FALSE(ConfigParser::parseYaml(f.dir / "missing.yaml", f.logger, &error));
    CHECK(contains(error, "Cannot read configuration file"));
}
