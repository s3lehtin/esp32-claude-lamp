# Design: Token Usage Bars

## Affected files

| File | Change |
|---|---|
| `firmware/claude_lamp/claude_lamp.ino` | Partition strip, new `usage` command, bar rendering, single-`show()` frame model |
| `claude_hooks/claude_lamp_daemon.py` | Keychain + OAuth usage fetch, 60 s poll in main loop, `--once` flag |
| `README.md` | Protocol table + setup notes (keychain prompt, redeploy) |
| `claude_hooks/claude_lamp_hook.sh`, `claude_hooks/settings.json` | **Unchanged** |

## Architecture

```
macOS Keychain ──security──▶ daemon ──GET /api/oauth/usage──▶ utilization %
                              │  (every 60 s, asyncio.to_thread)
Claude Code hooks ──state file┤
                              ▼  single asyncio loop, serialized BLE writes
                        BleakClient ──"usage P7,P5" / "working"/…──▶ ESP32 NUS RX
                                                                       │
                                              renderFrame() @50 fps:   ▼
                                              [0..19 status][20..24 7d][25..29 5h]
```

Usage data flows daemon → firmware only; the hook script and state-file protocol are
untouched (`usage` is never a *state*, it is daemon-internal).

## Firmware design (`firmware/claude_lamp/claude_lamp.ino`)

### Constants & state

After the config block (~line 34):

```cpp
#define STATUS_LEDS  20   // indices 0..19: status animation
#define BAR7_START   20   // 7-day bar: indices 20..24
#define BAR5_START   25   // 5-hour bar: indices 25..29
#define BAR_LEN       5
#define BLINK_THRESHOLD  95   // util >= this -> bar blinks (AC-14)
#define BLINK_PERIOD_MS  1000 // ~1 Hz: 500 ms on, 500 ms off
```

Alongside `lampState` (~line 48):

```cpp
volatile int16_t util7 = -1;        // -1 = unknown → bar dark (AC-5)
volatile int16_t util5 = -1;
volatile bool forceRender = false;  // set by `bright` to repaint static states
```

### Single-`show()` frame model (AC-1, AC-4, R-4)

`fillColor()` (line 55) currently loops all 30 LEDs and calls `strip.show()` — it would
overwrite the bars and flash them every frame. Change: loop bound `LED_COUNT` →
`STATUS_LEDS`, **delete its `strip.show()`**. One `show()` per frame at the end of
`renderFrame()`. All callers reviewed:

- `animWorking` (65) / `animInput` (72): set pixels only; fine.
- `renderFrame` STATE_IDLE (84) / STATE_COLOR (87): fine (buffer persists between frames).
- `handleCommand` `bright` branch (121–122): **remove the inline `fillColor` calls**, set
  `forceRender = true` instead; next tick (≤20 ms) repaints (AC-7).

Reworked `renderFrame()`:

```cpp
static void renderFrame(uint32_t now) {
  static LampState lastRendered = STATE_OFF;
  bool entered = (lampState != lastRendered) || forceRender;
  lastRendered = lampState;
  forceRender = false;

  if (lampState == STATE_OFF) {     // all 30 dark, bars included (AC-4)
    strip.clear();
    strip.show();
    return;
  }
  switch (lampState) {
    case STATE_WORKING: animWorking(now); break;
    case STATE_INPUT:   animInput(now);   break;
    case STATE_IDLE:    if (entered) fillColor(255, 140, 30); break;
    case STATE_COLOR:   fillColor(customR, customG, customB); break;
    default: break;
  }
  renderUsageBars(now);  // repainted every frame — idempotent, 10 setPixelColor calls
  strip.show();          // single show per frame
}
```

Bars are repainted every frame rather than maintained on change: removes all ordering
hazards with the per-frame animations at negligible cost.

### Bar rendering (AC-2, AC-3, AC-14)

