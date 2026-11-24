# DeployGuard design notes

This document explains *why* `deploy-cli` is built the way it is. The README
says what it does; the code comments explain individual lines.

## 1. Goal and scope

Goal: whatever goes wrong during a deployment on one host, the target ends up
either on the new version (exit 0) or back on the previous one (exit 1) - and
if neither can be achieved, a human is told clearly (exit 2) and the next run
picks up where this one stopped.

Non-goals: copying files, talking to remote machines, orchestrating several
hosts. Those belong in the hook scripts or in a bigger system. Keeping
`deploy-cli` to *control flow* keeps it small enough to reason about
completely.

## 2. Components

| Component | Responsibility | Depends on |
| --- | --- | --- |
| `DeployController` | the state machine (lock, recover, pre-flight, deploy, verify, commit, rollback) | everything below |
| `ProcessManager` | run one hook: fork/exec, stream output, timeout, cancellation | `Logger` |
| `StateStore` | read/write the crash-recovery state file atomically and durably | - |
| `FileLock` | exclusive per-target lock | - |
| `ConfigParser` / `ArgumentParser` | turn YAML / argv into a validated `Config` | - |
| `Logger` | console and log file | - |
| `signals` | turn SIGINT/SIGTERM/SIGHUP into a flag | - |
| `report` | serialise the run summary as JSON for `--report` | - |

Each has its own header in `include/deployguard/` and is unit-tested in
isolation; `DeployController` receives its `Logger` and `ProcessManager` (and
a cancellation callback) through its constructor, so tests can run it against
small throw-away scripts.

## 3. The state machine

```
            +------------------- recover (previous run died) ------------------+
            |                                                                   v
 STABLE --(write INSTALLING)--> INSTALLING --(deploy + health ok)--> STABLE   ROLLING_BACK
                                    |                                           |   |
                                    +--(failure/timeout/cancel)--> ROLLING_BACK-+   |
                                                                     |  (ok)        | (fails: stays,
                                                                     v              |  next run retries)
                                                                   STABLE <---------+
```

Only two states are ever written to disk, `INSTALLING` and `ROLLING_BACK`.
`STABLE` is the absence of the file. The state is written **before** the
corresponding hook starts and removed only **after** it succeeded, so at
every instant the file errs on the side of "something may be half done".

### Recovery repeats the interrupted run's own rollback

The state file is not just a flag. Its content looks like this:

```
INSTALLING
rollback=/srv/hooks/rollback.sh
run_id=20261124T054012Z-4242-1
pid=4242
updated_at=2026-11-24T05:40:12Z
```

Recovery runs *that* rollback hook, on behalf of *that* run id - not the hook
named in today's `deploy.yaml`. Between a crash and the next run someone may
well edit the configuration (that is often how the crash gets "fixed"), and a
different rollback hook is not guaranteed to know how to undo what the dead
run did. If the two differ, `deploy-cli` warns; if the recorded hook no
longer exists, it refuses with exit 2 rather than guess. Paths are recorded
as absolute paths, so recovery also works when the next run is started from
another directory (which is also why hooks get the absolute target as `$1`).

State files written before v0.4 contain only the state; recovery then falls
back to the configured rollback hook.

### Retrying a failed rollback

Exit 2 means "page a human", so a failed rollback is retried
(`rollback_attempts`, default 3, `rollback_retry_interval_seconds` apart)
before giving up: rollbacks often fail for transient reasons - a port not
released yet, a network blip. This is safe only because rollback hooks must
be idempotent anyway (recovery may run them again after a crash). A hook that
cannot be *started* (missing, not executable) is not retried: that cannot fix
itself.

### Recovery is only safe because of the lock

Recovery interprets a leftover `INSTALLING` as "the previous run died". That
is only true if no other run is alive. Version 0.1 had no lock, and a second
`deploy-cli` started during a slow deployment rolled the target back
underneath the first one. Now every run first takes an exclusive `flock()` on
`.deploy_guard_<name>.lock`:

