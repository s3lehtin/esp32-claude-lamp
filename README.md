# ESP32 Claude Status Lamp (WS2812B)

A physical status indicator for [Claude Code](https://docs.anthropic.com/en/docs/claude-code),
built on an ESP32 DevKit and a WS2812B LED strip. Fork of
[bobek-balinek/claude-lamp](https://github.com/bobek-balinek/claude-lamp), with the
commercial Moonside lamp replaced by DIY hardware — animations are rendered on the ESP32,
and macOS only sends state words over BLE.

| State | Effect | Trigger |
|---|---|---|
| **Working** | Slow breathing white ↔ navy | Prompt submit, tool use |
| **Idle** | Solid warm amber | Claude finishes responding, session start |
| **Needs input** | Gentle purple pulse | Permission request, plan approval, question |
| **Off** | Dark | Session end |

## Architecture

```
Claude Code hook event
  → claude_lamp_hook.sh (writes state to /tmp/claude_lamp_state, launches daemon if needed)
    → claude_lamp_daemon.py (persistent BLE connection, polls state file every 200ms)
      → ESP32 "CLAUDE-LAMP" via BLE (Nordic UART Service)
        → WS2812B strip (animations rendered in firmware)
```

## Hardware

- Classic ESP32 DevKit (ESP32-WROOM)
- WS2812B strip, short (~30 LEDs), powered from the board's 5V/VIN pin (USB)

### Wiring

| Strip wire | ESP32 pin |
|---|---|
| +5V | `VIN` / `5V` |
| GND | `GND` |
| DIN (data) | `GPIO 16` |

Notes:
- The ESP32 outputs 3.3V data; for short strips this is normally fine. If you see glitches,
  add a level shifter (74AHCT125) or a single "sacrificial" pixel powered at ~4.3V.
- Brightness is capped at 80/255 by default (`DEFAULT_BRIGHTNESS` in the sketch) to stay
  within USB power limits. A 330Ω resistor in the data line and a 470µF cap across power
  are good practice but optional for short strips.

## Firmware

Configuration constants at the top of [`firmware/claude_lamp/claude_lamp.ino`](firmware/claude_lamp/claude_lamp.ino):
`LED_PIN` (16), `LED_COUNT` (30), `DEFAULT_BRIGHTNESS` (80), `DEVICE_NAME` (`CLAUDE-LAMP`).

### Build & flash

```sh
make setup     # one-time: install ESP32 core + libraries
make compile
make upload    # auto-detects /dev/cu.usbserial-*, or: make upload PORT=/dev/cu.usbserial-0001
make monitor   # serial console at 115200 baud
```

On boot the strip runs a quick amber wipe (self-test), then stays dark and advertises as
`CLAUDE-LAMP`.

### BLE protocol

Nordic UART Service (`6e400001-…`), ASCII commands written to the RX characteristic
(`6e400002-…`):

| Command | Effect |
|---|---|
| `working` / `idle` / `input` / `off` | The four states |
| `color R,G,B` | Solid custom color, e.g. `color 255,0,128` |
| `bright N` | Brightness cap 0–255, e.g. `bright 120` |

## macOS setup

Requirements: macOS (CoreBluetooth), Python 3.10+ with [bleak](https://github.com/hbldh/bleak).

### 1. Copy scripts

```sh
mkdir -p ~/.claude/claude_lamp_hooks
cp claude_hooks/claude_lamp_hook.sh claude_hooks/claude_lamp_daemon.py ~/.claude/claude_lamp_hooks/
chmod +x ~/.claude/claude_lamp_hooks/claude_lamp_hook.sh
```

### 2. Create the venv

The hook looks for a `venv/` next to itself first (then falls back to `python3`,
`/opt/homebrew/bin/python3`, `$CONDA_PREFIX`):

```sh
python3 -m venv ~/.claude/claude_lamp_hooks/venv   # needs python 3.10+
~/.claude/claude_lamp_hooks/venv/bin/pip install bleak
```

### 3. Install the hooks

Merge `claude_hooks/settings.json` into `~/.claude/settings.json`, replacing
`$CLAUDE_PROJECT_DIR/claude_hooks` with `~/.claude/claude_lamp_hooks`:

```sh
sed 's|\$CLAUDE_PROJECT_DIR/claude_hooks|~/.claude/claude_lamp_hooks|g' claude_hooks/settings.json
```

### 4. Restart Claude Code

Open a new session — the daemon auto-discovers the lamp by name. First connection takes a
few seconds; tail the log to watch it.

### Hook event mapping

| Event | State |
|---|---|
| `SessionStart`, `Stop`, `Notification: idle_prompt` | idle |
| `UserPromptSubmit`, `PreToolUse` (work tools), `PostToolUse` (any tool) | working |
| `PreToolUse: AskUserQuestion/ExitPlanMode`, `PermissionRequest`, `Notification: permission_prompt` | input |
| `SessionEnd` | off |

`PostToolUse → working` makes the lamp return to breathing after you approve a permission
or answer a question. Known limitation: there is no "permission granted" hook event, so
during a single *long-running* approved command the lamp stays purple until that command
completes; multi-step turns self-correct on the next tool call.

## Daemon lifecycle

| File | Purpose |
|---|---|
| `/tmp/claude_lamp_daemon.pid` | Daemon PID |
| `/tmp/claude_lamp_state` | Current desired state |
| `/tmp/claude_lamp_daemon.log` | Daemon log |

The daemon keeps a persistent BLE connection (avoids 2–5s reconnects per hook event) and
auto-exits after 30 minutes idle or on `SessionEnd`.

## Smoke test without Claude Code

```sh
# use a python that has bleak, e.g. the hook venv:
~/.claude/claude_lamp_hooks/venv/bin/python3 - <<'EOF'
import asyncio
from bleak import BleakClient, BleakScanner

RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"

async def main():
    dev = next(d for d in await BleakScanner.discover(timeout=10)
               if (d.name or "").startswith("CLAUDE-LAMP"))
    async with BleakClient(dev) as c:
        for state in ("working", "input", "idle", "off"):
            print("->", state)
            await c.write_gatt_char(RX, state.encode(), response=True)
            await asyncio.sleep(4)

asyncio.run(main())
EOF
```

## Troubleshooting

**Lamp not responding:**
```sh
cat /tmp/claude_lamp_daemon.log
```

**Daemon stuck:**
```sh
kill "$(cat /tmp/claude_lamp_daemon.pid)"
rm -f /tmp/claude_lamp_daemon.pid /tmp/claude_lamp_state
```

**bleak not found:** the hook tries `venv/` next to itself first, then `python3`,
`/opt/homebrew/bin/python3`, and `$CONDA_PREFIX/bin/python3` — make sure one of them
has bleak installed.

**Flashing fails ("Invalid head of packet" / serial corruption):** don't flash through a
USB hub or dock — plug the board directly into the Mac. The CP2102 serial stream corrupts
behind hubs, which looks like a boot-mode problem but isn't.

## Acknowledgments

- [bobek-balinek/claude-lamp](https://github.com/bobek-balinek/claude-lamp) — the original
  project this is forked from; the hook → state file → BLE daemon architecture and the
  Claude Code hooks configuration come from there. A local clone lives in `fork/`
  (git-ignored, reference only).

## License

MIT, same as the original [claude-lamp](https://github.com/bobek-balinek/claude-lamp/blob/main/LICENSE).
