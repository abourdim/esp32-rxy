/*
 * Micro:bit Remote Builder — ESP32 firmware
 *
 * Speaks the same BLE protocol as a BBC micro:bit running the rxy
 * MakeCode template, so the unmodified web app at
 *   https://abourdim.github.io/rxy/
 * can connect to an ESP32 and drive widgets exactly as it would a micro:bit.
 *
 * Target board: ESP32-C3 Super Mini (any ESP32 family chip with BLE works).
 * Library:      NimBLE-Arduino (Library Manager → search "NimBLE-Arduino").
 *               Tested against NimBLE-Arduino 2.x.
 *
 * HOW TO USE
 * ----------
 *  1. Open the rxy web app, switch to the "Build" tab, design your remote.
 *  2. Click "📄 Code". In the generated MakeCode, find the line
 *        const CFG = "..."
 *     Copy the long base64 string.
 *  3. Paste it into LAYOUT_CFG_BASE64 below, replacing the default.
 *  4. Upload to the ESP32, then in the rxy "Play" tab click "📡 Connect"
 *     and pick "BBC micro:bit ESP32" from the chooser. Every widget type
 *     already has a working demo built into handleWidget() below (they
 *     all drive the on-board LED and print to Serial), so try your
 *     layout first — no C++ required yet.
 *  5. Once you wire up real hardware, replace the matching demo in
 *     handleWidget() with your own logic.
 *
 * PROTOCOL (reverse-audited from rxy script.js)
 * ---------------------------------------------
 *  Service:    6e400001-b5a3-f393-e0a9-e50e24dcca9e
 *  Notify:     6e400002-...   (device → app, the micro:bit "TX" characteristic)
 *  Write:      6e400003-...   (app → device, the micro:bit "RX" characteristic)
 *  Encoding:   ASCII lines, '\n' terminated. CRLF tolerated.
 *  App → ESP:  "SET <id> <val>"   or   "GETCFG"
 *  ESP → App:  "UPD <id> <val>"
 *              "CFGBEGIN" / "CFG <chunk>"... / "CFGEND"
 *
 *  Note: the role of characteristics 0002 / 0003 is the OPPOSITE of the
 *  Nordic UART Service convention used by Adafruit/Bluefruit. We follow the
 *  micro:bit's convention here because the web app expects it.
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_idf_version.h>

// =====================================================================
//  CONFIGURATION  —  edit this section for your project
// =====================================================================

// Advertised BLE name. MUST start with "BBC micro:bit" or the rxy app
// will not show this device in the connection chooser. The name is short
// on purpose: BLE advertisement packets are only 31 bytes, and combined
// with the 128-bit UART service UUID the budget is tight. Anything longer
// gets silently dropped by some BLE stacks. The rxy filter is a
// namePrefix match, so this exact string qualifies.
static const char* BLE_DEVICE_NAME = "BBC micro:bit";

// Paste the base64 layout string from the rxy "📄 Code" button here.
// The default below is a minimal layout with a single "Test" button.
static const char* LAYOUT_CFG_BASE64 =
  "eyJ0aXRsZSI6Ik15IFJlbW90ZSIsIndpZGdldHMiOlt7ImlkIjoiYnRuX3Rlc3"
  "QiLCJ0IjoiYnV0dG9uIiwieCI6NTAsInkiOjUwLCJ3IjoxMDAsImgiOjEwMCwi"
  "bGFiZWwiOiJUZXN0IiwibW9kZWwiOiJuZW8ifV19";

// On the ESP32-C3 Super Mini the on-board blue LED is wired to GPIO 8
// and is ACTIVE LOW (write LOW to turn it on). Adjust for your board.
static const int LED_PIN          = 8;
static const int LED_ON           = LOW;
static const int LED_OFF          = HIGH;

// Debug button — when pressed, dumps the current LAYOUT_CFG_BASE64 to
// Serial in the same "CFGBEGIN / CFG <18-char> / CFGEND" framing that
// is sent over BLE. Useful to confirm visually that the layout you
// pasted is what is actually running, without needing a BLE connect.
//
// Defaults to GPIO 9 because that is the on-board BOOT button on the
// ESP32-C3 Super Mini — no extra wiring needed. CAVEAT: GPIO 9 is also
// the C3's BOOT strapping pin. Holding this button while the board is
// powering on / resetting will boot the chip into ROM download mode
// (firmware will not run). It is safe to press AFTER boot completes.
// If you want a separate dedicated debug button without that gotcha,
// wire one between GPIO 0 and GND and change BUTTON_PIN to 0.
static const int BUTTON_PIN       = 9;
static const int BUTTON_ACTIVE    = LOW;

// =====================================================================
//  BLE UUIDs  —  DO NOT CHANGE (these are the micro:bit's UART service)
// =====================================================================
#define UART_SERVICE_UUID   "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define UART_TX_CHAR_UUID   "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // notify
#define UART_RX_CHAR_UUID   "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // write

// =====================================================================
//  State
// =====================================================================
static NimBLECharacteristic* gTxChar    = nullptr;
static volatile bool         gConnected = false;
static String                gRxBuffer;

// Forward declarations
static void handleLine(const String& line);
static void handleWidget(const String& id, const String& val);
static void sendLine(const String& line);
static void sendCfg();

/**
 * Send "UPD <id> <val>" to the app. Use this to update output widgets
 * like LEDs, labels, gauges, graphs, and battery indicators.
 */