* `flock()` locks are released by the kernel when the process exits *for any
  reason*, including SIGKILL and the OOM killer. So "I hold the lock and the
  state says INSTALLING" really does mean "whoever wrote that is dead".
* Alternatives considered: a PID file (the PID may be reused by an unrelated
  process; a stale file needs heuristics to clean up) and an `O_EXCL` lock
  file (survives a crash, so a crashed run blocks all future runs until
  someone deletes it by hand). Both fail exactly in the crash case that
  matters most.
* The lock file is opened with `O_CLOEXEC` and never deleted. A lock belongs
  to an *open file description*; without `O_CLOEXEC` a daemon started by the
  deploy hook would inherit it and keep the target locked forever. Deleting
  the file would let two processes lock two different inodes of the same name.

## 4. Crash safety of the files

### The state file

Written with the standard atomic-replace sequence:

1. write `state.tmp`, `fsync()` it;
2. `rename()` it over `state` - atomic: a reader sees the old or the new file,
   never a mix;
3. `fsync()` the directory, because the rename is a change to the directory
   and is otherwise not guaranteed to survive a power cut.

On macOS, `fcntl(F_FULLFSYNC)` is used instead of `fsync()`, which there only
pushes data to the drive's volatile cache. An unreadable or unrecognised state
file is treated as an error (exit 2), never as `STABLE`: guessing "nothing
was in progress" would silently skip a needed rollback.

The file lives *next to* the target (`/srv/.deploy_guard_app.state` for
`/srv/app`), not inside it. A deploy hook may legitimately delete or replace
the whole target directory; with the marker inside, that would erase the very
evidence crash recovery depends on, and backups of the target would contain
it.

### The backup (owned by the hooks)

`deploy-cli` does not copy files, but the bundled hooks show the invariant a
rollback hook needs:

