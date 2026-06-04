# Requirements: Token Usage Bars

Display Claude token utilization as two persistent 5-LED bargraphs on the 30-LED strip:
physical LEDs 21–25 (strip indices 20–24) show the **7-day window** utilization, physical
LEDs 26–30 (strip indices 25–29) show the **5-hour window** utilization.

## User stories

- **US-1**: As a Claude Code user, I want the lamp to show my current 5-hour and 7-day
  token utilization at a glance, so I can pace my usage without opening `/usage`.
- **US-2**: As a user, I want the usage bars visible regardless of the lamp's status
  animation (working/idle/input), so utilization is always readable while I work.
- **US-3**: As a user, I want stale or unavailable usage data to be shown as dark bars
  rather than a frozen old value, so I never act on misleading information.

## Acceptance criteria

### LED layout & rendering (firmware)

- **AC-1**: Strip indices 20–24 render the 7-day bar; indices 25–29 render the 5-hour bar.
  Status animations (working/idle/input/color) touch only indices 0–19.
- **AC-2** *(fuel gauge — revised 2026-06-04)*: Each bar LED represents 20 percentage
  points of **remaining** budget `r = 100 − u`. LEDs whose full 20%-segment is below `r`
  are fully lit; the LED containing `r` is dimmed proportionally (`(r mod 20)/20` of full
  color); LEDs above `r` are dark — the bar *drains* as usage grows.
  Examples: `u=0` → all 5 lit; `u=73` (r=27) → 1 full + 1 at 35% + 3 dark;
  `u=80` (r=20) → 1 full; `u≥95` → see AC-14.
- **AC-3**: Bar color ramps green → yellow → red with the bar's own **utilization**:
  green at 0%, yellow at 50%, red at 100% (linear integer interpolation, whole bar one hue).
  A full gauge is green; a draining gauge turns yellow then red.
- **AC-4**: Bars persist unchanged across all status states and animation frames
  (no flicker from status animation `show()` calls). In `off` state all 30 LEDs are dark.
- **AC-5**: Before any usage data arrives (firmware boot default) and after an explicit
  clear, both bars are dark. Internal sentinel: utilization `-1` = unknown. With
  fuel-gauge semantics this stays unambiguous: exhausted (100%) shows a blinking red LED
  (AC-14), never full dark.
- **AC-6**: New BLE command `usage P7,P5` (ASCII, e.g. `usage 73,12`) sets both bars
  atomically; `usage -` clears both to unknown. The command does not change `lampState`.
  Values are clamped to 0–100; malformed input is ignored with a serial log line.
- **AC-7**: `bright N` continues to scale the whole strip (status + bars) and takes
  visual effect within one animation tick in every state. Existing commands
  (`working`/`idle`/`input`/`off`/`color R,G,B`) behave exactly as before on indices 0–19.
- **AC-14** *(revised 2026-06-04)*: When a bar's utilization is ≥ 95 % (imminent limit),
  that bar shows **exactly one full-brightness red LED blinking at ~1 Hz** (500 ms on,
  500 ms off), overriding the proportional fill. Each bar blinks independently; the
  other bar and the status animation are unaffected. Below 95 % the bar is steady.

### Data fetch & refresh (daemon)

- **AC-8**: The daemon fetches utilization from the Claude Code OAuth usage endpoint
  using the access token from the macOS Keychain item `Claude Code-credentials`
  *(see Assumptions)* and sends `usage P7,P5` over BLE.
- **AC-9** *(revised 2026-06-04: 60 s → 180 s after observing HTTP 429)*: First fetch
  happens immediately on daemon start; thereafter every 180 s while the daemon runs and
  the lamp state is not `off`. The value is re-sent every cycle even if unchanged
  (heals firmware reboots within one interval).
- **AC-10**: On a hard fetch failure (keychain missing, token expired, HTTP 401/5xx,
  network down, schema mismatch) the last shown value is kept; after **3 consecutive**
  hard failures the daemon sends `usage -` to clear the bars. A success resets the
  counter.
