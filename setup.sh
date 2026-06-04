#!/usr/bin/env bash
# Full vanilla setup for the ESP32 Claude Status Lamp.
#
#   ./setup.sh                  firmware (build + optional flash) and macOS host install
#   ./setup.sh --firmware-only  only check toolchain, build and optionally flash
#   ./setup.sh --host-only      only install hooks/daemon/venv and merge settings.json
#   ./setup.sh --no-flash       build but never offer to flash
#
# Checks prerequisites and tells you exactly what to install if something is
# missing — it never installs system-level tools itself. Safe to re-run:
# every step is idempotent.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
HOOKS_DIR="$HOME/.claude/claude_lamp_hooks"
SETTINGS="$HOME/.claude/settings.json"
ESP32_URL="https://espressif.github.io/arduino-esp32/package_esp32_index.json"

DO_FIRMWARE=1
DO_HOST=1
DO_FLASH=1

for arg in "$@"; do
    case "$arg" in
        --host-only)     DO_FIRMWARE=0 ;;
        --firmware-only) DO_HOST=0 ;;
        --no-flash)      DO_FLASH=0 ;;
        -h|--help)       sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $arg (try --help)" >&2; exit 1 ;;
    esac
done

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
info() { printf '    %s\n' "$*"; }
fail() { printf '\033[31mError:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- guards ----

[ "$(uname -s)" = "Darwin" ] || fail "this script targets macOS (BLE via CoreBluetooth)."
[ -f "$REPO_DIR/firmware/claude_lamp/claude_lamp.ino" ] \
    || fail "run from the repo root (firmware/claude_lamp/claude_lamp.ino not found)."
cd "$REPO_DIR"

# --------------------------------------------------- prerequisite checks ----

step "Checking prerequisites"

# Python ≥ 3.10 (needed by the daemon; system /usr/bin/python3 3.9 is too old)
PYTHON=""
for cand in python3 /opt/homebrew/bin/python3; do
    if command -v "$cand" >/dev/null 2>&1 \
       && "$cand" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' 2>/dev/null; then
        PYTHON="$cand"
        break
    fi
done
[ -n "$PYTHON" ] || fail "no Python 3.10+ found. Install one with:  brew install python"
info "python: $PYTHON ($("$PYTHON" -V 2>&1))"

if [ "$DO_FIRMWARE" = 1 ]; then
    command -v arduino-cli >/dev/null 2>&1 \
        || fail "arduino-cli not found. Install it with:  brew install arduino-cli"
    info "arduino-cli: $(arduino-cli version | head -n1)"
fi

# ---------------------------------------------------------------- firmware ----

if [ "$DO_FIRMWARE" = 1 ]; then
    step "Configuring ESP32 board support"
    arduino-cli config init >/dev/null 2>&1 || true   # no-op if a config already exists
    if arduino-cli config dump 2>/dev/null | grep -q "package_esp32_index.json"; then
        info "ESP32 board-manager URL already configured"
    else
        arduino-cli config add board_manager.additional_urls "$ESP32_URL"
        info "added board-manager URL: $ESP32_URL"
    fi

    step "Installing ESP32 core + libraries (make setup)"
    make setup

    step "Compiling firmware (make compile)"
    make compile

    # Same port globs as the Makefile
    PORT=""
    for p in /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART /dev/cu.wchusbserial*; do
        [ -e "$p" ] && { PORT="$p"; break; }
    done

    if [ "$DO_FLASH" = 1 ] && [ -n "$PORT" ] && [ -t 0 ]; then
        printf '\nBoard detected on %s. Flash now? [y/N] ' "$PORT"
        read -r answer
        if [ "$answer" = "y" ] || [ "$answer" = "Y" ]; then
            step "Flashing (make upload PORT=$PORT)"
            make upload PORT="$PORT"
        else
            info "skipped — flash later with: make upload"
        fi
    elif [ -n "$PORT" ]; then
        info "board on $PORT — flash with: make upload"
    else
        info "no board detected — plug the ESP32 in directly (not via a hub, see"
        info "Troubleshooting in README.md) and run: make upload"
    fi
fi

# ------------------------------------------------------------ host install ----

if [ "$DO_HOST" = 1 ]; then
    step "Installing hook + daemon to $HOOKS_DIR"
    mkdir -p "$HOOKS_DIR"
    cp "$REPO_DIR/claude_hooks/claude_lamp_hook.sh" \
       "$REPO_DIR/claude_hooks/claude_lamp_daemon.py" "$HOOKS_DIR/"
    chmod +x "$HOOKS_DIR/claude_lamp_hook.sh"

    # Stop a running daemon so the next hook event respawns the fresh code
    if pid=$(cat /tmp/claude_lamp_daemon.pid 2>/dev/null) && [ -n "$pid" ] \
       && kill "$pid" 2>/dev/null; then
        rm -f /tmp/claude_lamp_daemon.pid
        info "stopped running daemon (pid $pid) — next hook event restarts it"
    fi

    step "Setting up Python venv with bleak"
    if [ -x "$HOOKS_DIR/venv/bin/python3" ] \
       && "$HOOKS_DIR/venv/bin/python3" -c "import bleak" 2>/dev/null; then
        info "venv with bleak already present"
    else
        "$PYTHON" -m venv "$HOOKS_DIR/venv"
        "$HOOKS_DIR/venv/bin/pip" install --quiet --upgrade pip
        "$HOOKS_DIR/venv/bin/pip" install --quiet bleak
        info "created $HOOKS_DIR/venv and installed bleak"
    fi

    step "Merging hook entries into $SETTINGS"
    "$PYTHON" - "$REPO_DIR/claude_hooks/settings.json" "$HOOKS_DIR" "$SETTINGS" <<'PYEOF'
import json, os, shutil, sys, time

src_path, hooks_dir, settings_path = sys.argv[1:4]

with open(src_path) as f:
    src = json.load(f)

def subst(obj):
    """Point hook commands at the installed copy (absolute path — '~' would
    not expand inside the double-quoted command strings)."""
    if isinstance(obj, str):
        return obj.replace("$CLAUDE_PROJECT_DIR/claude_hooks", hooks_dir)
    if isinstance(obj, list):
        return [subst(x) for x in obj]
    if isinstance(obj, dict):
        return {k: subst(v) for k, v in obj.items()}
    return obj

src_hooks = subst(src["hooks"])

try:
    with open(settings_path) as f:
        settings = json.load(f)
except FileNotFoundError:
    settings = {}
except json.JSONDecodeError as e:
    sys.exit(f"{settings_path} is not valid JSON ({e}) — fix it and re-run.")

before = json.dumps(settings, sort_keys=True)
hooks = settings.setdefault("hooks", {})

def is_lamp(group):
    return any("claude_lamp_hook.sh" in h.get("command", "")
               for h in group.get("hooks", []))

# Replace any existing lamp entries with fresh ones — idempotent re-runs,
# and in-place upgrades when the hook config changes upstream.
for event, groups in src_hooks.items():
    hooks[event] = [g for g in hooks.get(event, []) if not is_lamp(g)] + groups

if json.dumps(settings, sort_keys=True) == before:
    print("    hooks already installed — no change")
    sys.exit(0)

if os.path.exists(settings_path):
    backup = settings_path + ".bak." + time.strftime("%Y%m%d-%H%M%S")
    shutil.copy2(settings_path, backup)
    print(f"    backed up old settings to {backup}")

os.makedirs(os.path.dirname(settings_path), exist_ok=True)
with open(settings_path, "w") as f:
    json.dump(settings, f, indent=2)
    f.write("\n")
print(f"    hook entries merged into {settings_path}")
PYEOF
fi

# ---------------------------------------------------------------- summary ----

step "Done"
[ "$DO_HOST" = 1 ] && cat <<EOF
Next steps:
  1. Restart Claude Code — the daemon auto-discovers the lamp ("CLAUDE-LAMP")
     on the first hook event; the first connection takes a few seconds.
  2. The first usage fetch may pop a macOS Keychain dialog for
     "Claude Code-credentials" — click "Always Allow" (the daemon runs
     headless; until then the usage bars stay dark).
  3. Watch it work:  tail -f /tmp/claude_lamp_daemon.log
     Or smoke-test without Claude Code — see README.md.
EOF
exit 0