static inline void sendValue(const String& id, const String& val) {
  sendLine("UPD " + id + " " + val);
}

// =====================================================================
//  BLE callbacks
// =====================================================================
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* /*server*/, NimBLEConnInfo& info) override {
    gConnected = true;
    gRxBuffer  = "";
    Serial.printf("[BLE] Client connected  peer=%s\n",
                  info.getAddress().toString().c_str());
  }
  void onDisconnect(NimBLEServer* /*server*/, NimBLEConnInfo& /*info*/, int reason) override {
    gConnected = false;
    Serial.printf("[BLE] Client disconnected (reason 0x%02x) — re-advertising\n", reason);
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo& /*info*/) override {
    Serial.printf("[BLE] MTU negotiated: %u\n", mtu);
  }
};

// Human-readable name for an ESP reset reason code.
static const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT (external pin)";
    case ESP_RST_SW:        return "SW (esp_restart)";
    case ESP_RST_PANIC:     return "PANIC (exception/crash)";
    case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (task watchdog)";
    case ESP_RST_WDT:       return "WDT (other watchdog)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP wakeup";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    // ESP_RST_USB / ESP_RST_JTAG were added to esp_reset_reason_t in IDF 5.0;
    // older IDF (e.g. arduino-esp32 2.0.9 on IDF 4.4) doesn't declare them.
    case ESP_RST_USB:       return "USB host reset";
    case ESP_RST_JTAG:      return "JTAG";
#endif
    default:                return "UNKNOWN";
  }
}

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& /*info*/) override {
    std::string v = chr->getValue();
    for (size_t i = 0; i < v.size(); ++i) {
      const char c = v[i];
      if (c == '\r') continue;
      if (c == '\n') {
        if (gRxBuffer.length() > 0) {
          handleLine(gRxBuffer);
          gRxBuffer = "";
        }
      } else {
        gRxBuffer += c;
        if (gRxBuffer.length() > 256) gRxBuffer = "";  // overflow guard
      }
    }
  }
};

// =====================================================================
//  Protocol
// =====================================================================
static void sendLine(const String& line) {
  if (!gConnected || gTxChar == nullptr) return;
  String out = line + "\n";
  // ATOMIC notify: pass the payload directly so each call captures its
  // own bytes. The two-step setValue() + notify() form races on the
  // characteristic's internal buffer when called back-to-back from
  // sendCfg() — NimBLE's async host task can read the buffer AFTER a
  // newer setValue has overwritten it, collapsing several queued
  // notifications into the last one. Symptom on rxy: only CFGEND
  // arrives, everything in between vanishes, JSON.parse('') fails.
  gTxChar->notify((const uint8_t*)out.c_str(), out.length());
}

static void sendCfg() {
  sendLine("CFGBEGIN");
  const char* p   = LAYOUT_CFG_BASE64;
  const size_t n  = strlen(p);
  const size_t CHUNK = 18;  // matches the official rxy MakeCode template
  for (size_t i = 0; i < n; i += CHUNK) {
    String line = "CFG ";
    for (size_t j = 0; j < CHUNK && (i + j) < n; ++j) line += p[i + j];
    sendLine(line);
    delay(15);  // pace notifications so the BLE stack does not drop any
  }
  sendLine("CFGEND");
  Serial.println("[BLE] Sent CFG");
}

