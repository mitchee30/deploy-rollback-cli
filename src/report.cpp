#include "deployguard/report.h"

#include "posix_util.h"

#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace DeployGuard {

namespace {

// JSON string literal, including the quotes. Control characters are escaped;
// other bytes are copied as-is (input is expected to be UTF-8).
std::string quote(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

// "" becomes null, so consumers can test for presence.
std::string quoteOrNull(const std::string& text) {
    return text.empty() ? std::string("null") : quote(text);
}

} // namespace

std::string utcTimestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::ostringstream os;
    os << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

std::string toJson(const RunSummary& summary) {
    std::ostringstream os;
    os << "{\n"
       << "  \"version\": " << quote(DEPLOYGUARD_VERSION) << ",\n"
       << "  \"outcome\": " << quote(summary.outcome) << ",\n"
       << "  \"exit_code\": " << summary.exit_code << ",\n"
       << "  \"target\": " << quoteOrNull(summary.target) << ",\n"
       << "  \"run_id\": " << quoteOrNull(summary.run_id) << ",\n"
       << "  \"recovered_run_id\": " << quoteOrNull(summary.recovered_run_id) << ",\n"
       << "  \"reason\": " << quoteOrNull(summary.reason) << ",\n"
       << "  \"started_at\": " << quoteOrNull(summary.started_at) << ",\n"
       << "  \"duration_ms\": " << summary.duration_ms << ",\n"
       << "  \"steps\": [";
    for (size_t i = 0; i < summary.steps.size(); ++i) {
        const StepRecord& step = summary.steps[i];
        os << (i == 0 ? "\n" : ",\n")
           << "    {\"phase\": " << quote(step.phase) << ", \"attempt\": " << step.attempt
           << ", \"result\": " << quote(step.result) << ", \"exit_code\": " << step.exit_code
           << ", \"duration_ms\": " << step.duration_ms << "}";
    }
    os << (summary.steps.empty() ? "]\n" : "\n  ]\n") << "}\n";
    return os.str();
}

bool writeReport(const std::string& path, const RunSummary& summary, std::string* error) {
    const std::string tmp = path + ".tmp";
    {
        detail::UniqueFd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
        if (!fd) {
            if (error != nullptr) {
                *error = detail::systemError("cannot create " + tmp);
            }
            return false;
        }
        if (!detail::writeAll(fd.get(), toJson(summary))) {
            if (error != nullptr) {
                *error = detail::systemError("cannot write " + tmp);
            }
            ::unlink(tmp.c_str());
            return false;
        }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        if (error != nullptr) {
            *error = detail::systemError("cannot rename " + tmp + " to " + path);
        }
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace DeployGuard