```cpp
// 0..50 ramps green→yellow, 50..100 yellow→red; whole bar one hue (AC-3)
static uint32_t barColor(int u) {
  u = constrain(u, 0, 100);
  uint8_t r, g;
  if (u <= 50) { r = (uint8_t)(255 * u / 50);         g = 255; }
  else         { r = 255; g = (uint8_t)(255 * (100 - u) / 50); }
  return strip.Color(r, g, 0);
}

// Fuel gauge (revised 2026-06-04): paint REMAINING budget (100-util) — full at
// 0% usage, drains as usage grows. Hue still keyed to usage via barColor(util).
static void renderBar(int start, int util, uint32_t now) {
  if (util < 0) {                                   // unknown → all dark (AC-5)
    for (int i = 0; i < BAR_LEN; i++) strip.setPixelColor(start + i, 0);
    return;
  }
  if (util >= BLINK_THRESHOLD) {                    // imminent-limit alarm (AC-14):
    bool on = (now % BLINK_PERIOD_MS) < BLINK_PERIOD_MS / 2;  // one full red LED,
    strip.setPixelColor(start, on ? strip.Color(255, 0, 0) : 0);  // blinking ~1 Hz
    for (int i = 1; i < BAR_LEN; i++) strip.setPixelColor(start + i, 0);
    return;
  }
  int remaining = 100 - util;                       // fuel left (AC-2)
  uint32_t col = barColor(util);                    // hue keyed to usage (AC-3)
  for (int i = 0; i < BAR_LEN; i++) {
    int px = start + i, lo = i * 20, hi = lo + 20;  // LED i covers [lo, hi) of remaining
    if (remaining >= hi)      strip.setPixelColor(px, col);     // full
    else if (remaining <= lo) strip.setPixelColor(px, 0);       // drained
    else {                                                      // fractional (AC-2)
      int frac = remaining - lo;                                // 1..19
      strip.setPixelColor(px, strip.Color(
        (uint8_t)(((col >> 16) & 0xFF) * frac / 20),
        (uint8_t)(((col >>  8) & 0xFF) * frac / 20),
        (uint8_t)(( col        & 0xFF) * frac / 20)));
    }
  }
}

static void renderUsageBars(uint32_t now) {
  renderBar(BAR7_START, util7, now);
  renderBar(BAR5_START, util5, now);
}
```

Per-pixel fractional dimming composes with the global `strip.setBrightness()` cap — intended.

Blink notes: keyed off the same `millis()` already passed into `renderFrame()` — no new
timers. `now % BLINK_PERIOD_MS` gives a free-running 1 Hz square wave; both bars share
phase (blink in sync) when both are ≥ 95 %, which reads as intentional. The alarm shows
exactly one full-brightness red LED (first of the bar) so an exhausted budget (100 %)
remains visually distinct from "no data" (fully dark, `-1`).

### BLE protocol addition (AC-6)

Grammar (ASCII over NUS RX `6e400002-…`, case-insensitive, optional trailing `\n`):

| Command | Effect |
|---|---|
| `usage P7,P5` | set 7-day = P7 %, 5-hour = P5 % (ints, clamped 0–100), e.g. `usage 73,12` |
| `usage -` | clear both bars to unknown (dark) |

New branch in `handleCommand()` (before the final else):

```cpp
else if (cmd.startsWith("usage ")) {
  const char *arg = cmd.c_str() + 6;
  if (arg[0] == '-') { util7 = -1; util5 = -1; }
  else {
    int a, b;
    if (sscanf(arg, "%d,%d", &a, &b) == 2) {
      util7 = (int16_t)constrain(a, 0, 100);
      util5 = (int16_t)constrain(b, 0, 100);
    } else Serial.println("Bad usage syntax: usage P7,P5 | usage -");
  }
}
```

`usage` never touches `lampState` — usage stays orthogonal to status. Header comment
block (lines 8–14) gains the new command.