> The deploy hook does not modify the target until a complete backup has been
> committed (copied to `backup.tmp`, then `mv`'d to `backup`).

Therefore at any crash point: backup present means it is complete; backup
absent means the target was not touched. The remaining danger is a *stale*
backup - left by an earlier, committed deployment. If one were still there
when a new deployment crashed right after `INSTALLING`, recovery would restore
it and roll the target back two versions. Two independent safeguards:

1. `deploy-cli` deletes `DEPLOYGUARD_BACKUP_DIR` *before* writing
   `INSTALLING` (a crash in between leaves state `STABLE` with nothing
   changed);
2. the demo deploy hook tags its backup with `DEPLOYGUARD_RUN_ID`
   (`<backup>.run_id`), and the rollback hook only restores a backup whose tag
   matches the run it is rolling back.

## 5. Running hooks

* **fork + execvp, no shell.** The target path is passed as `argv[1]`, so a
  path like `x'; rm -rf / #` is just an odd directory name. (`system()` and
  `popen()` go through `/bin/sh -c` and would execute it.)
* **Wait for the child, not for EOF.** Output is read with `poll()` on
  non-blocking pipes while `waitpid(WNOHANG)` is checked every 50 ms. A deploy
  script that starts a background daemon passes our pipes on to it; waiting
  for EOF would block for as long as the daemon lives.
* **Own process group.** The child calls `setpgid(0, 0)` (the parent does
  too, to win the race), so a timeout or cancellation can signal the script
  *and everything it started* with `kill(-pgid, ...)`: SIGTERM first, SIGKILL
  after `kill_grace_seconds` (default 5), and a final SIGKILL sweep for
  stragglers.
* **A limit per kind of hook.** Deploy and rollback runs get
  `timeout_seconds` (default 600); a health check gets
  `health_check_timeout_seconds` (default 30). With a single limit, one hung
  health check would have cost 3 attempts x 10 minutes.
* **"Could not start" vs "exited 127".** A close-on-exec pipe carries `errno`
  back if `execvp()` fails; a successful exec closes it and the parent reads
  EOF. This is how `ENOENT`/`EACCES` are reported precisely. (Pre-flight
  checks catch most of these earlier.)
* **Hygiene.** stdin is `/dev/null` (a prompt cannot hang a pipeline),
  descriptors >= 3 are closed (`close_range()` on Linux), SIGPIPE is reset to
  its default (we ignore it ourselves, and ignored signals survive exec), and
  the signal mask is cleared. `argv` is built before `fork()`.
* **Context via environment.** `DEPLOYGUARD_PHASE`, `DEPLOYGUARD_TARGET`,
  `DEPLOYGUARD_RUN_ID`, `DEPLOYGUARD_BACKUP_DIR`, `DEPLOYGUARD_ATTEMPT` - the
  same pattern git hooks and CI systems use.

## 6. Observability

* **Log file**: every line, ours and the hooks' stdout *and* stderr, with a
  millisecond timestamp. Appended across runs; rotated to `<file>.1` once it
  passes 10 MiB, so it cannot fill the disk.
* **`--report <file>`**: a JSON summary - outcome, exit code, reason, run ids
  (including a recovered run's), and every hook run with attempt number,
  result and duration. Written atomically (temp file + rename) for every
  outcome, including invalid configuration, so a CI step can always read it.

## 7. Signals

* The handlers for SIGINT, SIGTERM and SIGHUP only store the signal number in
  a `volatile sig_atomic_t`; that is all a handler can safely do. The main loop
  polls it, terminates the running hook and rolls back.
* `SA_RESTART` is not set, so a blocking `poll()` returns `EINTR` at once.
* SIGINT and SIGHUP that were already *ignored* at start-up stay ignored. That
  is what `nohup` (and a non-interactive shell for background jobs) asks for;
  overriding it would let a closing terminal cancel a deployment the user
  deliberately detached.
* The rollback hook is never cancelled. Stopping a rollback halfway leaves the
  target in the worst possible state; if someone insists (`kill -9`), the state
  file still says `ROLLING_BACK` and the next run resumes it.
* SIGPIPE is ignored so that a closed stdout (`deploy-cli ... | head`) cannot
  kill `deploy-cli` halfway through a deployment.

## 8. Exit codes

The exit code is the API for the calling pipeline, so each code answers "what
state is the target in?" rather than "what went wrong?":

| Code | Target is | The pipeline should |
| --- | --- | --- |
| 0 | on the new version | continue |
| 1 | on the previous version | fail the job, nothing to clean up |
| 2 | unknown | page someone |
| 3 | unchanged (nothing attempted) | fix the input or wait for the other run |

Version 0.1 returned 1 for configuration errors too, which a pipeline could
not tell apart from "deployed and rolled back".

## 9. Testing strategy

* **Unit tests** (doctest, 164 cases) run each component against small
  `/bin/sh` scripts in a fresh temporary directory per test, so `ctest -j`
  runs them in parallel. They cover every edge of the state machine, the
  process handling (timeouts, process-group kills, SIGKILL escalation, exec
  errors, fd inheritance, stdin, SIGPIPE) and the parsers.
* **Integration tests** (bash) drive the real binary, because some properties
  only exist at that level: exit codes, real signals, and in particular the
  kernel releasing the lock of a process that was SIGKILLed mid-deployment.
* **Sanitizers**: the whole suite also runs under AddressSanitizer +
  UndefinedBehaviorSanitizer in CI.
* **Coverage**: about 90% of the lines in `src/`; CI prints the per-file
  report. The uncovered lines are mostly failing-syscall branches and the
  child side of `fork()`, which gcov cannot record because the child execs.
* **Not covered**: a real power cut (the fsync ordering is reasoned about,
  not tested) and network file systems.

## 10. Known limitations and possible next steps

* Single host only; `flock()` is not reliable on NFS.
* There is no overall deadline for a run, only per-hook limits; a run's
  worst case is the sum of all hook limits, retries and pauses.
* Run ids are unique per host (time, pid, counter), not globally.
* Retries use a fixed interval; exponential backoff would suit flaky
  networks better.
