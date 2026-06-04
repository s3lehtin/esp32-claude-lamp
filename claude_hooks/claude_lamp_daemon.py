#!/usr/bin/env python3
"""
Claude Lamp daemon — maintains persistent BLE connection to the ESP32 lamp
and forwards state changes written to /tmp/claude_lamp_state by
claude_lamp_hook.sh.

The firmware renders all animations locally, so each state is sent verbatim
as an ASCII command: working, idle, input, off.

Additionally polls Claude token utilization (5-hour / 7-day windows) every
60 s and forwards it as "usage P7,P5" for the bargraph LEDs. Run with --once
to do a single fetch (keychain + HTTP + parse) and exit without touching BLE.
"""

import asyncio
import fcntl
import json
import logging
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

from bleak import BleakClient, BleakScanner

LOCK_FILE = "/tmp/claude_lamp_daemon.lock"
PID_FILE = "/tmp/claude_lamp_daemon.pid"
STATE_FILE = "/tmp/claude_lamp_state"
LOG_FILE = "/tmp/claude_lamp_daemon.log"

NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NAME_PREFIX = "CLAUDE-LAMP"

IDLE_TIMEOUT = 30 * 60  # 30 minutes

USAGE_POLL_INTERVAL = 180         # baseline seconds between utilization polls
USAGE_BACKOFF_MAX = 1800          # cap for HTTP 429 exponential backoff
USAGE_FAIL_CLEAR_THRESHOLD = 3    # consecutive hard failures -> clear bars ("usage -")
USAGE_CACHE_FILE = "/tmp/claude_lamp_usage_cache.json"
USAGE_CACHE_TTL = 300             # 5 min: the API is queried at most this often
KEYCHAIN_ITEM = "Claude Code-credentials"
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
OAUTH_BETA = "oauth-2025-04-20"

VALID_STATES = {"working", "idle", "input", "off"}

