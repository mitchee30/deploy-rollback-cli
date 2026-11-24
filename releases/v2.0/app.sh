#!/usr/bin/env bash
# Demo application, version 2.0 (broken).
#
# The process starts fine, but the release expects a database column that
# does not exist, so its health check fails. This is the failure mode a
# plain "did the deploy script exit 0?" check misses - and the reason
# deploy-cli runs a separate health check before committing a deployment.
set -euo pipefail
VERSION="2.0"

fatal() {
    echo "[app v$VERSION] FATAL ERROR: Database schema mismatch! (column 'avatar_url' is missing in table 'users')" >&2
}

case "${1:-}" in
    --version)
        echo "$VERSION"
        ;;
    --daemon)
        echo "[app v$VERSION] service started (pid $$)"
        fatal
        echo "[app v$VERSION] running in degraded mode: every request fails" >&2
        trap 'echo "[app v$VERSION] stopping"; exit 0' TERM INT
        while true; do sleep 1; done
        ;;
    --health)
        fatal
        exit 1
        ;;
    "")
        echo "====================================="
        echo "   🔴 MyWebservice App - Version $VERSION"
        echo "====================================="
        ;;
    *)
        echo "usage: app.sh [--version | --daemon | --health]" >&2
        exit 2
        ;;
esac
