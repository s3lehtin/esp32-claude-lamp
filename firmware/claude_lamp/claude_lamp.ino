/*
 * Claude Lamp — ESP32 + WS2812B status indicator for Claude Code.
 *
 * BLE peripheral exposing the Nordic UART Service (NUS). The macOS daemon
 * (claude_hooks/claude_lamp_daemon.py) writes ASCII state commands to the
 * RX characteristic; all animation is rendered locally on the ESP32.
 *
 * Commands (ASCII, optional trailing newline):
 *   working          slow breathing white <-> navy
 *   idle             solid warm amber
 *   input            gentle purple pulse
 *   off              all LEDs dark
 *   color R,G,B      solid color (0-255 each)
 *   bright N         global brightness cap (0-255)
 *   usage P7,P5      token utilization fuel gauges: 7-day on LEDs 21-25,
 *                    5-hour on LEDs 26-30. P = used % (0-100); the bar shows
 *                    what REMAINS (0% used = 5 green LEDs, drains toward red;
 *                    >=95% used = one red LED blinking)
 *   usage -          clear both usage bars (unknown -> dark)
 *
 * Status animations use LEDs 1-20; usage bars persist across all states
 * except "off".
 *
 * Board: classic ESP32 DevKit (esp32:esp32:esp32)
 * Libraries: Adafruit NeoPixel, NimBLE-Arduino (2.x API)
 */

#include <Adafruit_NeoPixel.h>
#include <NimBLEDevice.h>

// ---------- Configuration ----------
#define LED_PIN        16            // WS2812B data pin
#define LED_COUNT      30            // number of pixels on the strip
#define DEFAULT_BRIGHTNESS 80        // 0-255; conservative cap for USB power
#define DEVICE_NAME    "CLAUDE-LAMP"

// Nordic UART Service — same UUIDs the Moonside daemon already targets.
#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // central writes here
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // notify (unused for now)

#define FRAME_INTERVAL_MS 20         // ~50 fps animation tick

// Strip partition: status animation on 0..19, usage bargraphs on 20..29.
#define STATUS_LEDS    20            // indices 0..19: status animation
#define BAR7_START     20            // 7-day usage bar: indices 20..24
#define BAR5_START     25            // 5-hour usage bar: indices 25..29
#define BAR_LEN         5
#define BLINK_THRESHOLD 95           // util >= this -> bar blinks (imminent limit)
#define BLINK_PERIOD_MS 1000         // ~1 Hz: 500 ms on, 500 ms off

// ---------- State ----------
enum LampState : uint8_t {
  STATE_OFF,
  STATE_WORKING,
  STATE_IDLE,
  STATE_INPUT,
  STATE_COLOR,   // static color set via "color R,G,B"
};

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

volatile LampState lampState = STATE_OFF;
volatile uint8_t customR = 0, customG = 0, customB = 0;
volatile int16_t util7 = -1;         // 7-day utilization 0..100, -1 = unknown (bar dark)
volatile int16_t util5 = -1;         // 5-hour utilization 0..100, -1 = unknown
volatile bool forceRender = false;   // set by "bright" to repaint static states

// ---------- Animation helpers ----------
static uint8_t lerp8(uint8_t a, uint8_t b, float t) {
  return (uint8_t)(a + (b - a) * t);
}

// Fill the status area only; strip.show() happens once per frame in renderFrame().
static void fillColor(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < STATUS_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
}

// working: breathe between white and navy, 4 s period
static void animWorking(uint32_t now) {
  float t = (sinf(now * (2.0f * PI / 4000.0f)) + 1.0f) * 0.5f;
  fillColor(lerp8(0, 255, t), lerp8(0, 255, t), lerp8(80, 255, t));
}

// input: purple pulse between 30% and 100%, 2 s period, never fully dark
static void animInput(uint32_t now) {
  float t = (sinf(now * (2.0f * PI / 2000.0f)) + 1.0f) * 0.5f;  // 0..1
  float level = 0.3f + 0.7f * t;
  fillColor((uint8_t)(180 * level), 0, (uint8_t)(255 * level));
}

// ---------- Usage bargraphs (indices 20..29) ----------
// Map utilization 0..100 to green -> yellow -> red; whole bar one hue.
static uint32_t barColor(int u) {
  u = constrain(u, 0, 100);
  uint8_t r, g;
  if (u <= 50) { r = (uint8_t)(255 * u / 50);         g = 255; }  // green -> yellow
  else         { r = 255; g = (uint8_t)(255 * (100 - u) / 50); }  // yellow -> red
  return strip.Color(r, g, 0);
}

