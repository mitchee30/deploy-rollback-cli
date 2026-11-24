#ifndef DEPLOYGUARD_REPORT_H
#define DEPLOYGUARD_REPORT_H

#include <string>
#include <vector>

namespace DeployGuard {

// One hook run.
struct StepRecord {
    std::string phase;      // "deploy", "health_check" or "rollback"
    int attempt = 1;
    std::string result;     // ExecResult::describe(), e.g. "exited with status 1"
    int exit_code = 0;
    long long duration_ms = 0;
};

// What happened during one deploy-cli run - kept by DeployController and
// written by --report as JSON, so a CI pipeline can read the outcome without
// parsing log text.
struct RunSummary {
    std::string outcome = "ABORTED";  // SUCCESS / ROLLED_BACK / CRITICAL_FAILURE / ABORTED
    int exit_code = 3;
    std::string target;               // absolute target path
    std::string run_id;               // id of the deployment this run started, if any
    std::string recovered_run_id;     // id of an interrupted run that was recovered, if any
    std::string reason;               // why it did not simply succeed; empty on success
    std::string started_at;           // UTC, ISO 8601
    long long duration_ms = 0;
    std::vector<StepRecord> steps;
};

// Serialises the summary as a JSON object (pretty-printed, stable key order).
std::string toJson(const RunSummary& summary);

// Writes toJson(summary) to `path` atomically (temporary file + rename), so a
// reader never sees half a report. Returns false and fills *error on failure.
bool writeReport(const std::string& path, const RunSummary& summary, std::string* error = nullptr);

// Current time as "2026-11-24T05:40:12Z".
std::string utcTimestamp();

} // namespace DeployGuard

#endif // DEPLOYGUARD_REPORT_H