// Dump every piece of state we know about — build info, runtime info,
// BLE state, and the current LAYOUT_CFG_BASE64 (in the same framing
// it is sent over BLE). Called from the button handler. A single
// press gives you a complete snapshot of the firmware at that moment.
static void printAllConfig() {
  // -------- Chip / build --------
  esp_chip_info_t chip;
  esp_chip_info(&chip);

  Serial.println();
  Serial.println("=================== STATE DUMP ===================");
  Serial.printf("[BUILD ] firmware     : Micro:bit Remote — ESP32\n");
  Serial.printf("[BUILD ] compiled     : %s %s\n", __DATE__, __TIME__);
  Serial.printf("[CHIP  ] model        : %d  cores=%u  rev=%u  features=0x%08lx\n",
                (int)chip.model, chip.cores, chip.revision,
                (unsigned long)chip.features);
  Serial.printf("[CHIP  ] sdk_version  : %s\n", esp_get_idf_version());
  Serial.printf("[CHIP  ] arduino_core : %d.%d.%d\n",
                ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR,
                ESP_ARDUINO_VERSION_PATCH);
  Serial.printf("[CHIP  ] cpu_mhz      : %lu\n",
                (unsigned long)getCpuFrequencyMhz());
  Serial.printf("[FLASH ] size         : %lu bytes\n",
                (unsigned long)ESP.getFlashChipSize());
  Serial.printf("[FLASH ] sketch       : %lu / %lu bytes (free %lu)\n",
                (unsigned long)ESP.getSketchSize(),
                (unsigned long)(ESP.getSketchSize() + ESP.getFreeSketchSpace()),
                (unsigned long)ESP.getFreeSketchSpace());

  // -------- Runtime --------
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("[RUN   ] reset_reason : %d (%s)\n", (int)rr, resetReasonStr(rr));
  Serial.printf("[RUN   ] uptime       : %lu s\n",
                (unsigned long)(millis() / 1000));
  Serial.printf("[RUN   ] free_heap    : %lu bytes\n",
                (unsigned long)ESP.getFreeHeap());
  Serial.printf("[RUN   ] min_heap     : %lu bytes\n",
                (unsigned long)ESP.getMinFreeHeap());

  // -------- GPIO --------
  Serial.printf("[GPIO  ] LED_PIN      : %d  state=%s  (active=%s)\n",
                LED_PIN,
                digitalRead(LED_PIN) == LED_ON ? "ON" : "OFF",
                LED_ON == LOW ? "LOW" : "HIGH");
  Serial.printf("[GPIO  ] BUTTON_PIN   : %d  state=%s  (active=%s)\n",
                BUTTON_PIN,
                digitalRead(BUTTON_PIN) == BUTTON_ACTIVE ? "PRESSED" : "released",
                BUTTON_ACTIVE == LOW ? "LOW" : "HIGH");

  // -------- BLE --------
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  Serial.printf("[BLE   ] device_name  : '%s'\n", BLE_DEVICE_NAME);
  Serial.printf("[BLE   ] local_mac    : %s\n",
                NimBLEDevice::getAddress().toString().c_str());
  Serial.printf("[BLE   ] service_uuid : %s\n", UART_SERVICE_UUID);
  Serial.printf("[BLE   ] tx_char_uuid : %s (notify)\n", UART_TX_CHAR_UUID);
  Serial.printf("[BLE   ] rx_char_uuid : %s (write)\n",  UART_RX_CHAR_UUID);
  Serial.printf("[BLE   ] advertising  : %s\n",
                (adv && adv->isAdvertising()) ? "YES" : "no");
  Serial.printf("[BLE   ] security     : open (no pairing required)\n");
  Serial.printf("[BLE   ] connected    : %s\n", gConnected ? "YES" : "no");
  Serial.printf("[BLE   ] rx_buf_len   : %u bytes\n",
                (unsigned)gRxBuffer.length());

  // -------- Layout CFG (same framing as BLE GETCFG reply) --------
  const size_t n = strlen(LAYOUT_CFG_BASE64);
  const size_t CHUNK = 18;
  Serial.printf("[CFG   ] total_bytes  : %lu\n", (unsigned long)n);
  Serial.printf("[CFG   ] chunk_size   : %lu\n", (unsigned long)CHUNK);
  Serial.printf("[CFG   ] chunks       : %lu\n",
                (unsigned long)((n + CHUNK - 1) / CHUNK));
  Serial.println("CFGBEGIN");
  for (size_t i = 0; i < n; i += CHUNK) {
    Serial.print("CFG ");
    for (size_t j = 0; j < CHUNK && (i + j) < n; ++j) {
      Serial.print(LAYOUT_CFG_BASE64[i + j]);
    }
    Serial.println();
  }
  Serial.println("CFGEND");

  Serial.println("==================================================");
  Serial.flush();
}