## Daemon design (`claude_hooks/claude_lamp_daemon.py`)

### Approach: time-gated poll in the existing single loop (AC-11)

No second asyncio task — the single `BleakClient` must not see interleaved
`write_gatt_char` calls from two tasks. The existing 200 ms loop gains a monotonic-time
gate; the only blocking parts (keychain subprocess, HTTPS) run via `asyncio.to_thread`,
and the resulting `send()` happens back on the main loop.

### New constants & helpers (stdlib only — no new venv deps)

```python
USAGE_POLL_INTERVAL = 180           # baseline seconds (AC-9, rev. after 429s)
USAGE_BACKOFF_MAX = 1800            # cap for HTTP 429 exponential backoff (AC-15)
USAGE_FAIL_CLEAR_THRESHOLD = 3      # consecutive hard failures → "usage -" (AC-10)
KEYCHAIN_ITEM = "Claude Code-credentials"
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
OAUTH_BETA = "oauth-2025-04-20"
```

`fetch_usage()` returns a status triple — `("ok", (u7,u5))`, `("rate_limited",
retry_after|None)`, or `("error", None)`. The loop keeps a `next_usage_poll` deadline
instead of a fixed-interval timestamp: success → `+USAGE_POLL_INTERVAL` and backoff
reset; 429 → `+max(Retry-After, backoff)` with backoff doubling to `USAGE_BACKOFF_MAX`,
last value kept on the lamp (rate limiting never clears the bars — AC-15); hard error →
`+USAGE_POLL_INTERVAL` and the AC-10 fail counter.

**Cache (AC-16)**: `USAGE_CACHE_FILE = "/tmp/claude_lamp_usage_cache.json"`,
`USAGE_CACHE_TTL = 300`. `fetch_usage()` first tries `read_usage_cache()` (JSON
`{ts, u7, u5}`, epoch seconds; fresh if `time.time() - ts < TTL`) and returns
`("ok", cached)` without HTTP; successful HTTP fetches `write_usage_cache()`. This
decouples the lamp resend cadence (180 s, reboot-heal) from the API query cadence
(≤1 per 5 min across all daemon restarts). Cache holds only percentages — no
credential material.

- `read_access_token() -> str | None` — `subprocess.run(["security",
  "find-generic-password", "-s", KEYCHAIN_ITEM, "-w"], timeout=10)`, parse JSON,
  return `claudeAiOauth.accessToken`; return `None` if `expiresAt` (ms epoch, **A-1**)
  is in the past or on any error. Never logs the token (AC-13).
- `_fetch_usage_blocking(token) -> dict` — `urllib.request` GET with
  `Authorization: Bearer` + `anthropic-beta` headers, 10 s timeout.
- `async fetch_usage() -> tuple[int, int] | None` — token → `asyncio.to_thread(_fetch_
  usage_blocking)` → parse `seven_day.utilization` / `five_hour.utilization` (**A-2**),
  clamp 0–100, return `(u7, u5)`; `None` on HTTPError/URLError/KeyError/ValueError with
  a `log.warning` (raw body logged only on parse failure, for schema-drift debugging).

### Main-loop integration (AC-9, AC-10)

Before the loop: `last_usage_poll = 0.0; usage_fail_count = 0; last_usage_sent = None`.
`last_usage_poll = 0.0` makes the first poll fire immediately (daemon restart heals fast).

Inside the loop, after the state-transition block, before `asyncio.sleep(0.2)`:

```python
now_m = time.monotonic()
if current_state and current_state != "off" and now_m - last_usage_poll >= USAGE_POLL_INTERVAL:
    last_usage_poll = now_m
    usage = await fetch_usage()
    if usage is None:
        usage_fail_count += 1
        if usage_fail_count >= USAGE_FAIL_CLEAR_THRESHOLD and last_usage_sent is not None:
            try:
                if client.is_connected:
                    await send(client, "usage -")
                last_usage_sent = None
            except Exception as e:
                log.error("usage clear send error: %s", e)
    else:
        usage_fail_count = 0
        try:
            if client.is_connected:
                await send(client, f"usage {usage[0]},{usage[1]}")
                last_usage_sent = usage
        except Exception as e:
            log.error("usage send error: %s", e)
```

