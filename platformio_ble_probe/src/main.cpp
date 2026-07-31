/*
 * BLE throughput probe — v2: real CFG protocol, no demo loop.
 *
 * v1 result: plain notify() succeeded 60/60 in an isolated burst, so
 * the ESP32-C3 + NimBLE-Arduino combo is NOT fundamentally broken.
 * indicate() got nacked 60/60 (likely because Web Bluetooth's
 * startNotifications() only enables the notify CCCD bit even on a
 * dual-property characteristic — unrelated to buffer exhaustion).
 *
 * This version adds back the REAL GETCFG/CFGBEGIN/CFG/CFGEND framing
 * and the actual layout content from the main firmware, using plain
 * notify() (no indicate, no wait-for-ack), but still WITHOUT the demo
 * output loop. If the real bit-rxy web app can now load this layout
 * successfully, the demo loop was the actual culprit all along; if it
 * still fails, the issue is elsewhere (payload content/size, framing).
 *
 * HOW TO USE: upload, then connect from https://abourdim.github.io/bit-rxy/
 * exactly as normal — this speaks the real protocol now, so the app's
 * loading overlay should behave like the real firmware.
 */

#include <Arduino.h>
#include <NimBLEDevice.h>

#define UART_SERVICE_UUID   "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define UART_TX_CHAR_UUID   "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define UART_RX_CHAR_UUID   "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// Same layout content as the main firmware's LAYOUT_CFG_BASE64.
static const char* LAYOUT_CFG_BASE64 =
  "eyJ0aXRsZSI6IlN1cGVyIERlbW8gUmVtb3RlIiwid2lkZ2V0cyI6W3siaWQiOiJzbGlkZXIzNiIsInQiOiJzbGlkZXIiLCJ4IjoyMCwieSI6MjAsInciOjkwLCJoIjoxODAsImxhYmVsIjoiQXJtIDEiLCJtb2RlbCI6InRyYWNrIiwibWluIjowLCJtYXgiOjEwMCwic3RlcCI6MX0seyJpZCI6InNsaWRlcjM3IiwidCI6InNsaWRlciIsIngiOjEzMCwieSI6MjAsInciOjkwLCJoIjoxODAsImxhYmVsIjoiQXJtIDIiLCJtb2RlbCI6InRyYWNrIiwibWluIjowLCJtYXgiOjEwMCwic3RlcCI6MX0seyJpZCI6InNsaWRlcjM4IiwidCI6InNsaWRlciIsIngiOjI0MCwieSI6MjAsInciOjkwLCJoIjoxODAsImxhYmVsIjoiQXJtIDMiLCJtb2RlbCI6InRyYWNrIiwibWluIjowLCJtYXgiOjEwMCwic3RlcCI6MX0seyJpZCI6InRvZ2dsZTM5IiwidCI6InRvZ2dsZSIsIngiOjMzOCwieSI6ODAsInciOjkwLCJoIjo5MCwibGFiZWwiOiJHcmlwIiwibW9kZWwiOiJzcXVhcmUifSx7ImlkIjoiZ2F1Z2U0MCIsInQiOiJnYXVnZSIsIngiOjQ5MiwieSI6NDIsInciOjE0MCwiaCI6MTYwLCJsYWJlbCI6IiIsIm1vZGVsIjoiY2xhc3NpYyIsIm1pbiI6MCwibWF4IjoxMDAsImRlY2ltYWxzIjoxLCJ1bml0cyI6IiIsIndhcm4iOm51bGwsImRhbmdlciI6bnVsbH0seyJpZCI6ImdhdWdlNDEiLCJ0IjoiZ2F1Z2UiLCJ4Ijo2NjksInkiOjQwLCJ3IjoxNDAsImgiOjE2MCwibGFiZWwiOiIiLCJtb2RlbCI6ImNsYXNzaWMiLCJtaW4iOjAsIm1heCI6MTAwLCJkZWNpbWFscyI6MSwidW5pdHMiOiIiLCJ3YXJuIjpudWxsLCJkYW5nZXIiOm51bGx9XX0=";

