# shellcheck shell=bash
# Shared helpers for the integration tests. Each test_*.sh sources this file.
#
#   usage: test_<name>.sh <path/to/deploy-cli> <repository root>
#
# The tests drive the real deploy-cli binary - including SIGKILL-ing it in
# the middle of a deployment - so they cover what unit tests cannot: process
# exit codes, signals, and the kernel releasing the lock of a dead process.
# Everything happens in a private temporary directory, so tests can run in
# parallel. Compatible with macOS's bash 3.2 (no `timeout`, no /proc).
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <path/to/deploy-cli> <repository root>" >&2
    exit 2
fi
CLI="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
# shellcheck disable=SC2034  # REPO and LOG are used by the sourcing tests
REPO="$(cd "$2" && pwd)"
[ -x "$CLI" ] || { echo "deploy-cli not found: $CLI" >&2; exit 2; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/deployguard-it.XXXXXX")"
WORK="$(cd "$WORK" && pwd -P)"
EVENTS="$WORK/events"   # hooks append their name here
# shellcheck disable=SC2034
LOG="$WORK/run.log"     # deploy-cli's log file
export NO_COLOR=1

BACKGROUND_PIDS=""
_cleanup() {
    local pid f
    for pid in $BACKGROUND_PIDS; do
        kill -9 "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    # Demo apps started by the bundled hooks, and pids recorded by tests.
    for f in "$WORK"/.demo_app_*.pid "$WORK"/*.pid; do
        [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap _cleanup EXIT

CURRENT=""
section() { CURRENT="$*"; }
pass() { echo "ok - $CURRENT"; }
fail() {
    {
        echo "FAIL - $CURRENT"
        printf '  %b\n' "$*"
        if [ -n "${OUT:-}" ]; then
            echo "  ---- deploy-cli output (last 30 lines) ----"
            printf '%s\n' "$OUT" | tail -n 30 | sed 's/^/  | /'
        fi
    } >&2
    exit 1
}

# write_hook <path> <shell commands>: an executable /bin/sh script.
write_hook() {
    printf '#!/bin/sh\n%s\n' "$2" >"$1"
    chmod +x "$1"
}

# write_config <file> <target> <deploy> <rollback> [extra YAML lines...]
write_config() {
    local file="$1" target="$2" deploy="$3" rollback="$4"
    shift 4
    {
        echo "deployment:"
        echo "  target_path: \"$target\""
        echo "  scripts:"
        echo "    deploy: \"$deploy\""
        echo "    rollback: \"$rollback\""
        local line
        for line in "$@"; do
            echo "  $line"
        done
    } >"$file"
}

# Starts `"$@"` in the background with a watchdog: the process is SIGKILLed
# after $WATCHDOG_SECONDS (default 60) so a hang fails the test instead of
# blocking CI. Sets $BG_PID.
WATCHDOG_SECONDS=60
start_bg() {
    local out="$1"
    shift
    "$@" >"$out" 2>&1 &
    BG_PID=$!
    BACKGROUND_PIDS="$BACKGROUND_PIDS $BG_PID"
    local target=$BG_PID
    (
        exec >/dev/null 2>&1
        sleep "$WATCHDOG_SECONDS" &
        sleeper=$!
        trap 'kill "$sleeper" 2>/dev/null; exit 0' TERM
        wait "$sleeper" && kill -9 "$target"
    ) &
    BACKGROUND_PIDS="$BACKGROUND_PIDS $!"
    eval "WATCHDOG_OF_$BG_PID=$!"
}

# wait_bg <pid>: waits for a start_bg process, stops its watchdog; sets $EXIT.
# (stderr of `wait` is silenced: bash would print "Killed" for processes
# that a test kills on purpose.)
wait_bg() {
    local watchdog
    set +e
    wait "$1" 2>/dev/null
    EXIT=$?
    eval "watchdog=\${WATCHDOG_OF_$1:-}"
    if [ -n "$watchdog" ]; then
        kill -TERM "$watchdog" 2>/dev/null
        wait "$watchdog" 2>/dev/null
    fi
    set -e
}

# run_cli <args...>: runs deploy-cli in the foreground (with a watchdog).
# Sets $EXIT (its exit code), $OUT (its output) and $ELAPSED (seconds).
run_cli() {
    local start
    start=$(date +%s)
    start_bg "$WORK/cli.out" "$CLI" "$@"
    wait_bg "$BG_PID"
    ELAPSED=$(( $(date +%s) - start ))
    OUT="$(cat "$WORK/cli.out")"
}

# wait_for_file <path> [seconds]
wait_for_file() {
    local tries=$(( ${2:-10} * 10 ))
    while [ ! -e "$1" ]; do
        tries=$(( tries - 1 ))
        [ "$tries" -gt 0 ] || fail "timed out waiting for $1"
        sleep 0.1
    done
}

# process_running <pid>: true if it exists and is not a zombie.
process_running() {
    local state
    kill -0 "$1" 2>/dev/null || return 1
    state="$(ps -o stat= -p "$1" 2>/dev/null | tr -d ' ')"
    [ -n "$state" ] && [ "${state#Z}" = "$state" ]
}

# wait_until_dead <pid> [seconds]
wait_until_dead() {
    local tries=$(( ${2:-5} * 10 ))
    while process_running "$1"; do
        tries=$(( tries - 1 ))
        [ "$tries" -gt 0 ] || return 1
        sleep 0.1
    done
}

events() { [ -f "$EVENTS" ] && tr '\n' ' ' <"$EVENTS" | sed 's/ $//'; true; }
reset_events() { rm -f "$EVENTS"; }

expect_exit()        { [ "$EXIT" -eq "$1" ] || fail "expected exit code $1, got $EXIT"; }
expect_output()      { printf '%s' "$OUT" | grep -qF -- "$1" || fail "output does not contain: $1"; }
expect_events()      { [ "$(events)" = "$1" ] || fail "expected hooks [$1], got [$(events)]"; }
expect_exists()      { [ -e "$1" ] || fail "expected $1 to exist"; }
expect_missing()     { [ ! -e "$1" ] || fail "expected $1 not to exist"; }
expect_file_has()    { grep -qF -- "$2" "$1" 2>/dev/null || fail "$1 does not contain: $2"; }
expect_first_line()  { [ "$(head -n 1 "$1" 2>/dev/null)" = "$2" ] || fail "first line of $1 is not '$2'"; }
expect_faster_than() { [ "$ELAPSED" -lt "$1" ] || fail "took ${ELAPSED}s, expected under $1s"; }
