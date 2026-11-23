# DeployGuard CLI (deploy-rollback-cli)

![CI](https://github.com/mitchee30/deploy-rollback-cli/actions/workflows/ci.yml/badge.svg)
![C++ Standard](https://img.shields.io/badge/C%2B%2B-17-blue)
![License](https://img.shields.io/badge/License-MIT-yellow)

A fault-tolerant CLI built in C++17 that runs deployment scripts and automatically rolls back on failure.

Acts as a deployment state machine for CI/CD pipelines: if a deployment script exits non-zero, the CLI triggers a rollback script to restore the previous state.

## ✨ Key Features

* **Runs deployment hooks** (`deploy.sh`, `rollback.sh`, optional `health_check.sh`) with `fork`/`execvp`, streaming their output live and into a log file.
* **Automatic rollback** when the deploy hook exits non-zero, crashes, times out, is cancelled (Ctrl+C / SIGTERM), or the health check keeps failing.
* **Crash recovery:** a run killed mid-deployment (`kill -9`, power loss) is detected and rolled back on the next start.
* **Exit codes for CI** that separate "rolled back, safe" from "rollback failed, needs a human" from "never started".

## 📁 Repository Structure

```text
.
├── CMakeLists.txt              # build: core library + CLI + tests
├── .github/workflows/ci.yml    # CI: Linux + macOS, warnings as errors
├── include/deployguard/        # one header per component
│   ├── argument_parser.h       # CLI options (no exit() inside, fully testable)
│   ├── config.h                # deploy.yaml schema + validation
│   ├── deploy_controller.h     # the deployment state machine
│   ├── file_lock.h             # flock(): one deploy per target at a time
│   ├── logger.h                # console + timestamped log file
│   ├── process_manager.h       # fork/execvp, timeouts, process groups
│   ├── signals.h               # Ctrl+C / SIGTERM -> graceful rollback
│   └── state_store.h           # atomic, fsync'd crash-recovery marker
├── src/                        # implementations (+ main.cpp)
├── scripts/                    # demo hooks: deploy.sh, rollback.sh, setup_v1.sh
└── tests/                      # doctest unit tests (one CTest entry per case)
```

## 🚀 Getting Started

### Prerequisites

* A C++17 compatible compiler (GCC, Clang, or MSVC)
* CMake (3.16 or higher)

### Build Instructions

You can quickly build the project using the provided shell script:

```bash
chmod +x scripts/build.sh
./scripts/build.sh
```

Alternatively, use standard CMake commands:

```bash
mkdir build && cd build
cmake ..
make
ctest -j8 --output-on-failure   # run the tests
```

### Usage

Run the CLI tool by specifying the target deployment path:

```bash
./deploy-cli --path /var/www/target_app
```

Expected Workflow (Simulated):
1. The CLI parses the arguments and invokes `scripts/deploy.sh`.
2. If `deploy.sh` encounters an error (e.g., exit 1), the CLI intercepts the error.
3. The CLI immediately triggers `scripts/rollback.sh` to restore the system state.
4. Logs are outputted to the standard output/error stream for pipeline monitoring.

### Options

```text
-c, --config <file>      YAML configuration (see deploy.yaml)
-p, --path <dir>         Target directory, using ./scripts/deploy.sh and ./scripts/rollback.sh
-t, --timeout <seconds>  Time limit per hook run (default 600, 0 = none); overrides the config
    --log-file <file>    Append a timestamped log (default deploy_guard.log, "" = off)
-n, --dry-run            Validate everything and print the plan without changing anything
-h, --help / -V, --version
```

Optional keys in `deploy.yaml`: `scripts.health_check`, `timeout_seconds`,
`health_check_attempts`, `health_check_interval_seconds`.

### Exit Codes

`deploy-cli` distinguishes four outcomes so CI/CD pipelines can react differently to each:

| Code | Meaning |
| ---- | ------- |
| `0`  | Deployment succeeded (and passed its health check, if configured). |
| `1`  | Deployment failed, timed out, was cancelled or failed its health check, and the automatic rollback succeeded — the target is back in its previous state. |
| `2`  | The rollback itself failed — the target may be inconsistent and needs manual attention. The state file is kept so the next run retries the rollback. |
| `3`  | Deployment not attempted: invalid arguments or configuration, a failed pre-flight check (missing target, missing/non-executable hook), or another `deploy-cli` is already running against the same target. |

### Safety mechanisms

* **Crash recovery:** before running the deploy hook, the state `INSTALLING` is written to `.deploy_guard_<name>.state` next to the target (write to temp file → `fsync` → `rename` → `fsync` the directory, so it is never half-written). If the next run finds it, the previous run died mid-deployment and is rolled back first.
* **One run per target:** an exclusive `flock()` on `.deploy_guard_<name>.lock`. The kernel releases it even if `deploy-cli` is `kill -9`ed, so a leftover state file can be told apart from a run that is still in progress.
* **Timeouts and Ctrl+C:** hooks run in their own process group; on timeout or SIGINT/SIGTERM the whole group gets SIGTERM, then SIGKILL after a grace period, and the rollback runs. The rollback itself is never interrupted.
* **Pre-flight checks:** the target and all hooks are checked *before* anything is modified — a missing rollback script is found up front, not when it is needed.
* **Hook isolation:** `fork`/`execvp` (no shell, so no injection), stdin is `/dev/null`, inherited file descriptors are closed, and context is passed as `$1` plus `DEPLOYGUARD_PHASE`, `DEPLOYGUARD_TARGET`, `DEPLOYGUARD_BACKUP_DIR`.

### Running Tests

107 doctest unit tests cover argument parsing, config validation, the logger,
the state file, the lock, process handling (timeouts, process-group kills,
SIGKILL escalation, exec failures, shell-injection payloads, fd inheritance)
and every path through the deployment state machine:

```bash
./scripts/build.sh
cd build && ctest -j8 --output-on-failure
```

## 🛠️ Future Roadmap

* [x] Replace `std::system` with POSIX `fork/execvp` for real-time stdout/stderr streaming.
* [x] Persistent, timestamped log file including the hooks' stdout and stderr.
* [x] Timeouts, cancellation, health checks, locking, atomic state file (v0.2).
* [ ] Rework the demo scripts (atomic backup, PID files instead of `pkill -f`) and add a health_check.sh.
* [ ] Integration tests that `kill -9` the real binary mid-deployment.
* [ ] Add JSON configuration parsing for complex deployment workflows.

## 📄 License

This project is licensed under the MIT License - see the LICENSE file for details.
