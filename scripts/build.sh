#!/bin/bash
set -e

# Get the directory of the script
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_DIR="$( dirname "$SCRIPT_DIR" )"

BUILD_DIR="$PROJECT_DIR/build"

echo "Creating build directory at $BUILD_DIR..."
mkdir -p "$BUILD_DIR"

cd "$BUILD_DIR"

echo "Running CMake..."
cmake ..

echo "Building project..."
make

echo "Build complete. Executable is at $BUILD_DIR/deploy-cli"