static void handleLine(const String& line) {
  Serial.print("[RX] ");
  Serial.println(line);

  if (line == "GETCFG") { sendCfg(); return; }

  if (line.startsWith("SET ")) {
    int sp = line.indexOf(' ', 4);
    if (sp < 0) return;
    String id  = line.substring(4, sp);
    String val = line.substring(sp + 1);
    handleWidget(id, val);
  }
}

// =====================================================================
//  WIDGET HANDLERS  —  edit this for your project
// =====================================================================
//
// Widget value formats (from the rxy README + audit):
//
//   Button    "btn_..."     val: "0" | "1"
//   Slider    "slider_..."  val: integer in the widget's min..max range
//   Toggle    "toggle_..."  val: "0" | "1"
//   Joystick  "joy_..."     val: "<angle 0-360> <distance 0-100>"
//                                  (angle: 0°=right, 90°=down, 180°=left, 270°=up)
//   D-Pad     "dpad_..."    val: "<up|down|left|right> <0|1>"
//   XY Pad    "xypad_..."   val: "<x 0-100> <y 0-100>"
//   Timer     "timer_..."   val: seconds elapsed (sent ~every 5 s)
//
// To update an output widget (LED, label, gauge, graph, battery)
// call sendValue() from anywhere:
//
//   sendValue("led_status",  "1");
//   sendValue("gauge_temp",  "23");
//   sendValue("label_score", "Score: 42");
//   sendValue("graph_data",  "12,7,18");
//   sendValue("battery_lvl", "75");
//
// EVERY widget type below already does something with the one actuator
// this board has out of the box (the on-board LED) and prints what it
// received to Serial. Add ANY widget of that type in the rxy Build tab
// (any id, any name) and it works immediately — no firmware edit needed.
// Once you wire up real hardware (motors, sensors, extra LEDs...),
// replace the body of the matching "if" with your own logic; the
// id/val parsing above each one can stay exactly as-is.

// Sets LED_PIN's brightness from a 0..100 percentage, accounting for the
// C3 Super Mini's on-board LED being active-LOW (0 = full brightness).
static inline void setLedPercent(int pct) {
  pct = constrain(pct, 0, 100);
  int duty = map(pct, 0, 100, 0, 255);
  analogWrite(LED_PIN, LED_ON == LOW ? 255 - duty : duty);
}

static void handleWidget(const String& id, const String& val) {

  // ------- Built-in demo: the shipped default layout's "Test" button ---
  if (id == "btn_test") {
    digitalWrite(LED_PIN, val == "1" ? LED_ON : LED_OFF);
    return;
  }

  // ------- Buttons: LED on while held, off on release -------------------
  if (id.startsWith("btn_")) {
    digitalWrite(LED_PIN, val == "1" ? LED_ON : LED_OFF);
    Serial.printf("[WIDGET] button   '%s' %s\n", id.c_str(), val == "1" ? "pressed" : "released");
    return;
  }

  // ------- Sliders: LED brightness follows the slider --------------------
  if (id.startsWith("slider_")) {
    int pct = val.toInt();               // 0..100 by default
    setLedPercent(pct);
    Serial.printf("[WIDGET] slider   '%s' = %d\n", id.c_str(), pct);
    return;
  }

  // ------- Toggles: LED on/off, stays put until toggled again -----------
  if (id.startsWith("toggle_")) {
    digitalWrite(LED_PIN, val == "1" ? LED_ON : LED_OFF);
    Serial.printf("[WIDGET] toggle   '%s' = %s\n", id.c_str(), val == "1" ? "ON" : "OFF");
    return;
  }

  // ------- Joystick: "<angle> <distance>" — LED brightens as you push ---
  if (id.startsWith("joy_")) {
    int sp    = val.indexOf(' ');
    int angle = val.substring(0, sp).toInt();
    int dist  = val.substring(sp + 1).toInt();   // 0..100, how far from center
    setLedPercent(dist);
    Serial.printf("[WIDGET] joystick '%s' angle=%d dist=%d\n", id.c_str(), angle, dist);
    return;
  }

  // ------- D-Pad: "<direction> <0|1>" — LED on while any key is held -----
  if (id.startsWith("dpad_")) {
    int sp     = val.indexOf(' ');
    String dir = val.substring(0, sp);          // "up" | "down" | "left" | "right"
    bool  down = val.substring(sp + 1) == "1";
    digitalWrite(LED_PIN, down ? LED_ON : LED_OFF);
    Serial.printf("[WIDGET] dpad     '%s' %s %s\n", id.c_str(), dir.c_str(), down ? "pressed" : "released");
    return;
  }

  // ------- XY Pad: "<x> <y>" (0..100 each) — LED brightness follows y ----
  if (id.startsWith("xypad_")) {
    int sp = val.indexOf(' ');
    int x  = val.substring(0, sp).toInt();
    int y  = val.substring(sp + 1).toInt();
    setLedPercent(y);
    Serial.printf("[WIDGET] xypad    '%s' x=%d y=%d\n", id.c_str(), x, y);
    return;
  }

  // ------- Timer: seconds elapsed — LED blinks once per tick -------------
  if (id.startsWith("timer_")) {
    digitalWrite(LED_PIN, digitalRead(LED_PIN) == LED_ON ? LED_OFF : LED_ON);
    Serial.printf("[WIDGET] timer    '%s' = %ds\n", id.c_str(), val.toInt());
    return;
  }
}

