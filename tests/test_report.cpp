#include <doctest/doctest.h>

#include "deployguard/report.h"
#include "test_helpers.h"

using namespace DeployGuard;
using namespace testutil;

namespace {

RunSummary sampleSummary() {
    RunSummary s;
    s.outcome = "ROLLED_BACK";
    s.exit_code = 1;
    s.target = "/srv/app";
    s.run_id = "20261124T054012Z-42-1";
    s.reason = "the health check failed";
    s.started_at = "2026-11-24T05:40:12Z";
    s.duration_ms = 5321;
    s.steps.push_back(StepRecord{"deploy", 1, "exited with status 0", 0, 1203});
    s.steps.push_back(StepRecord{"rollback", 1, "exited with status 0", 0, 87});
    return s;
}

} // namespace

TEST_CASE("toJson: stable, complete output") {
    const std::string expected =
        "{\n"
        "  \"version\": \"" DEPLOYGUARD_VERSION "\",\n"
        "  \"outcome\": \"ROLLED_BACK\",\n"
        "  \"exit_code\": 1,\n"
        "  \"target\": \"/srv/app\",\n"
        "  \"run_id\": \"20261124T054012Z-42-1\",\n"
        "  \"recovered_run_id\": null,\n"
        "  \"reason\": \"the health check failed\",\n"
        "  \"started_at\": \"2026-11-24T05:40:12Z\",\n"
        "  \"duration_ms\": 5321,\n"
        "  \"steps\": [\n"
        "    {\"phase\": \"deploy\", \"attempt\": 1, \"result\": \"exited with status 0\", \"exit_code\": 0, "
        "\"duration_ms\": 1203},\n"
        "    {\"phase\": \"rollback\", \"attempt\": 1, \"result\": \"exited with status 0\", \"exit_code\": 0, "
        "\"duration_ms\": 87}\n"
        "  ]\n"
        "}\n";
    CHECK(toJson(sampleSummary()) == expected);
}

TEST_CASE("toJson: empty fields become null and no steps is an empty array") {
    RunSummary s;
    s.reason = "";
    const std::string json = toJson(s);
    CHECK(contains(json, "\"outcome\": \"ABORTED\""));
    CHECK(contains(json, "\"exit_code\": 3"));
    CHECK(contains(json, "\"reason\": null"));
    CHECK(contains(json, "\"steps\": []"));
}

TEST_CASE("toJson: strings are escaped") {
    RunSummary s;
    s.reason = "quote \" backslash \\ newline \n tab \t bell \a";
    CHECK(contains(toJson(s), "\"reason\": \"quote \\\" backslash \\\\ newline \\n tab \\t bell \\u0007\""));
}

TEST_CASE("writeReport: writes the file atomically, leaving no temporary file") {
    TempDir dir;
    const std::string path = dir / "result.json";
    std::string error;
    REQUIRE(writeReport(path, sampleSummary(), &error));
    CHECK(readFile(path) == toJson(sampleSummary()));
    CHECK_FALSE(exists(path + ".tmp"));
}

TEST_CASE("writeReport: an unwritable location is an error, not a crash") {
    TempDir dir;
    std::string error;
    CHECK_FALSE(writeReport(dir / "no/such/dir/result.json", sampleSummary(), &error));
    CHECK(contains(error, "cannot create"));
}

TEST_CASE("utcTimestamp: ISO 8601 in UTC") {
    const std::string ts = utcTimestamp();
    REQUIRE(ts.size() == 20);
    CHECK(ts[4] == '-');
    CHECK(ts[10] == 'T');
    CHECK(ts.back() == 'Z');
}
