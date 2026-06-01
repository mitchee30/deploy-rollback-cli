#!/bin/bash
if [ "$1" == "--daemon" ]; then
    echo "Service running in background (PID $$)..."
    while true; do sleep 10; done
else
    echo "====================================="
    echo "   🟢 MyWebservice App - Version 1.0"
    echo "   Status: Running stable"
    echo "   Port: 8080"
    echo "====================================="
    exit 0
fi
