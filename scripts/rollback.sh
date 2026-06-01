#!/bin/bash
TARGET_PATH="$1"

echo "[rollback.sh] ⚠️ Rollback operation initiated for: $TARGET_PATH"

if [ -z "$TARGET_PATH" ]; then
    echo "[rollback.sh] ❌ Error: Target path not provided."
    exit 1
fi

# Locate the same dynamic temporary backup path
APP_NAME=$(basename "$TARGET_PATH")
BACKUP_DIR="${TMPDIR:-/tmp}/deploy_guard_backup_${APP_NAME}"

# 1. Verify backup exists
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
if [ $? -ne 0 ]; then
    echo "[rollback.sh] ❌ CRITICAL: Failed to copy backup files back to target!"
    exit 1
fi

# 3. Verify restored version
echo "[rollback.sh] 🔍 Verifying restored application..."
sleep 0.5
"$TARGET_PATH/app.sh"
VERIFY_CODE=$?

if [ $VERIFY_CODE -ne 0 ]; then
    echo "[rollback.sh] ❌ CRITICAL: Restored application failed post-rollback verification!"
    exit 1
fi

# 4. Clean up backup directory
echo "[rollback.sh] 🧹 Cleaning up temporary backup..."
rm -rf "$BACKUP_DIR"

echo "[rollback.sh] ✅ Rollback successfully completed. Version 1.0 is active and healthy."
exit 0
