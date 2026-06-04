#!/usr/bin/env python3
"""
Claude Lamp daemon — maintains persistent BLE connection to the ESP32 lamp
and forwards state changes written to /tmp/claude_lamp_state by
claude_lamp_hook.sh.

The firmware renders all animations locally, so each state is sent verbatim
as an ASCII command: working, idle, input, off.
"""

import asyncio
import fcntl
import logging
import os
import signal
import sys
import time

from bleak import BleakClient, BleakScanner

LOCK_FILE = "/tmp/claude_lamp_daemon.lock"
PID_FILE = "/tmp/claude_lamp_daemon.pid"
STATE_FILE = "/tmp/claude_lamp_state"
LOG_FILE = "/tmp/claude_lamp_daemon.log"

NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NAME_PREFIX = "CLAUDE-LAMP"

IDLE_TIMEOUT = 30 * 60  # 30 minutes

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
    asyncio.run(main())
