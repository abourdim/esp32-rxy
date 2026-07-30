# Micro:bit Remote — ESP32 firmware

ESP32 firmware that speaks the same BLE protocol as a BBC micro:bit running
the [rxy](https://abourdim.github.io/rxy/) MakeCode template. The unmodified
rxy web app can connect to an ESP32 and drive widgets exactly as it would
a micro:bit.

Target board: **ESP32-C3 Super Mini** (any ESP32 with BLE works).

## What's in this folder

```
arduino/
  esp32_rxy/
    esp32_rxy.ino                   ← open this in Arduino IDE
platformio/
  platformio.ini                   ← PlatformIO project root
  src/main.cpp                     ← identical source as the .ino
```

The two source files are **identical**; pick whichever toolchain you prefer.

## Which toolchain should I use?

On the **ESP32-C3 Super Mini** (and other boards using the C3's native
USB-CDC instead of an external USB-serial bridge), **prefer PlatformIO
for uploading**. Its `platformio.ini` pins `upload_speed = 115200` and
sets `--no-stub`, both of which are required to work around USB-CDC
re-enumeration quirks in esptool. Arduino IDE / arduino-cli has no
clean way to pass `--no-stub` through, so uploads may fail with
"No serial data received" or "Unable to verify flash chip connection".
If you must use arduino-cli, hold the BOOT button while pressing RESET
to force ROM-bootloader mode, then immediately upload.

## Arduino IDE

1. Install the **ESP32 by Espressif Systems** board package (Boards Manager).
2. Install the **NimBLE-Arduino** library (Library Manager → search "NimBLE-Arduino", version 2.x).
3. Open `arduino/esp32_rxy/esp32_rxy.ino`.
4. **Tools → Board** → "ESP32C3 Dev Module" (or your specific ESP32 board).
5. **Tools → USB CDC On Boot** → "Enabled".
6. Plug in the board, select the port, click Upload.

## PlatformIO

```bash
cd platformio
pio run -t upload
pio device monitor
```

That's it. `platformio.ini` pins the NimBLE library version and sets the
USB-CDC build flags needed by the C3 Super Mini.

## First boot test

1. Open Chrome or Edge on your phone/desktop (Web Bluetooth requires one of these).
2. Go to https://abourdim.github.io/rxy/
3. Switch to the **Play** tab → tap **📡 Connect**.
4. Pick **BBC micro:bit ESP32** from the chooser.
5. You should see a single "Test" button appear. Tapping it toggles the
   on-board blue LED on the C3 Super Mini.

If the connect overlay says "Requesting layout (GETCFG)…" and never
dismisses, the firmware is reachable but the `CFGEND` reply did not arrive
— check the serial monitor for `[BLE] Sent CFG`.

## Customising the layout

The firmware ships a minimal one-button layout. To use your own:

1. Open rxy → Build tab → design your remote.
2. Click **📄 Code**. In the generated MakeCode, copy the long string from:
   ```js
   const CFG = "....long base64 string..."
   ```
3. In the firmware, paste it into `LAYOUT_CFG_BASE64`, replacing the default.
4. Re-upload.

The widget IDs you used in the Build tab (`btn_fire`, `slider_speed`, …)
are the same IDs that arrive in `SET` messages, so the kid handles them
by name in `handleWidget()`.

## Protocol summary

| Direction       | Message                  | Example                  |
|-----------------|--------------------------|--------------------------|
| App → ESP32     | `SET <id> <value>`       | `SET slider_speed 75`    |
| App → ESP32     | `GETCFG`                 | (sent on connect)        |
| ESP32 → App     | `UPD <id> <value>`       | `UPD gauge_temp 23`      |
| ESP32 → App     | `CFGBEGIN`               | (start of layout reply)  |
| ESP32 → App     | `CFG <18-char chunk>`    | (one per chunk)          |
| ESP32 → App     | `CFGEND`                 | (end of layout reply)    |

Transport: BLE Nordic-UART-style service.
Service UUID: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
Notify (device → app): `6e400002-...`
Write  (app → device): `6e400003-...`

Note: the role of characteristics 0002 / 0003 is the **opposite** of the
Nordic UART Service convention used by Adafruit, Bluefruit, and most
generic NUS libraries. This is because we are impersonating the micro:bit,
which assigns the roles the other way around. The firmware declares the
characteristics explicitly so this is handled correctly.

## Caveats

- **Name spoofing.** The web app filters scans by name prefix
  `"BBC micro:bit"`, so this firmware advertises itself as
  `BBC micro:bit ESP32`. Fine for personal use; be aware that any other
  micro:bit-aware tool may also try to connect.
- **Web Bluetooth support.** Works in Chrome, Edge, Opera, and Samsung
  Internet. Does **not** work in Safari or Firefox on any platform, or
  in any browser on iOS (Apple does not allow Web Bluetooth in iOS).
- **One connection at a time.** BLE peripherals can hold one central
  connection; reconnecting from another phone requires disconnecting first.
