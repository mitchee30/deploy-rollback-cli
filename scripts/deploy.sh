#!/bin/bash
set -e
trap 'echo "[deploy.sh] ❌ CRITICAL: Unhandled error on line $LINENO. Aborting deployment!" >&2' ERR

TARGET_PATH="$1"
WORKSPACE_DIR="/Users/wenbozhi/Downloads/deploy-rollback-cli cpp"
RELEASE_V2="$WORKSPACE_DIR/releases/v2.0"

echo "[deploy.sh] 🚀 Starting deployment to target: $TARGET_PATH"

# 1. Pre-installation checks
if [ -z "$TARGET_PATH" ]; then
    echo "[deploy.sh] ❌ Error: Target path not provided."
    exit 1
fi

if [ ! -d "$TARGET_PATH" ]; then
    echo "[deploy.sh] ❌ Error: Target directory $TARGET_PATH does not exist."
    exit 1
fi

# 1.5 Process Mutex & Pre-flight Check
echo "[deploy.sh] 🔍 Pre-flight checks: Scanning for active application processes..."
if pgrep -f "app.sh --daemon" > /dev/null; then
    echo "[deploy.sh] ⚠️ Detected running instance of app.sh. Initiating safe shutdown..."
    pkill -f "app.sh --daemon" || true
    sleep 1
    echo "[deploy.sh] ✅ Active processes safely terminated."
else
    echo "[deploy.sh] ✅ No active processes found."
fi

# 2. Dynamic platform-appropriate temp backup path
APP_NAME=$(basename "$TARGET_PATH")
BACKUP_DIR="${TMPDIR:-/tmp}/deploy_guard_backup_${APP_NAME}"

echo "[deploy.sh] 📦 Creating temporary backup of v1.0 in: $BACKUP_DIR"
rm -rf "$BACKUP_DIR"
mkdir -p "$BACKUP_DIR"

# Copy all existing files to temporary backup
cp -r "$TARGET_PATH/"* "$BACKUP_DIR/"
echo "[deploy.sh] ✅ Backup successfully saved to temp."

# 3. Simulate copying/installing the new version (v2.0)
echo "[deploy.sh] ⚙️ Overwriting installation with new v2.0 files..."
sleep 0.5
rm -rf "$TARGET_PATH"/*
cp -r "$RELEASE_V2/"* "$TARGET_PATH/"

# 4. Perform Health Check verification on newly installed v2.0
echo "[deploy.sh] 🔍 Initiating post-deployment health checks..."
sleep 0.5

# Suspend strict error trapping specifically to manually evaluate the health check status
set +e
"$TARGET_PATH/app.sh"
HEALTH_CODE=$?
set -e

if [ $HEALTH_CODE -ne 0 ]; then
    echo "[deploy.sh] ❌ Post-deployment health check failed with exit code $HEALTH_CODE!"
    echo "[deploy.sh] ❌ Halting installation. Signalling orchestrator to rollback."
    exit 1
fi

# 5. Success Phase: Restart the application as daemon
"$TARGET_PATH/app.sh" --daemon &
echo "[deploy.sh] 🎉 Deployment successfully verified and application launched in background."
exit 0
