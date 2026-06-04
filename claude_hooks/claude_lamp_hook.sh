#!/usr/bin/env bash
# Claude Lamp hook for Claude Code (ESP32 + WS2812B).
# Usage: claude_lamp_hook.sh <working|idle|input|off>
# Always exits 0 to never block Claude.

set -e

STATE="${1:-idle}"
PID_FILE="/tmp/claude_lamp_daemon.pid"
STATE_FILE="/tmp/claude_lamp_state"
DAEMON="$(cd "$(dirname "$0")" && pwd)/claude_lamp_daemon.py"

# 1. Write desired state
printf '%s' "$STATE" > "$STATE_FILE"

# 2. If daemon is alive, nothing more to do
if [ -f "$PID_FILE" ]; then
    pid=$(cat "$PID_FILE" 2>/dev/null)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        exit 0
    fi
    rm -f "$PID_FILE"
fi

# 3. Auto-detect python with bleak (dedicated venv first)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PYTHON=""
if [ -x "$SCRIPT_DIR/venv/bin/python3" ] && "$SCRIPT_DIR/venv/bin/python3" -c "import bleak" 2>/dev/null; then
    PYTHON="$SCRIPT_DIR/venv/bin/python3"
elif python3 -c "import bleak" 2>/dev/null; then
    PYTHON="python3"
elif /opt/homebrew/bin/python3 -c "import bleak" 2>/dev/null; then
    PYTHON="/opt/homebrew/bin/python3"
elif [ -n "$CONDA_PREFIX" ] && "$CONDA_PREFIX/bin/python3" -c "import bleak" 2>/dev/null; then
    PYTHON="$CONDA_PREFIX/bin/python3"
fi

if [ -z "$PYTHON" ]; then
    echo "[claude_lamp] No python with bleak found, skipping" >> /tmp/claude_lamp_daemon.log
    exit 0
fi

# 4. Launch daemon
nohup "$PYTHON" "$DAEMON" >> /tmp/claude_lamp_daemon.log 2>&1 &
echo $! > "$PID_FILE"

exit 0