logging.basicConfig(
    filename=LOG_FILE,
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("claude_lamp")


async def discover_lamp(timeout: float = 10.0):
    """Scan for a device whose name starts with CLAUDE-LAMP, return BLEDevice."""
    log.info("Scanning for device named %s*...", NAME_PREFIX)
    devices = await BleakScanner.discover(timeout=timeout)
    for d in devices:
        name = d.name or ""
        if name.upper().startswith(NAME_PREFIX):
            log.info("Found: %s (%s)", name, d.address)
            return d
    log.error("No device named %s* found", NAME_PREFIX)
    sys.exit(1)


async def send(client: BleakClient, cmd: str):
    log.info("TX %s", cmd)
    await client.write_gatt_char(NUS_RX_UUID, cmd.encode("utf-8"), response=True)


async def connect_with_retry(device) -> tuple[BleakClient, object]:
    """Returns (client, device). Re-discovers by name if retries fail."""
    delays = [1, 2, 4, 8, 16]
    for delay in delays:
        try:
            client = BleakClient(device, timeout=15.0)
            await client.connect()
            if client.is_connected:
                log.info("Connected to %s", device.name or device.address)
                return client, device
        except Exception as e:
            log.warning("Connect failed: %s, retry in %ds", e, delay)
            await asyncio.sleep(delay)

    log.warning("All retries exhausted, re-scanning...")
    device = await discover_lamp()
    client = BleakClient(device, timeout=15.0)
    await client.connect()
    return client, device


# ---------- Token utilization fetch ----------
def read_access_token() -> str | None:
    """Return the Claude Code OAuth access token from the Keychain, or None.

    Never logs the token itself.
    """
    try:
        out = subprocess.run(
            ["security", "find-generic-password", "-s", KEYCHAIN_ITEM, "-w"],
            capture_output=True, text=True, timeout=10,
        )
        if out.returncode != 0:
            log.warning("keychain read failed rc=%s: %s",
                        out.returncode, out.stderr.strip())
            return None
        oauth = json.loads(out.stdout).get("claudeAiOauth") or {}
        token = oauth.get("accessToken")
        exp = oauth.get("expiresAt")  # epoch ms
        if exp is not None and float(exp) <= time.time() * 1000:
            log.warning("oauth token expired (expiresAt=%s)", exp)
            return None
        return token
    except Exception as e:
        log.warning("keychain/JSON error: %s", e)
        return None


def read_usage_cache() -> tuple[int, int] | None:
    """Return cached (u7, u5) if younger than USAGE_CACHE_TTL, else None."""
    try:
        with open(USAGE_CACHE_FILE) as f:
            d = json.load(f)
        age = time.time() - float(d["ts"])
        if 0 <= age < USAGE_CACHE_TTL:
            return int(d["u7"]), int(d["u5"])
    except Exception:
        pass
    return None


def write_usage_cache(u7: int, u5: int):
    try:
        with open(USAGE_CACHE_FILE, "w") as f:
            json.dump({"ts": time.time(), "u7": u7, "u5": u5}, f)
    except Exception as e:
        log.warning("usage cache write error: %s", e)


def _fetch_usage_blocking(token: str) -> dict:
    req = urllib.request.Request(
        USAGE_URL,
        headers={
            "Authorization": f"Bearer {token}",
            "anthropic-beta": OAUTH_BETA,
            "Accept": "application/json",
        },
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8"))


async def fetch_usage() -> tuple[str, object]:
    """Fetch utilization. Returns one of:
      ("ok", (util_7d, util_5h))        ints 0..100
      ("rate_limited", retry_after)     int seconds or None — last value still valid
      ("error", None)                   hard failure (token/network/parse)

    Served from the 5-min disk cache when fresh — daemon restarts within the
    TTL never hit the API.
    """
    cached = read_usage_cache()
    if cached is not None:
        log.info("usage cache hit: %s", cached)
        return ("ok", cached)
    token = read_access_token()
    if not token:
        return ("error", None)
    try:
        body = await asyncio.to_thread(_fetch_usage_blocking, token)
    except urllib.error.HTTPError as e:
        if e.code == 429:
            retry_after = None
            try:
                retry_after = int(e.headers.get("Retry-After", ""))
            except (TypeError, ValueError):
                pass
            log.warning("usage HTTP 429 (Retry-After=%s)", retry_after)
            return ("rate_limited", retry_after)
        log.warning("usage HTTP %s", e.code)  # 401 -> token bad; 5xx -> server
        return ("error", None)
    except Exception as e:
        log.warning("usage fetch error: %s", e)  # network down, timeout, ...
        return ("error", None)
    try:
        u7 = int(round(float(body["seven_day"]["utilization"])))
        u5 = int(round(float(body["five_hour"]["utilization"])))
    except (KeyError, TypeError, ValueError) as e:
        log.warning("usage parse error: %s body=%s", e, body)
        return ("error", None)
    u7, u5 = max(0, min(100, u7)), max(0, min(100, u5))
    write_usage_cache(u7, u5)
    return ("ok", (u7, u5))


def read_state() -> str:
    try:
        with open(STATE_FILE) as f:
            return f.read().strip()
    except FileNotFoundError:
        return ""


def cleanup():
    try:
        os.unlink(PID_FILE)
    except OSError:
        pass


async def main():
    # Ensure only one daemon runs at a time
    lock_fp = open(LOCK_FILE, "w")
    try:
        fcntl.flock(lock_fp, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        log.info("Another daemon already running, exiting")
        sys.exit(0)

    device = await discover_lamp()

    # Write PID
    with open(PID_FILE, "w") as f:
        f.write(str(os.getpid()))

    shutdown = asyncio.Event()

    def handle_sigterm(*_):
        shutdown.set()

    signal.signal(signal.SIGTERM, handle_sigterm)
    signal.signal(signal.SIGINT, handle_sigterm)

    client, device = await connect_with_retry(device)

    current_state = ""
    idle_since: float | None = None

    next_usage_poll = 0.0  # monotonic deadline; 0 -> first fetch fires immediately
    usage_backoff = USAGE_POLL_INTERVAL  # grows on HTTP 429, resets on success
    usage_fail_count = 0
    last_usage_sent: tuple[int, int] | None = None

    try:
        while not shutdown.is_set():
            desired = read_state()

            # State transition
            if desired != current_state and desired in VALID_STATES:
                log.info("State: %s -> %s", current_state or "(none)", desired)
                current_state = desired
                idle_since = None

                try:
                    if not client.is_connected:
                        client, device = await connect_with_retry(device)

                    await send(client, current_state)

                    if current_state == "idle":
                        idle_since = time.monotonic()
                    elif current_state == "off":
                        break
                except Exception as e:
                    log.error("BLE send error on transition: %s", e)
                    current_state = ""  # retry the transition next tick
                    try:
                        client, device = await connect_with_retry(device)
                    except Exception:
                        pass

            # Usage poll: every USAGE_POLL_INTERVAL while a non-off state is
            # active. Always re-sent (not only on change) so a rebooted ESP32
            # self-heals within one interval. Runs on this loop -> BLE writes
            # stay serialized; HTTP happens off-thread via asyncio.to_thread.
            # HTTP 429 backs off exponentially (honoring Retry-After) and keeps
            # the last value on the lamp — a rate limit is not stale data.
            now_m = time.monotonic()
            if (current_state and current_state != "off"
                    and now_m >= next_usage_poll):
                status, payload = await fetch_usage()
                if status == "ok":
                    usage_fail_count = 0
                    usage_backoff = USAGE_POLL_INTERVAL
                    next_usage_poll = now_m + USAGE_POLL_INTERVAL
                    try:
                        if client.is_connected:
                            await send(client, f"usage {payload[0]},{payload[1]}")
                            last_usage_sent = payload
                    except Exception as e:
                        log.error("usage send error: %s", e)
                elif status == "rate_limited":
                    delay = max(payload or 0, usage_backoff)
                    usage_backoff = min(usage_backoff * 2, USAGE_BACKOFF_MAX)
                    next_usage_poll = now_m + delay
                    log.info("usage poll backed off %ds", delay)
                else:  # hard failure: token/network/parse
                    usage_fail_count += 1
                    next_usage_poll = now_m + USAGE_POLL_INTERVAL
                    if (usage_fail_count >= USAGE_FAIL_CLEAR_THRESHOLD
                            and last_usage_sent is not None):
                        try:
                            if client.is_connected:
                                await send(client, "usage -")
                            last_usage_sent = None
                        except Exception as e:
                            log.error("usage clear send error: %s", e)

            # Idle timeout
            if current_state == "idle" and idle_since is not None:
                if time.monotonic() - idle_since >= IDLE_TIMEOUT:
                    log.info("Idle timeout reached, shutting down")
                    try:
                        if client.is_connected:
                            await send(client, "off")
                    except Exception:
                        pass
                    break

            await asyncio.sleep(0.2)

    finally:
        # Graceful shutdown
        try:
            if client.is_connected:
                await send(client, "off")
                await asyncio.sleep(0.1)
                await client.disconnect()
        except Exception as e:
            log.warning("Shutdown disconnect error: %s", e)
        cleanup()
        log.info("Daemon exited")


if __name__ == "__main__":
    if "--once" in sys.argv:
        # Single usage fetch for testing: no BLE, no lock/PID files.
        print("usage:", asyncio.run(fetch_usage()))
        sys.exit(0)
    asyncio.run(main())
