# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Project forks the [Claude Lamp](https://github.com/bobek-balinek/claude-lamp) project (local clone in `fork/`, git-ignored). Intention is to build similar concept using ESP32 and ws2812B led strip. ESP32 will be controlled over BLE from macos.  Built with `arduino-cli` (no Arduino IDE).


## Toolchain prerequisites

Already installed on this machine:
- `arduino-cli` 1.4.1 (Homebrew) with ESP32 board-manager URL configured in `~/Library/Arduino15/arduino-cli.yaml`
- ESP32 core `esp32:esp32` 3.3.8 and ESP8266 core installed
- Libraries: `Adafruit NeoPixel`, `NimBLE-Arduino` 2.x
- Homebrew python 3.14; bleak + pyserial live in `claude_hooks/venv` (system python 3.9 is too old)

## Deployment state

- Hooks are installed: scripts + venv at `~/.claude/claude_lamp_hooks/`, hook entries merged into `~/.claude/settings.json` (backup: `settings.json.bak`). The repo's `claude_hooks/settings.json` is the source of truth — re-merge after changing it.

## Build commands

- `make compile` / `make upload` / `make monitor` (FQBN `esp32:esp32:esp32`, board on `/dev/cu.usbserial-0001`)

## Gotchas

- Flashing fails ("Invalid head of packet", serial corruption) when the board is connected through the USB dock/hub — plug the CP2102 board directly into the Mac.
