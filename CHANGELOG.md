# Changelog

## 0.3.0

### Demo hooks
- Backups are committed atomically (copy to `.tmp`, then `mv`), so a crash
  while backing up can no longer leave a partial backup for rollback to restore.
- Hidden files are backed up and restored (`cp -r dir/*` skipped them, and a
  rollback left the new version's dotfiles behind).
- Deploying into an empty directory works (the `*` glob used to fail).
- Each target's app is tracked by a pid file instead of `pkill -f "app.sh --daemon"`,
  which stopped every matching process on the machine.
- The demo app runs fully detached (`nohup`, `</dev/null`, output to a log
  file) instead of inheriting deploy-cli's pipes, where its first write after
  deploy-cli exited would have killed it with SIGPIPE.
- New `health_check.sh`; the health check moved out of `deploy.sh` so that
  deploy-cli can retry it. Release v2.0 now starts but fails its health check.
- New release v1.1 and `scripts/demo.sh` (v1.0 -> v1.1 -> v2.0 rolled back).
- `test_app_target/` is created by `setup_v1.sh` and no longer committed.

### deploy-cli
- Deletes a stale backup directory before writing `INSTALLING`, so a backup
  seen during crash recovery always belongs to the interrupted deployment.
- `--path` mode also uses the bundled `health_check.sh`.

### Tests and CI
- Six integration suites driving the real binary: exit-code contract,
  SIGKILL mid-deployment and mid-rollback, concurrent runs, SIGINT/SIGTERM/
  nohup, timeouts, and the demo end to end.
- CI: GCC and Clang on Linux, Clang on macOS, ASan + UBSan, shellcheck, demo.
- `docs/DESIGN.md`.

## 0.2.0

- Fixed CI: yaml-cpp 0.8.0 no longer compiles on new GCC; now 0.9.0.
- Per-target `flock()`: a second run is refused (exit 3) instead of mistaking
  the first run's state for a crash and rolling back underneath it.
- Per-hook timeout (default 600 s); hooks run in their own process group and
  are stopped with SIGTERM, then SIGKILL.
- SIGINT/SIGTERM/SIGHUP cancel the deployment and roll back; the rollback is
  not interruptible.
- Pre-flight checks (target, hooks executable) before anything changes;
  new exit code 3 for "nothing attempted".
- Atomic, fsync'd state file, kept next to the target; corrupt state -> exit 2.
- Optional health check with retries; `--dry-run`, `--timeout`, `--log-file`,
  `--help`, `--version`.
- The hooks' stderr is written to the log file, every line timestamped.
- Hooks get stdin `/dev/null`, no inherited descriptors, default SIGPIPE, and
  `DEPLOYGUARD_*` environment variables.
- Code split into one header per component and a core library; 107 unit tests.

## 0.1.0

- Initial version: runs `deploy.sh`, runs `rollback.sh` if it fails, records
  `INSTALLING`/`ROLLING_BACK` for crash recovery, YAML configuration,
  `fork`/`execvp` with streamed output, exit codes 0/1/2.