// =====================================================================
//  Setup / Loop
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(200);                                 // give USB-CDC time to enumerate
  Serial.println();
  Serial.println("=================================================");
  Serial.println("=== Micro:bit Remote — ESP32 firmware (boot) ===");
  Serial.println("=================================================");

  // ----- Boot diagnostics -----------------------------------------------
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("[BOOT] reset_reason : %d (%s)\n", (int)rr, resetReasonStr(rr));

  esp_chip_info_t chip;
  esp_chip_info(&chip);
  Serial.printf("[BOOT] chip_model   : %d  cores=%u  rev=%u  features=0x%08lx\n",
                (int)chip.model, chip.cores, chip.revision,
                (unsigned long)chip.features);
  Serial.printf("[BOOT] sdk_version  : %s\n", esp_get_idf_version());
  Serial.printf("[BOOT] arduino_core : %d.%d.%d\n",
                ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR,
                ESP_ARDUINO_VERSION_PATCH);
  Serial.printf("[BOOT] cpu_mhz      : %lu\n",
                (unsigned long)getCpuFrequencyMhz());
  Serial.printf("[BOOT] free_heap    : %lu bytes\n",
                (unsigned long)ESP.getFreeHeap());
  Serial.printf("[BOOT] min_heap     : %lu bytes\n",
                (unsigned long)ESP.getMinFreeHeap());
  Serial.printf("[BOOT] flash_size   : %lu bytes\n",
                (unsigned long)ESP.getFlashChipSize());
  Serial.printf("[BOOT] sketch_size  : %lu / %lu bytes\n",
                (unsigned long)ESP.getSketchSize(),
                (unsigned long)ESP.getFreeSketchSpace());
  Serial.flush();

  // ----- Step 1: GPIO ---------------------------------------------------
  Serial.println("[SETUP] step 1/4 — GPIO init");
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.printf("[GPIO] LED_PIN=%d  BUTTON_PIN=%d (INPUT_PULLUP, press to dump CFG)\n",
                LED_PIN, BUTTON_PIN);

  // ----- Step 2: NimBLE init -------------------------------------------
  Serial.println("[SETUP] step 2/4 — NimBLEDevice::init");
  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  // Request a larger ATT MTU. Default is 23 bytes (= 20 bytes payload
  // per notify), which is too small for our 23-byte "CFG <18-char>\n"
  // lines and would force fragmentation. Centrals negotiate downward
  // as needed (Chrome on macOS typically lands ~185, phones ~247).
  NimBLEDevice::setMTU(247);

  // Match a real micro:bit's MakeCode "No pairing required: anyone can
  // connect via Bluetooth" mode. Explicit, not relying on NimBLE/SDK
  // defaults — those can flip across versions and some BLE centrals
  // (notably macOS) treat an UNSPECIFIED security policy more strictly
  // than an explicitly-open one.
  //
  // SECURITY CAVEAT: with these settings, anyone in radio range can
  // write SET commands to this device. Fine for a desk toy / classroom
  // remote (matches what micro:bits do); WRONG for anything sensitive.
  NimBLEDevice::setSecurityAuth(false, false, false);   // no bonding / MITM / LESC
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::deleteAllBonds();                       // discard any prior pairings
  Serial.printf("[BLE] device_name   : '%s'\n", BLE_DEVICE_NAME);
  Serial.printf("[BLE] local_mac     : %s\n",
                NimBLEDevice::getAddress().toString().c_str());
  Serial.printf("[BLE] heap_after_init: %lu bytes\n",
                (unsigned long)ESP.getFreeHeap());

  // ----- Step 3: GATT service + characteristics ------------------------
  Serial.println("[SETUP] step 3/4 — GATT service setup");
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* svc = server->createService(UART_SERVICE_UUID);

  gTxChar = svc->createCharacteristic(
              UART_TX_CHAR_UUID,
              NIMBLE_PROPERTY::NOTIFY);

  NimBLECharacteristic* rxChar = svc->createCharacteristic(
              UART_RX_CHAR_UUID,
              NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();
  Serial.printf("[BLE] service_uuid  : %s\n", UART_SERVICE_UUID);
  Serial.printf("[BLE] tx_char_uuid  : %s (notify)\n", UART_TX_CHAR_UUID);
  Serial.printf("[BLE] rx_char_uuid  : %s (write)\n",  UART_RX_CHAR_UUID);

  // ----- Step 4: Advertising -------------------------------------------
  // Mimic exactly what a real micro:bit running MakeCode does:
  //   - Primary packet contains FLAGS + NAME only (no service UUID).
  //   - The Nordic UART service is exposed via GATT after connect, not
  //     announced in the advertisement. rxy / bit-playground / nRF
  //     Connect all discover it via optionalServices on the JS side.
  //   - Advertising interval pinned to 200 ms (matches MakeCode's
  //     pxt.json: "advertising_interval": 200).
  //
  // Why this matters: with our 13-char name (15 bytes) + a 128-bit
  // service UUID (18 bytes) + flags (3 bytes), the primary advertising
  // packet overflows the 31-byte BLE limit and NimBLE is forced to
  // shove the name into a scan-response packet. macOS Web Bluetooth
  // (Chrome on macOS) handles scan-response-borne names unreliably —
  // the device becomes invisible in the pair chooser even though
  // phones see it fine. Dropping the service UUID frees enough room
  // for the name to live in the primary packet, which every BLE
  // central scans reliably.
  Serial.println("[SETUP] step 4/4 — start advertising");
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(BLE_DEVICE_NAME);
  adv->enableScanResponse(false);                       // name fits in primary
  adv->setMinInterval(0x140);                           // 320 * 0.625 ms = 200 ms
  adv->setMaxInterval(0x140);
  NimBLEDevice::startAdvertising();

  Serial.printf("[BLE] advertising as: '%s'\n", BLE_DEVICE_NAME);
  Serial.printf("[BOOT] setup() OK   — free_heap=%lu bytes\n",
                (unsigned long)ESP.getFreeHeap());
  Serial.println("[BLE] Open https://abourdim.github.io/rxy/ and click Connect.");
  Serial.println("=================================================");
  Serial.flush();
}

