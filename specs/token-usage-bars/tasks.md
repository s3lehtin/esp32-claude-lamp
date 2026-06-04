# Tasks: Token Usage Bars

Small, ordered implementation tasks. Each references acceptance criteria from
`requirements.md` and ends with verification commands. T4 is a **gate**: if the live
data contradicts assumptions A-1/A-2, update `design.md` before T5–T6.

---

## T1 — Firmware: strip partition & single-`show()` refactor

**ACs**: AC-1, AC-4 (partially), AC-7. **Files**: `firmware/claude_lamp/claude_lamp.ino`

- Add `STATUS_LEDS`/`BAR7_START`/`BAR5_START`/`BAR_LEN` defines and
  `util7`/`util5` (= -1) / `forceRender` globals.
- `fillColor()`: loop bound → `STATUS_LEDS`, remove its `strip.show()`.
- `renderFrame()`: single `strip.show()` at end; `STATE_OFF` → `strip.clear(); show(); return`.
- `bright` handler: drop inline `fillColor` calls, set `forceRender = true`;
  fold `forceRender` into the `entered` check.

**Verify**:
```sh
make compile
```
Compiles clean. (Visual check lands in T3's smoke test.)

---

## T2 — Firmware: bar rendering

**ACs**: AC-2, AC-3, AC-4, AC-5, AC-14. **Files**: `firmware/claude_lamp/claude_lamp.ino`

- Implement `barColor()` (integer green→yellow→red ramp keyed to usage),
  `renderBar(start, util, now)` rendering **remaining** budget `100−util` as a fuel
  gauge (full / fractional-dim / drained per LED, `-1` → all dark), `renderUsageBars(now)`.
- Alarm: when `util >= BLINK_THRESHOLD` (95), exactly one full red LED blinking ~1 Hz
  via `now % BLINK_PERIOD_MS` (AC-14); steady gauge below threshold.
- Call `renderUsageBars(now)` from `renderFrame()` every frame, before `show()`,
  skipped only in `STATE_OFF`.

**Verify**:
```sh
make compile
```
Desk-check the math (fuel gauge): u=0 → 5 full green; u=30 (r=70) → 3 full + 1 at 50 %;
u=73 (r=27) → 1 full + 1 at 35 %; u=80 (r=20) → 1 full; u=94 (r=6) → 1 LED at 30 %,
steady; u=95/100 → 1 red LED blinking; u=-1 → steadily dark.

---

## T3 — Firmware: `usage` BLE command + flash + smoke test

**ACs**: AC-6, AC-7, AC-1…AC-5 (visual confirmation). **Files**: `firmware/claude_lamp/claude_lamp.ino`

- Add `usage P7,P5` / `usage -` parsing branch in `handleCommand()`; clamp 0–100;
  malformed → serial message; never touches `lampState`.
- Update the header comment block with the new command.

**Verify**:
```sh
make compile
make upload        # CP2102 plugged directly into the Mac (see CLAUDE.md gotcha)
make monitor       # watch RX echoes
```
BLE smoke test (README manual-write procedure, RX `6e400002-b5a3-f393-e0a9-e50e24dcca9e`),
send in order and confirm against the AC in parentheses:
```
working            # breathing confined to LEDs 1–20 (AC-1)
usage 0,0          # both gauges full: 5 + 5 green (AC-2)
usage 73,12        # 7d: 1 full + 1 at 35%; 5h: 4 full + 1 at 40%, greenish (AC-2/AC-3); no flicker (AC-4)
usage 100,0        # 7d: one blinking red LED (AC-14), 5h full green
usage 0,100        # inverse
usage 95,94        # 7d blinks; 5h: 1 LED at 30% steady (AC-14 threshold)
usage 100,100      # both bars one blinking red LED, in sync; status animation unaffected (AC-14)
usage 999,-5       # clamps to 100,0 → 7d blinks, 5h full green (AC-6 edge, AC-14)
usage garbage      # ignored, serial message, bars unchanged (AC-6)
usage -            # both bars dark (AC-5), status animation unchanged (AC-6)
input              # bars persist over purple pulse (AC-4)
bright 200         # whole strip rescales within a tick (AC-7)
off                # all 30 dark (AC-4)
working            # bars still dark (firmware default -1, AC-5)
```

---

## T4 — GATE: validate keychain & usage endpoint live (assumptions A-1, A-2)

**ACs**: prerequisites for AC-8. **Files**: none (read-only investigation)

**Verify**:
```sh
# A-1: keychain JSON shape (do NOT paste output into the repo — contains the token)
security find-generic-password -s "Claude Code-credentials" -w | python3 -c \
  'import json,sys; d=json.load(sys.stdin)["claudeAiOauth"]; print(sorted(d.keys()), d.get("expiresAt"))'

# A-2: endpoint + schema (token used inline, never saved)
TOKEN=$(security find-generic-password -s "Claude Code-credentials" -w | python3 -c \
  'import json,sys; print(json.load(sys.stdin)["claudeAiOauth"]["accessToken"])')
curl -s https://api.anthropic.com/api/oauth/usage \
  -H "Authorization: Bearer $TOKEN" -H "anthropic-beta: oauth-2025-04-20" | python3 -m json.tool
```
Expected: keys include `accessToken`, `expiresAt` (ms epoch); response has
`five_hour.utilization` and `seven_day.utilization` as 0–100 numbers.
**If anything differs, stop and update `design.md` (and `requirements.md` A-1/A-2) first.**

---

## T5 — Daemon: fetch helpers + `--once` flag

**ACs**: AC-8, AC-12, AC-13. **Files**: `claude_hooks/claude_lamp_daemon.py`

- Add constants (`USAGE_POLL_INTERVAL`, `USAGE_FAIL_CLEAR_THRESHOLD`, `KEYCHAIN_ITEM`,
  `USAGE_URL`, `OAUTH_BETA`) and helpers `read_access_token()`,
  `_fetch_usage_blocking()`, `fetch_usage()` per design (stdlib only; token never logged).
- Add `--once` handling in `__main__` before lock/PID/BLE setup.

**Verify**:
```sh
claude_hooks/venv/bin/python3 claude_hooks/claude_lamp_daemon.py --once
# → usage: (u7, u5) matching the curl numbers from T4 (AC-8, AC-12)

# failure path: bogus item name → "usage: None" + warning in log, no token in log (AC-10, AC-13)
KEYCHAIN_ITEM_OVERRIDE_TEST=1  # temporarily edit KEYCHAIN_ITEM or test offline
grep -i "accessToken\|Bearer" /tmp/claude_lamp_daemon.log && echo "TOKEN LEAKED" || echo "log clean"
```

---

## T6 — Daemon: main-loop integration + end-to-end

**ACs**: AC-9, AC-10, AC-11. **Files**: `claude_hooks/claude_lamp_daemon.py`

- Add `next_usage_poll = 0.0` / `usage_backoff` / `usage_fail_count` / `last_usage_sent`
  before the loop; insert the deadline-gated fetch/send block (immediate first poll,
  180 s resend-always, 3-hard-failure → `usage -`) per design. No new asyncio tasks (AC-11).

**Verify**:
```sh
# deploy: copy daemon to installed hooks dir and restart it
cp claude_hooks/claude_lamp_daemon.py ~/.claude/claude_lamp_hooks/
kill "$(cat /tmp/claude_lamp_daemon.pid)" 2>/dev/null; rm -f /tmp/claude_lamp_daemon.pid
bash ~/.claude/claude_lamp_hooks/claude_lamp_hook.sh idle

tail -f /tmp/claude_lamp_daemon.log   # expect "TX usage P7,P5" within seconds (AC-9),
                                      # then one TX usage per ~180 s
```
Hardware checks: bars light within seconds of daemon start (AC-9); press ESP32 reset →
bars reappear within one interval (AC-9); turn Wi-Fi off → bars keep last value for 2
polls, dark after 3rd (AC-10), recover after Wi-Fi on; status changes remain instant
during a poll (AC-11).

---

## T8 — Daemon: HTTP 429 backoff (AC-15) *(added 2026-06-04 after observed 429s)*

**ACs**: AC-15, AC-9 (interval revision), AC-10 (429 excluded from fail counter).
**Files**: `claude_hooks/claude_lamp_daemon.py`

- `USAGE_POLL_INTERVAL` 60 → 180 s; add `USAGE_BACKOFF_MAX = 1800`.
- `fetch_usage()` → status triple; 429 returns `("rate_limited", Retry-After|None)`.
- Loop: `next_usage_poll` deadline; on 429 back off `max(Retry-After, backoff)`,
  double backoff up to cap, keep last value (never `usage -` from 429); reset on success.

**Verify**:
```sh
claude_hooks/venv/bin/python3 claude_hooks/claude_lamp_daemon.py --once  # ('ok', (u7,u5)) or ('rate_limited', N)
# deploy + restart (T6 commands), then:
grep -E "TX usage|backed off|usage HTTP" /tmp/claude_lamp_daemon.log
# expect: TX usage every ~180 s; on 429: "usage poll backed off Ns" lines with
# growing N and NO "TX usage -" caused by them
```

---

## T9 — Daemon: 5-min API cache (AC-16) *(added 2026-06-04)*

**ACs**: AC-16. **Files**: `claude_hooks/claude_lamp_daemon.py`

- `USAGE_CACHE_FILE` (/tmp JSON `{ts,u7,u5}`) + `USAGE_CACHE_TTL = 300`;
  `read_usage_cache()` / `write_usage_cache()`.
- `fetch_usage()` serves fresh cache without HTTP; writes cache on successful fetch.

**Verify**:
```sh
claude_hooks/venv/bin/python3 claude_hooks/claude_lamp_daemon.py --once   # populates cache (when not 429'd)
claude_hooks/venv/bin/python3 claude_hooks/claude_lamp_daemon.py --once   # second run: "usage cache hit" in log, same value
cat /tmp/claude_lamp_usage_cache.json                                     # {ts, u7, u5}, no token material
grep "cache hit" /tmp/claude_lamp_daemon.log
```

---

## T7 — Docs

**ACs**: documentation for AC-6, AC-8, R-3. **Files**: `README.md`, `CLAUDE.md` (if needed)

- README: add `usage P7,P5` / `usage -` to the BLE protocol table; document the LED
  layout split (1–20 status, 21–25 7d, 26–30 5h); add a "usage bars" section covering
  the unofficial endpoint caveat (R-1) and the one-time Keychain "Always Allow" prompt (R-3).
- Note redeploy step (copy daemon to `~/.claude/claude_lamp_hooks/`, restart daemon).

**Verify**:
```sh
grep -n "usage" README.md      # protocol + setup sections present
```

---

## Done criteria

All ACs verified per task; `make compile` clean; smoke test (T3) and end-to-end (T6)
pass on hardware; no token material in logs or repo.
