#!/usr/bin/env bash
# Demo application, version 1.1 (healthy).
#
#   app.sh            print a banner
#   app.sh --version  print the version
#   app.sh --daemon   run as a long-lived service (until SIGTERM)
#   app.sh --health   exit 0 if the service is healthy
set -euo pipefail
VERSION="1.1"

case "${1:-}" in
    --version)
        echo "$VERSION"
        ;;
    --daemon)
        echo "[app v$VERSION] service started (pid $$)"
        trap 'echo "[app v$VERSION] stopping"; exit 0' TERM INT
        while true; do sleep 1; done
        ;;
    --health)
        echo "[app v$VERSION] healthy"
        ;;
    "")
        echo "====================================="
        echo "   🟢 MyWebservice App - Version $VERSION"
        echo "   Status: running stable, port 8080"
        echo "====================================="
        ;;
    *)
        echo "usage: app.sh [--version | --daemon | --health]" >&2
        exit 2
        ;;
esac
