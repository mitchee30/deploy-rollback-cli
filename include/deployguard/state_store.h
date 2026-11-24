#ifndef DEPLOYGUARD_STATE_STORE_H
#define DEPLOYGUARD_STATE_STORE_H

#include <map>
#include <string>

namespace DeployGuard {

enum class DeploymentState {
    STABLE,       // no deployment in progress (state file absent)
    INSTALLING,   // deploy hook (or health check) is running
    ROLLING_BACK  // rollback hook is running
};

const char* stateName(DeploymentState state);

// ------------------------------------------------------------------------
// Files deploy-cli keeps for a target directory.
//
// They live *next to* the target (as hidden siblings), not inside it:
//
//   /srv/myapp                         <- target_path
//   /srv/.deploy_guard_myapp.state     <- crash-recovery marker
//   /srv/.deploy_guard_myapp.lock      <- one-deploy-at-a-time lock
//   /srv/.deploy_guard_myapp.backup/   <- backup location offered to the hooks
//
// Keeping them outside the target means a deploy hook is free to wipe or
// replace the whole target directory without destroying the marker that crash
// recovery depends on, and backups of the target never contain our own files.
// ------------------------------------------------------------------------
struct TargetPaths {
    std::string target;      // absolute, normalised, no trailing slash
    std::string state_file;
    std::string lock_file;
    std::string backup_dir;
};

TargetPaths deriveTargetPaths(const std::string& target_path);

// key=value lines stored after the state, e.g. which run wrote the file and
// which rollback hook that run was using.
using StateFields = std::map<std::string, std::string>;

struct StateSnapshot {
    DeploymentState state = DeploymentState::STABLE;
    bool valid = true;    // false: the file exists but is unreadable or unrecognised
    std::string detail;   // every extra line, for log messages (or the error)
    StateFields fields;   // the key=value lines

    // The value of a field, or "" if the file does not have it (files
    // written by v0.3 and earlier only contain the state).
    std::string field(const std::string& key) const;
};

// ------------------------------------------------------------------------
// StateStore
//
// Persists the deployment state so that a crash (kill -9, power loss, OOM
// killer...) in the middle of a deployment is detected on the next run.
//
// Writes are atomic and durable:
//   1. write the new content to "<file>.tmp" and fsync() it,
//   2. rename() it over the old file (atomic on POSIX file systems),
//   3. fsync() the directory, so the rename itself survives a power cut.
// A reader therefore sees either the complete old state or the complete new
// state - never an empty or half-written file.
//
// Concurrent writers are prevented by the FileLock held around a deployment,
// not by this class.
// ------------------------------------------------------------------------
class StateStore {
public:
    explicit StateStore(std::string state_file_path);

    const std::string& path() const { return path_; }

    // Records `state` plus `fields` (and the writer's pid and a timestamp).
    // Keys must be lowercase identifiers and values must fit on one line.
    // Returns false and fills *error on failure.
    bool write(DeploymentState state, const StateFields& fields = {}, std::string* error = nullptr);
    StateSnapshot read() const;
    bool clear(std::string* error = nullptr);

private:
    std::string path_;
};

} // namespace DeployGuard

#endif // DEPLOYGUARD_STATE_STORE_H