static NimBLECharacteristic* gTxChar    = nullptr;
static volatile bool         gConnected = false;
static String                gRxBuffer;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* /*server*/, NimBLEConnInfo& info) override {
    gConnected = true;
    gRxBuffer  = "";
    Serial.printf("[PROBE] Connected peer=%s\n", info.getAddress().toString().c_str());
  }
  void onDisconnect(NimBLEServer* /*server*/, NimBLEConnInfo& /*info*/, int reason) override {
    gConnected = false;
    Serial.printf("[PROBE] Disconnected (reason 0x%02x) — re-advertising\n", reason);
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo& /*info*/) override {
    Serial.printf("[PROBE] MTU negotiated: %u\n", mtu);
  }
};

static void sendLine(const String& line) {
  if (!gConnected || gTxChar == nullptr) return;
  String out = line + "\n";
  bool ok = gTxChar->notify((const uint8_t*)out.c_str(), out.length());
  if (!ok) {
    Serial.printf("[PROBE] notify() returned false for: %s\n", line.c_str());
  }
}

static void sendCfg() {
  int okCount = 0, failCount = 0;
  uint32_t t0 = millis();
  sendLine("CFGBEGIN");
  const char* p   = LAYOUT_CFG_BASE64;
  const size_t n  = strlen(p);
  const size_t CHUNK = 18;
  for (size_t i = 0; i < n; i += CHUNK) {
    String line = "CFG ";
    for (size_t j = 0; j < CHUNK && (i + j) < n; ++j) line += p[i + j];
    bool ok = gTxChar != nullptr && gTxChar->notify((const uint8_t*)(line + "\n").c_str(), line.length() + 1);
    if (ok) okCount++; else failCount++;
    delay(15);
  }
  sendLine("CFGEND");
  Serial.printf("[PROBE] sendCfg done in %lums: %d/%d chunks returned true\n",
                (unsigned long)(millis() - t0), okCount, okCount + failCount);
}

// Set from onWrite() (NimBLE's own host task), consumed from loop() (the
// main Arduino task). onWrite() must NOT call sendCfg() directly: that
// would run the whole ~900ms/60-notify burst synchronously ON the host
// task, blocking it from processing its own buffer-completion events
// for the entire burst — starving the very pool sendCfg() depends on.
// This is the actual cause of the rc=6 (ENOMEM) cascade we chased for
// hours: it wasn't MTU, indicate(), connection params, or the demo
// loop — it was running the burst from inside a BLE callback at all.
static volatile bool gGetCfgRequested = false;

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& /*info*/) override {
    std::string v = chr->getValue();
    for (size_t i = 0; i < v.size(); ++i) {
      const char c = v[i];
      if (c == '\r') continue;
      if (c == '\n') {
        if (gRxBuffer.length() > 0) {
          Serial.printf("[PROBE] RX line: %s\n", gRxBuffer.c_str());
          if (gRxBuffer == "GETCFG") gGetCfgRequested = true;
          gRxBuffer = "";
        }
      } else {
        gRxBuffer += c;
        if (gRxBuffer.length() > 256) gRxBuffer = "";
      }
    }
  }
};

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[PROBE v2] Booting — real CFG protocol, no demo loop...");

  NimBLEDevice::init("BBC micro:bit");
  NimBLEDevice::setSecurityAuth(false, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::deleteAllBonds();

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* svc = server->createService(UART_SERVICE_UUID);
  gTxChar = svc->createCharacteristic(UART_TX_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);

  NimBLECharacteristic* rxChar = svc->createCharacteristic(
              UART_RX_CHAR_UUID,
              NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName("BBC micro:bit");
  NimBLEDevice::startAdvertising();

  Serial.println("[PROBE v2] Advertising as 'BBC micro:bit' — connect from bit-rxy web app.");
}

void loop() {
  if (gGetCfgRequested) {
    gGetCfgRequested = false;
    sendCfg();  // runs on the main Arduino task, not the BLE host task
  }
  delay(50);
}
