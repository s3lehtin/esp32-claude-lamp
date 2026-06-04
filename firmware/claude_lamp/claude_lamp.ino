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

// ---------- Animation helpers ----------
static uint8_t lerp8(uint8_t a, uint8_t b, float t) {
  return (uint8_t)(a + (b - a) * t);
}

static void fillColor(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
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

static void renderFrame(uint32_t now) {
  static LampState lastRendered = STATE_OFF;
  bool entered = (lampState != lastRendered);
  lastRendered = lampState;

  switch (lampState) {
    case STATE_WORKING: animWorking(now); break;
    case STATE_INPUT:   animInput(now);   break;
    case STATE_IDLE:
      if (entered) fillColor(255, 140, 30);  // warm amber, static
      break;
    case STATE_COLOR:
      fillColor(customR, customG, customB);  // re-fill: customRGB may change
      break;
    case STATE_OFF:
      if (entered) { strip.clear(); strip.show(); }
      break;
  }
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
      // force re-render of static states at the new brightness
      if (lampState == STATE_IDLE) fillColor(255, 140, 30);
      else if (lampState == STATE_COLOR) fillColor(customR, customG, customB);
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