- **AC-15** *(added 2026-06-04)*: HTTP 429 is **not** a hard failure: the last value
  stays on the lamp (never cleared because of rate limiting), and the next poll backs
  off exponentially — `max(Retry-After, backoff)` with backoff doubling from 180 s up
  to 1800 s, resetting to 180 s on the next success.
- **AC-11**: Usage polling never blocks or races the existing 200 ms state loop: BLE
  writes remain serialized on the single client; HTTP runs off the event loop thread.
- **AC-12**: `claude_lamp_daemon.py --once` performs one fetch (keychain + HTTP + parse),
  prints the result, and exits without touching BLE, the lock file, or the PID file.
- **AC-13**: Errors are logged to `/tmp/claude_lamp_daemon.log`; the access token value
  is never logged or written to disk.

## Edge cases

- Utilization > 100, negative, or non-numeric → clamped/treated as parse failure
  (daemon clamps 0–100; firmware `constrain`s again).
- Missing `five_hour`/`seven_day`/`utilization` fields in the response → fetch failure (AC-10).
- Firmware reboot (usage state lost, bars dark) → healed by unconditional 60 s resend (AC-9).
- Daemon restart → immediate first poll repaints bars promptly (AC-9).
- Keychain item absent / `expiresAt` in the past / HTTP 401 → fetch failure path (AC-10).
- macOS Keychain GUI permission prompt on first access from the venv python → fetch
  failure until the user clicks "Always Allow" (documented, never crashes the daemon).
- `off` while bars lit → everything dark; polling stops (daemon loop exits on `off`).
- `usage` command received in `off` state → values stored, bars appear on next non-off state.

## Non-goals

- Rendering overage / `extra_usage` indicators or `resets_at` countdowns.
- Supporting hosts other than macOS (keychain-based token retrieval is macOS-only).
- Per-model or per-project usage breakdown; opus/sonnet split.
- Estimating usage from local transcript JSONL (ccusage-style) as a fallback.
- Changing hook events, `claude_lamp_hook.sh`, or the state-file protocol.

## Risks

- **R-1 (high)**: `GET https://api.anthropic.com/api/oauth/usage` with
  `anthropic-beta: oauth-2025-04-20` is an **unofficial, undocumented endpoint**; the
  endpoint or response schema may change or disappear. Mitigated by AC-10 fail-soft
  behavior and a live validation gate before implementation (tasks T4).
  *Observed 2026-06-04: HTTP 429 after ~30 min of 60 s polling — hence AC-15 backoff.*
- **R-2 (medium)**: Keychain JSON field names (`claudeAiOauth.accessToken`,
  `expiresAt` ms-epoch) are assumed, not documented. Validated in T4.
- **R-3 (medium)**: `security find-generic-password` from a `nohup`-spawned venv python
  may trigger a GUI consent prompt or fail when the keychain is locked.
- **R-4 (low)**: Single `strip.show()` per frame refactor touches every render path;
  a missed caller could leave a state visually stale (covered by BLE smoke test).

## Assumptions (to validate during implementation — T4 gate)

- **A-1**: Keychain item `Claude Code-credentials` contains JSON with
  `claudeAiOauth.accessToken` (string) and `claudeAiOauth.expiresAt` (epoch **ms**).
- **A-2**: `GET https://api.anthropic.com/api/oauth/usage` with headers
  `Authorization: Bearer <token>` and `anthropic-beta: oauth-2025-04-20` returns JSON
  containing `five_hour.utilization` and `seven_day.utilization` as numbers 0–100.
- **A-3**: "LEDs 21–25 / 26–30" in the feature request are 1-based physical positions
  (strip indices 20–24 / 25–29). *(Confirmed with user.)*

## Open questions

- **Q-1**: ~~Should bars blink when utilization ≥ ~95%?~~ **Resolved (user, 2026-06-04):
  yes — bar blinks at ~1 Hz when its utilization ≥ 95 % (AC-14).**
- **Q-2**: Should `resets_at` ever be surfaced (e.g. pulsing near reset)? Out of scope now.
- **Q-3**: If the OAuth endpoint breaks permanently, is a ccusage-style local estimate an
  acceptable fallback? Out of scope now.
