#!/bin/bash
set -e
trap 'echo "[rollback.sh] ❌ CRITICAL: Unhandled error on line $LINENO. Rollback failed!" >&2' ERR

TARGET_PATH="$1"

echo "[rollback.sh] ⚠️ Rollback operation initiated for: $TARGET_PATH"

if [ -z "$TARGET_PATH" ]; then
    echo "[rollback.sh] ❌ Error: Target path not provided."
    exit 1
fi

APP_NAME=$(basename "$TARGET_PATH")
BACKUP_DIR="${TMPDIR:-/tmp}/deploy_guard_backup_${APP_NAME}"

# 1. Verify backup exists securely
if [ ! -d "$BACKUP_DIR" ]; then
    echo "[rollback.sh] ❌ CRITICAL ERROR: Backup directory not found at $BACKUP_DIR!"
    echo "[rollback.sh] ❌ Automatic rollback aborted."
    exit 1
fi

echo "[rollback.sh] 🧹 Cleaning up corrupted deployment files..."
rm -rf "$TARGET_PATH"/*

# 2. Restore stable v1.0 files from temporary backup
echo "[rollback.sh] ♻️ Restoring backup files from: $BACKUP_DIR"
sleep 0.5
cp -r "$BACKUP_DIR/"* "$TARGET_PATH/"

# 3. Verify restored version
echo "[rollback.sh] 🔍 Verifying restored application..."
sleep 0.5
set +e
"$TARGET_PATH/app.sh"
VERIFY_CODE=$?
set -e

if [ $VERIFY_CODE -ne 0 ]; then
    echo "[rollback.sh] ❌ CRITICAL: Restored application failed post-rollback verification!"
    exit 1
fi

# 4. Clean up backup directory
echo "[rollback.sh] 🧹 Cleaning up temporary backup..."
rm -rf "$BACKUP_DIR"

# 5. Restart restored application
echo "[rollback.sh] 🔄 Restarting stable application as daemon..."
pkill -f "app.sh --daemon" || true
"$TARGET_PATH/app.sh" --daemon &

echo "[rollback.sh] ✅ Rollback successfully completed. Version 1.0 is active and healthy."
exit 0