// Fuel gauge: paint REMAINING budget (100-util) — full at 0% usage, drains as
// usage grows; hue keyed to usage via barColor(). util -1 -> all dark (unknown).
// util >= BLINK_THRESHOLD -> alarm: one full red LED blinking ~1 Hz, so an
// exhausted budget never looks like "no data".
static void renderBar(int start, int util, uint32_t now) {
  if (util < 0) {                                   // unknown -> all dark
    for (int i = 0; i < BAR_LEN; i++) strip.setPixelColor(start + i, 0);
    return;
  }
  if (util >= BLINK_THRESHOLD) {                    // imminent-limit alarm
    bool on = (now % BLINK_PERIOD_MS) < BLINK_PERIOD_MS / 2;
    strip.setPixelColor(start, on ? strip.Color(255, 0, 0) : 0);
    for (int i = 1; i < BAR_LEN; i++) strip.setPixelColor(start + i, 0);
    return;
  }
  int remaining = 100 - util;                       // fuel left
  uint32_t col = barColor(util);
  for (int i = 0; i < BAR_LEN; i++) {
    int px = start + i, lo = i * 20, hi = lo + 20;  // LED i covers [lo, hi) of remaining
    if (remaining >= hi) {
      strip.setPixelColor(px, col);                 // fully lit
    } else if (remaining <= lo) {
      strip.setPixelColor(px, 0);                   // drained
    } else {
      int frac = remaining - lo;                    // 1..19 -> proportional dim
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

static void renderFrame(uint32_t now) {
  static LampState lastRendered = STATE_OFF;
  bool entered = (lampState != lastRendered) || forceRender;
  lastRendered = lampState;
  forceRender = false;

  if (lampState == STATE_OFF) {
    strip.clear();   // all 30 dark, usage bars included
    strip.show();
    return;
  }

  switch (lampState) {
    case STATE_WORKING: animWorking(now); break;
    case STATE_INPUT:   animInput(now);   break;
    case STATE_IDLE:
      if (entered) fillColor(255, 140, 30);  // warm amber, static
      break;
    case STATE_COLOR:
      fillColor(customR, customG, customB);  // re-fill: customRGB may change
      break;
    default: break;
  }
  renderUsageBars(now);  // bars persist across all non-off states
  strip.show();          // single show per frame
}

// ---------- Command parsing ----------
static void handleCommand(const std::string &raw) {
  String cmd = String(raw.c_str());
  cmd.trim();
  cmd.toLowerCase();
  Serial.printf("RX command: '%s'\n", cmd.c_str());

  if (cmd == "working")      lampState = STATE_WORKING;
  else if (cmd == "idle")    lampState = STATE_IDLE;
  else if (cmd == "input")   lampState = STATE_INPUT;
  else if (cmd == "off")     lampState = STATE_OFF;
  else if (cmd.startsWith("color ")) {
    int r, g, b;
    if (sscanf(cmd.c_str() + 6, "%d,%d,%d", &r, &g, &b) == 3) {
      customR = constrain(r, 0, 255);
      customG = constrain(g, 0, 255);
      customB = constrain(b, 0, 255);
      lampState = STATE_COLOR;
    } else {
      Serial.println("Bad color syntax, expected: color R,G,B");
    }
  } else if (cmd.startsWith("bright ")) {
    int n;
    if (sscanf(cmd.c_str() + 7, "%d", &n) == 1) {
      strip.setBrightness(constrain(n, 0, 255));
      forceRender = true;  // repaint static states at the new brightness next tick
    }
  } else if (cmd.startsWith("usage ")) {
    // "usage P7,P5" sets the bars (0-100 each); "usage -" clears to unknown.
    // Never touches lampState — usage stays orthogonal to status.
    const char *arg = cmd.c_str() + 6;
    if (arg[0] == '-') {
      util7 = -1;
      util5 = -1;
    } else {
      int a, b;
      if (sscanf(arg, "%d,%d", &a, &b) == 2) {
        util7 = (int16_t)constrain(a, 0, 100);
        util5 = (int16_t)constrain(b, 0, 100);
      } else {
        Serial.println("Bad usage syntax, expected: usage P7,P5 or usage -");
      }
    }
  } else {
    Serial.println("Unknown command");
  }
}

// ---------- BLE callbacks (NimBLE 2.x signatures) ----------
class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
    NimBLEAttValue value = pCharacteristic->getValue();
    if (value.size() > 0) {
      handleCommand(std::string(value.c_str(), value.size()));
    }
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
    Serial.printf("Connected: %s\n", connInfo.getAddress().toString().c_str());
  }
  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override {
    Serial.printf("Disconnected (reason %d), advertising again\n", reason);
    NimBLEDevice::startAdvertising();
  }
};

// ---------- Setup / loop ----------
static void selfTest() {
  // quick wipe so a fresh flash is visibly alive
  for (int i = 0; i < LED_COUNT; i++) {
    strip.clear();
    strip.setPixelColor(i, strip.Color(120, 60, 10));
    strip.show();
    delay(15);
  }
  strip.clear();
  strip.show();
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nClaude Lamp booting...");

  strip.begin();
  strip.setBrightness(DEFAULT_BRIGHTNESS);
  selfTest();

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService *nus = server->createService(NUS_SERVICE_UUID);
  NimBLECharacteristic *rx = nus->createCharacteristic(
      NUS_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCallbacks());
  nus->createCharacteristic(NUS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  nus->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName(DEVICE_NAME);
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->start();

  Serial.println("Advertising as " DEVICE_NAME);
}

void loop() {
  static uint32_t lastFrame = 0;
  uint32_t now = millis();
  if (now - lastFrame >= FRAME_INTERVAL_MS) {
    lastFrame = now;
    renderFrame(now);
  }
  delay(1);
}