Always resending each cycle (not only on change) transparently heals a firmware reboot
within ≤60 s (AC-9) with zero reboot-detection logic.

### `--once` test flag (AC-12)

In `__main__`, before any lock/PID/BLE work:

```python
if "--once" in sys.argv:
    print("usage:", asyncio.run(fetch_usage()))
    sys.exit(0)
```

### Untouched

`VALID_STATES`, `read_state()`, the state file, PID/lock handling, idle timeout,
reconnect backoff, `claude_lamp_hook.sh`, `settings.json` hook entries.

## Data / API / UI changes

- **BLE API**: additive command `usage P7,P5` / `usage -`; old firmware ignores it via
  the existing unknown-command fallthrough, old daemons simply never send it.
- **External API**: unofficial `GET /api/oauth/usage` (A-2, R-1). Read-only, 1 req/min.
- **UI (LEDs)**: status area shrinks from 30 → 20 LEDs; bars occupy the top 10.

## Migration / deployment

No persistent data; nothing to migrate. Firmware and daemon must ship together
(`make upload`, then copy `claude_lamp_daemon.py` to `~/.claude/claude_lamp_hooks/` and
kill the running daemon so the hook respawns the new one). Mixed versions degrade
gracefully: new daemon + old firmware → unknown command logged on serial, bars dark;
old daemon + new firmware → bars dark.

## Security

- OAuth access token read from Keychain at fetch time only; held in memory, never
  logged, never written to disk (AC-13).
- Raw response body is logged **only** on parse failure; it contains usage percentages,
  no credentials.
- `security` invoked with a fixed argv (no shell interpolation).
- First keychain access from the venv python may show a macOS consent prompt (R-3);
  failure mode is dark bars, never a crash. Document "Always Allow" in README.

## Observability

- Existing `/tmp/claude_lamp_daemon.log`: TX lines already log every BLE command
  (`TX usage 73,12`); new `log.warning` lines for keychain/HTTP/parse failures,
  `log.error` for send failures.
- Firmware serial (115200, `make monitor`): existing `RX command:` echo covers `usage`;
  explicit message on malformed syntax.

## Test strategy

1. **Compile**: `make compile` after each firmware task — zero warnings tolerated for
   the `sscanf`/`constrain` additions.
2. **BLE smoke test** (README's manual write procedure, RX `6e400002-…`):
   `working` → `usage 0,0` (both gauges 5× green) → `usage 73,12` (7d: 1 full + 1 at
   35 %; 5h: 4 full + 1 at 40 %, greenish) → `usage 100,0` (7d single blinking red,
   5h full green) → `usage 95,94` (7d blinks, 5h: 1 LED at 30 %, steady) → `usage -` →
   `idle` → `bright 200` → `off`. Verify AC-1…AC-7 + AC-14: gauges drain as usage
   rises, persist over animations without flicker, status confined to LEDs 1–20,
   ≥95 % shows one blinking red LED, `off` darkens everything.
3. **Live validation gate (T4)**: dump keychain JSON and curl the endpoint manually to
   confirm A-1/A-2 **before** writing daemon code; update this design if schemas differ.
4. **Daemon unit-ish**: `claude_hooks/venv/bin/python3 claude_lamp_daemon.py --once` →
   `(u7, u5)`; failure paths by temporary bogus `KEYCHAIN_ITEM` / network off → `None` +
   log line.
5. **Hardware end-to-end**: run a Claude Code session; bars appear within seconds of
   daemon start, refresh ≤60 s, survive ESP32 reset button (reappear ≤60 s), idle
   timeout still sends `off`.