void loop() {
  const uint32_t now = millis();

  // -----------------------------------------------------------------
  // Periodic heartbeat — confirms the firmware is alive and shows
  // connection state + heap headroom. Cheap, prints once every 5 s.
  // -----------------------------------------------------------------
  static uint32_t lastBeat = 0;
  if (now - lastBeat >= 5000) {
    lastBeat = now;
    Serial.printf("[HB] uptime=%lus  connected=%d  heap=%lu  min_heap=%lu\n",
                  (unsigned long)(now / 1000),
                  gConnected ? 1 : 0,
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMinFreeHeap());
  }

  // -----------------------------------------------------------------
  // BUTTON_PIN poll — on a clean press (active-low, debounced 30 ms),
  // dump the LAYOUT_CFG_BASE64 to Serial. Edge-triggered: only fires
  // once per press, not while held.
  // -----------------------------------------------------------------
  static int      btnLast       = HIGH;
  static uint32_t btnLastChange = 0;
  const int btnNow = digitalRead(BUTTON_PIN);
  if (btnNow != btnLast && (now - btnLastChange) > 30) {
    btnLastChange = now;
    if (btnNow == BUTTON_ACTIVE) {
      Serial.println("[BTN] press detected — dumping full state");
      printAllConfig();
    }
    btnLast = btnNow;
  }

  // -----------------------------------------------------------------
  // Periodic update example. Uncomment + adapt for your project.
  // The rxy web app rate-limits incoming messages to one per 200 ms,
  // so do not flood it. A 200–500 ms cadence is comfortable.
  // -----------------------------------------------------------------
  // static uint32_t lastUpd = 0;
  // if (gConnected && now - lastUpd > 500) {
  //   lastUpd = now;
  //   sendValue("gauge_temp", String((int)temperatureRead()));
  // }
}
