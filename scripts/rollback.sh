#!/bin/bash
set -e
trap 'echo "[rollback.sh] ❌ CRITICAL: Unhandled error on line $LINENO. Rollback failed!" >&2' ERR

TARGET_PATH="$1"

echo "[rollback.sh] ⚠️ Rollback operation initiated for: $TARGET_PATH"

if [ -z "$TARGET_PATH" ]; then
    echo "[rollback.sh] ❌ Error: Target path not provided."
    exit 1
fi

# Must match the persistent, same-disk backup location computed in deploy.sh.
APP_NAME=$(basename "$TARGET_PATH")
TARGET_PARENT_DIR="$(cd "$(dirname "$TARGET_PATH")" && pwd)"
BACKUP_DIR="$TARGET_PARENT_DIR/.deploy_guard_backup_${APP_NAME}"

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
    echo "[rollback.sh] ❌ Backup at $BACKUP_DIR is preserved for manual recovery."
    exit 1
fi

# 4. Restart restored application
echo "[rollback.sh] 🔄 Restarting stable application as daemon..."
pkill -f "app.sh --daemon" || true
"$TARGET_PATH/app.sh" --daemon &

# 5. Confirm the daemon actually came up before touching the backup.
sleep 1
if ! pgrep -f "app.sh --daemon" > /dev/null; then
    echo "[rollback.sh] ❌ CRITICAL: Restored application did not stay running after restart!"
    echo "[rollback.sh] ❌ Backup at $BACKUP_DIR is preserved for manual recovery."
    exit 1
fi

# 6. Only now that the restart + health check are confirmed good do we clean
# up the backup. If we crash again before this point, the next self-heal
# still has a backup to restore from; deleting it any earlier would make a
# second crash unrecoverable.
echo "[rollback.sh] 🧹 Cleaning up backup (restart confirmed healthy)..."
rm -rf "$BACKUP_DIR"

echo "[rollback.sh] ✅ Rollback successfully completed. Version 1.0 is active and healthy."
exit 0
