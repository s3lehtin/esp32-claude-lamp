FQBN    := esp32:esp32:esp32
SKETCH  := firmware/claude_lamp
# Auto-detect first USB serial port; override with: make upload PORT=/dev/cu.usbserial-XXXX
PORT    ?= $(firstword $(wildcard /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART /dev/cu.wchusbserial*))
BAUD    := 115200

.PHONY: setup compile upload monitor flash clean

setup:
	arduino-cli core update-index
	arduino-cli core install esp32:esp32
	arduino-cli lib install "Adafruit NeoPixel" "NimBLE-Arduino"

compile:
	arduino-cli compile --fqbn $(FQBN) $(SKETCH)

upload: compile
	@test -n "$(PORT)" || (echo "No serial port found — pass PORT=/dev/cu.…"; exit 1)
	arduino-cli upload --fqbn $(FQBN) -p $(PORT) $(SKETCH)

monitor:
	@test -n "$(PORT)" || (echo "No serial port found — pass PORT=/dev/cu.…"; exit 1)
	arduino-cli monitor -p $(PORT) --config baudrate=$(BAUD)

flash: upload monitor

clean:
	arduino-cli cache clean
