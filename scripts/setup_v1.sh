#!/bin/bash
# Set up workspace directories
WORKSPACE_DIR="/Users/wenbozhi/Downloads/deploy-rollback-cli cpp"
TARGET_DIR="$WORKSPACE_DIR/test_app_target"
RELEASE_v1="$WORKSPACE_DIR/releases/v1.0"
RELEASE_v2="$WORKSPACE_DIR/releases/v2.0"

echo "Initializing Demo Environment..."

# Clean up previous runs
rm -rf "$TARGET_DIR" "$WORKSPACE_DIR/releases"
mkdir -p "$RELEASE_v1"
mkdir -p "$RELEASE_v2"
mkdir -p "$TARGET_DIR"

# Create v1.0 file (Stable Version)
cat << 'EOF' > "$RELEASE_v1/app.sh"
#!/bin/bash
echo "====================================="
echo "   🟢 MyWebservice App - Version 1.0"
echo "   Status: Running stable"
echo "   Port: 8080"
echo "====================================="
exit 0
EOF
chmod +x "$RELEASE_v1/app.sh"

# Create v2.0 file (Broken Version)
cat << 'EOF' > "$RELEASE_v2/app.sh"
#!/bin/bash
echo "====================================="
echo "   🔴 MyWebservice App - Version 2.0"
echo "   Status: Starting..."
echo "====================================="
sleep 0.5
echo "[v2.0] Loading application modules..."
sleep 0.5
echo "[v2.0] ❌ FATAL ERROR: Database schema mismatch! (Column 'avatar_url' is missing in table 'users')" >&2
echo "[v2.0] ❌ Failed to start application!" >&2
exit 1
EOF
chmod +x "$RELEASE_v2/app.sh"

# Copy v1.0 to Target to simulate an initially active stable installation
cp "$RELEASE_v1/app.sh" "$TARGET_DIR/app.sh"
echo "✅ Environment initialized!"
echo "Stable version v1.0 is currently running at: $TARGET_DIR"
"$TARGET_DIR/app.sh"
