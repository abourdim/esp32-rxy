/*
 * Micro:bit Remote Builder — ESP32 firmware
 *
 * Speaks the same BLE protocol as a BBC micro:bit running the rxy
 * MakeCode template, so the unmodified web app at
 *   https://abourdim.github.io/bit-rxy/
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
 *
 * =====================================================================
 *  POST-MORTEM: the GETCFG burst failure (rc=6 / BLE_HS_ENOMEM), and
 *  everything it took to actually find the fix
 * =====================================================================
 *  SYMPTOM
 *  -------
 *  Every "Connect" attempt from the web app hung on "Receiving
 *  layout... (N)" forever. N was suspiciously consistent (often 17 of
 *  the ~60 real CFG chunks) across firmware rebuilds, reboots, and even
 *  totally different layouts — a hard, content-independent ceiling,
 *  not random flakiness. Once return-code logging was added (see
 *  CONFIG_NIMBLE_CPP_LOG_LEVEL in platformio.ini), the cause behind
 *  that ceiling was visible directly: NimBLE's own notify()/indicate()
 *  kept returning rc=6 (BLE_HS_ENOMEM, "not enough memory") on almost
 *  every packet of the burst.
 *
 *  THINGS THAT LOOKED LIKE THE CAUSE BUT WEREN'T
 *  ----------------------------------------------
 *  Each of these was a real, reasoned hypothesis, tested in isolation,
 *  and ruled out — recorded here so nobody re-chases them later:
 *
 *  1. MTU too small / too large. Tried explicit setMTU(247), then
 *     setMTU(64), then no explicit setMTU() call at all (letting
 *     NimBLE's own built-in default of 255 stand). All three gave the
 *     identical rc=6 cascade starting at the identical point. MTU was
 *     never the variable.
 *
 *  2. notify() vs indicate(). Switched the TX characteristic to
 *     INDICATE, added a real wait-for-ack using the characteristic's
 *     onStatus() callback (which fires from the genuine asynchronous
 *     BLE_GAP_EVENT_NOTIFY_TX completion, not just a synchronous
 *     "was it queued?" guess). Still failed identically — indicate()
 *     even got nacked outright in some tests, likely because Web
 *     Bluetooth's startNotifications() only ever enables the notify
 *     CCCD bit, not indicate, on a dual-property characteristic.
 *
 *  3. Retrying on a failed return code. notify()'s bool return turned
 *     out to be an unreliable signal on this stack in general — it
 *     sometimes reported failure for packets that still made it over
 *     the air. Retrying on it was actively harmful: each "failed"
 *     attempt got physically resent anyway, duplicating chunks (the
 *     app's received count was observed exceeding the real total).
 *
 *  4. Connection interval too slow to drain the send queue. Added an
 *     explicit NimBLEServer::updateConnParams() request for a fast
 *     7.5–15ms interval on connect. No change.
 *
 *  5. The periodic demo-output loop racing the CFG burst. loop()'s
 *     once-a-second UPD sends (led_demo, gauge_demo, etc.) run on a
 *     different task than a BLE write callback; added gSendingCfg to
 *     gate them off for the duration of a transfer. Legitimate
 *     hygiene, kept in the fix — but not what was causing rc=6.
 *
 *  6. Arduino-ESP32 core / BLE controller version. The default-
 *     resolved core (2.0.9, IDF 4.4) has a BLE controller config with
 *     CONFIG_BT_CTRL_BLE_STATIC_ACL_TX_BUF_NB=0, which looked like a
 *     smoking gun. Pinned platformio.ini to a newer core (2.0.14,
 *     IDF 5.x, see platform_packages) to test it — same rc=6 cascade,
 *     and the same config value turned out to be present on BOTH core
 *     versions. Red herring. (The pin was left in place anyway, since
 *     it's the exact combination this firmware is now tested against.)
 *
 *  THE ISOLATION TEST THAT ACTUALLY FOUND IT
 *  ------------------------------------------
 *  After six independent, well-reasoned fixes all failed identically,
 *  continuing to patch the full ~700-line application stopped being
 *  productive. Instead, a separate minimal PlatformIO project was
 *  built (platformio_ble_probe/, kept in this repo) that talks to the
 *  same GATT service but with no CFG protocol, no demo loop, no app
 *  logic at all — just "on connect, blast N small notify() calls."
 *
 *  - v1 (arbitrary payload, burst fired from loop() on a 1s timer
 *    after connect): 60/60 succeeded. The chip and library are fine.
 *  - v2 (real CFG payload and CFGBEGIN/CFG/CFGEND framing, burst fired
 *    from inside the RX characteristic's onWrite() callback in
 *    response to a real "GETCFG" write — i.e. structured exactly like
 *    this firmware): failed identically, stalling at 17/60 with rc=6.
 *  - v2 again, with the ONLY change being: onWrite() sets a flag
 *    instead of calling the burst function directly, and loop() reads
 *    that flag and runs the burst itself: 60/60 succeeded.
 *
 *  THE ACTUAL ROOT CAUSE
 *  ----------------------
 *  RxCallbacks::onWrite() runs on NimBLE's own host task. The old code
 *  called sendCfg() — a ~900ms loop of 60 notify() calls with delay(15)
 *  between them — directly from inside that callback, meaning the BLE
 *  host task was still "inside" our own code for the whole burst. That
 *  blocked the host task from returning to its own event loop to
 *  process buffer-completion housekeeping (freeing send buffers as the
 *  controller reports they've actually gone out over the air) for the
 *  entire ~900ms — so the pool never had a chance to drain, and every
 *  notify() past the first handful failed with ENOMEM. This is a
 *  well-known class of bug in event-driven / RTOS systems generally:
 *  never do slow, blocking work synchronously inside a callback that
 *  runs on a thread you also depend on to make forward progress.
 *
 *  THE FIX
 *  -------
 *  handleLine() no longer calls sendCfg() when it sees "GETCFG" — it
 *  only sets gGetCfgRequested = true. loop() (running on the ordinary
 *  Arduino main task, not NimBLE's host task) checks that flag once per
 *  iteration and runs sendCfg() from there instead. The BLE host task
 *  is now completely free during the burst to do its own bookkeeping
 *  concurrently, so the buffer pool never starves. Plain NOTIFY (no
 *  indicate, no retry, no wait-for-ack machinery) is reliable once the
 *  burst runs from the right task — none of the complexity from
 *  hypotheses 2–4 above was ever actually needed.
 * =====================================================================
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
"eyJ0aXRsZSI6IlN1cGVyIERlbW8gUmVtb3RlIiwid2lkZ2V0cyI6W3siaWQiOiJidG5fanVtcCIsInQiOiJidXR0b24iLCJ4Ijo1MTUsInkiOjE5MCwidyI6MTAwLCJoIjoxMDAsImxhYmVsIjoiSnVtcCEiLCJtb2RlbCI6Im5lbyIsInByb3BzIjp7fX0seyJpZCI6ImJ0bl9maXJlIiwidCI6ImJ1dHRvbiIsIngiOjYzMCwieSI6MTkwLCJ3IjoxMDAsImgiOjEwMCwibGFiZWwiOiJGaXJlISIsIm1vZGVsIjoiZ2xhc3MiLCJwcm9wcyI6e319LHsiaWQiOiJzbGlkZXJfc3BlZWQiLCJ0Ijoic2xpZGVyIiwieCI6MzI1LCJ5IjoxOTAsInciOjgwLCJoIjoxNjAsImxhYmVsIjoiU3BlZWQiLCJtb2RlbCI6InRyYWNrIiwibWluIjowLCJtYXgiOjEwMCwic3RlcCI6MSwicHJvcHMiOnt9fSx7ImlkIjoic2xpZGVyX3Bvd2VyIiwidCI6InNsaWRlciIsIngiOjQyMCwieSI6MTkwLCJ3Ijo4MCwiaCI6MTYwLCJsYWJlbCI6IlBvd2VyIiwibW9kZWwiOiJuZW9uIiwibWluIjowLCJtYXgiOjEwMCwic3RlcCI6MSwicHJvcHMiOnt9fSx7ImlkIjoidG9nZ2xlX3R1cmJvIiwidCI6InRvZ2dsZSIsIngiOjE1MCwieSI6MzY1LCJ3IjoxMDAsImgiOjgwLCJsYWJlbCI6IlR1cmJvIiwibW9kZWwiOiJwaWxsIiwicHJvcHMiOnt9fSx7ImlkIjoidG9nZ2xlX3NoaWVsZCIsInQiOiJ0b2dnbGUiLCJ4IjoyNjUsInkiOjM2NSwidyI6MTAwLCJoIjo4MCwibGFiZWwiOiJTaGllbGQiLCJtb2RlbCI6Imljb24iLCJwcm9wcyI6e319LHsiaWQiOiJsZWRfc3RhdHVzIiwidCI6ImxlZCIsIngiOjQ2NSwieSI6MzY1LCJ3Ijo3MCwiaCI6NzAsImxhYmVsIjoiU3RhdHVzIiwibW9kZWwiOiJkb3QiLCJjb2xvck9uIjoiIzAwZTY3NiIsImNvbG9yT2ZmIjoiIzFiMmEzYSIsInByb3BzIjp7fX0seyJpZCI6ImxlZF9hbGVydCIsInQiOiJsZWQiLCJ4Ijo1NTAsInkiOjM2NSwidyI6NzAsImgiOjcwLCJsYWJlbCI6IkFsZXJ0IiwibW9kZWwiOiJyaW5nIiwiY29sb3JPbiI6IiNmZjUyNTIiLCJjb2xvck9mZiI6IiMxYjJhM2EiLCJwcm9wcyI6e319LHsiaWQiOiJqb3lfbW92ZSIsInQiOiJqb3lzdGljayIsIngiOjc2MCwieSI6MTUsInciOjE0MCwiaCI6MTQwLCJsYWJlbCI6Ik1vdmUiLCJtb2RlbCI6InJpbmciLCJwcm9wcyI6e319LHsiaWQiOiJkcGFkX25hdiIsInQiOiJkcGFkIiwieCI6MTUsInkiOjE5MCwidyI6MTQwLCJoIjoxNDAsImxhYmVsIjoiTmF2aWdhdGUiLCJtb2RlbCI6ImNsYXNzaWMiLCJwcm9wcyI6e319LHsiaWQiOiJsYWJlbF9zY29yZSIsInQiOiJsYWJlbCIsIngiOjc0NSwieSI6MTkwLCJ3IjoxODAsImgiOjUwLCJsYWJlbCI6IlNjb3JlOiAwIiwibW9kZWwiOiJjaGlwIiwicHJvcHMiOnt9fSx7ImlkIjoieHlwYWRfYWltIiwidCI6Inh5cGFkIiwieCI6MTcwLCJ5IjoxOTAsInciOjE0MCwiaCI6MTQwLCJsYWJlbCI6IkFpbSIsIm1vZGVsIjoiZ3JpZCIsInByb3BzIjp7fX0seyJpZCI6ImJhdHRlcnlfbGV2ZWwiLCJ0IjoiYmF0dGVyeSIsIngiOjM4MCwieSI6MzY1LCJ3Ijo3MCwiaCI6MTAwLCJsYWJlbCI6IlBvd2VyIiwibW9kZWwiOiJ2ZXJ0aWNhbCIsInByb3BzIjp7fX0seyJpZCI6InRpbWVyX2dhbWUiLCJ0IjoidGltZXIiLCJ4IjoxNSwieSI6MzY1LCJ3IjoxMjAsImgiOjcwLCJsYWJlbCI6IkdhbWUgVGltZSIsIm1vZGVsIjoiZGlnaXRhbCIsImF1dG9TdGFydCI6ZmFsc2UsInByb3BzIjp7fX0seyJpZCI6ImdhdWdlX3RlbXAiLCJ0IjoiZ2F1Z2UiLCJ4Ijo0NTAsInkiOjE1LCJ3IjoxNDAsImgiOjE2MCwibGFiZWwiOiJUZW1wIiwibWluIjowLCJtYXgiOjUwLCJ1bml0cyI6IsKwQyIsImRlY2ltYWxzIjoxLCJtb2RlbCI6ImNsYXNzaWMiLCJ3YXJuIjpudWxsLCJkYW5nZXIiOm51bGwsInByb3BzIjp7fX0seyJpZCI6ImdhdWdlX2xldmVsIiwidCI6ImdhdWdlIiwieCI6NjA1LCJ5IjoxNSwidyI6MTQwLCJoIjoxNjAsImxhYmVsIjoiTGV2ZWwiLCJtaW4iOjAsIm1heCI6MTAwLCJ1bml0cyI6IiUiLCJkZWNpbWFscyI6MCwibW9kZWwiOiJuZW9uIiwid2FybiI6bnVsbCwiZGFuZ2VyIjpudWxsLCJwcm9wcyI6e319LHsiaWQiOiJncmFwaF9lbnYiLCJ0IjoiZ3JhcGgiLCJ4IjoxNSwieSI6MTUsInciOjQyMCwiaCI6MTUwLCJsYWJlbCI6IkxpdmUgRGF0YSIsInNlcmllcyI6Miwid2luZG93U2VjIjozMCwiYXV0b1NjYWxlIjp0cnVlLCJtb2RlbCI6ImdyaWQiLCJtaW4iOjAsIm1heCI6MTAwLCJzaG93TGVnZW5kIjp0cnVlLCJwcm9wcyI6e319XX0=";
	//"eyJ0aXRsZSI6IlN1cGVyIERlbW8gUmVtb3RlIiwid2lkZ2V0cyI6W3siaWQiOiJidG5fanVtcCIsInQiOiJidXR0b24iLCJ4Ijo1MTUsInkiOjE5MCwidyI6MTAwLCJoIjoxMDAsImxhYmVsIjoiSnVtcCEiLCJtb2RlbCI6Im5lbyIsInByb3BzIjp7fX0seyJpZCI6ImJ0bl9maXJlIiwidCI6ImJ1dHRvbiIsIngiOjYzMCwieSI6MTkwLCJ3IjoxMDAsImgiOjEwMCwibGFiZWwiOiJGaXJlISIsIm1vZGVsIjoiZ2xhc3MiLCJwcm9wcyI6e319LHsiaWQiOiJzbGlkZXJfc3BlZWQiLCJ0Ijoic2xpZGVyIiwieCI6MzI1LCJ5IjoxOTAsInciOjgwLCJoIjoxNjAsImxhYmVsIjoiU3BlZWQiLCJtb2RlbCI6InRyYWNrIiwibWluIjowLCJtYXgiOjEwMCwic3RlcCI6MSwicHJvcHMiOnt9fSx7ImlkIjoic2xpZGVyX3Bvd2VyIiwidCI6InNsaWRlciIsIngiOjQyMCwieSI6MTkwLCJ3Ijo4MCwiaCI6MTYwLCJsYWJlbCI6IlBvd2VyIiwibW9kZWwiOiJuZW9uIiwibWluIjowLCJtYXgiOjEwMCwic3RlcCI6MSwicHJvcHMiOnt9fSx7ImlkIjoidG9nZ2xlX3R1cmJvIiwidCI6InRvZ2dsZSIsIngiOjE1MCwieSI6MzY1LCJ3IjoxMDAsImgiOjgwLCJsYWJlbCI6IlR1cmJvIiwibW9kZWwiOiJwaWxsIiwicHJvcHMiOnt9fSx7ImlkIjoidG9nZ2xlX3NoaWVsZCIsInQiOiJ0b2dnbGUiLCJ4IjoyNjUsInkiOjM2NSwidyI6MTAwLCJoIjo4MCwibGFiZWwiOiJTaGllbGQiLCJtb2RlbCI6Imljb24iLCJwcm9wcyI6e319LHsiaWQiOiJsZWRfc3RhdHVzIiwidCI6ImxlZCIsIngiOjQ2NSwieSI6MzY1LCJ3Ijo3MCwiaCI6NzAsImxhYmVsIjoiU3RhdHVzIiwibW9kZWwiOiJkb3QiLCJjb2xvck9uIjoiIzAwZTY3NiIsImNvbG9yT2ZmIjoiIzFiMmEzYSIsInByb3BzIjp7fX0seyJpZCI6ImxlZF9hbGVydCIsInQiOiJsZWQiLCJ4Ijo1NTAsInkiOjM2NSwidyI6NzAsImgiOjcwLCJsYWJlbCI6IkFsZXJ0IiwibW9kZWwiOiJyaW5nIiwiY29sb3JPbiI6IiNmZjUyNTIiLCJjb2xvck9mZiI6IiMxYjJhM2EiLCJwcm9wcyI6e319LHsiaWQiOiJqb3lfbW92ZSIsInQiOiJqb3lzdGljayIsIngiOjc2MCwieSI6MTUsInciOjE0MCwiaCI6MTQwLCJsYWJlbCI6Ik1vdmUiLCJtb2RlbCI6InJpbmciLCJwcm9wcyI6e319LHsiaWQiOiJkcGFkX25hdiIsInQiOiJkcGFkIiwieCI6MTUsInkiOjE5MCwidyI6MTQwLCJoIjoxNDAsImxhYmVsIjoiTmF2aWdhdGUiLCJtb2RlbCI6ImNsYXNzaWMiLCJwcm9wcyI6e319LHsiaWQiOiJsYWJlbF9zY29yZSIsInQiOiJsYWJlbCIsIngiOjc0NSwieSI6MTkwLCJ3IjoxODAsImgiOjUwLCJsYWJlbCI6IlNjb3JlOiAwIiwibW9kZWwiOiJjaGlwIiwicHJvcHMiOnt9fSx7ImlkIjoieHlwYWRfYWltIiwidCI6Inh5cGFkIiwieCI6MTcwLCJ5IjoxOTAsInciOjE0MCwiaCI6MTQwLCJsYWJlbCI6IkFpbSIsIm1vZGVsIjoiZ3JpZCIsInByb3BzIjp7fX0seyJpZCI6ImJhdHRlcnlfbGV2ZWwiLCJ0IjoiYmF0dGVyeSIsIngiOjM4MCwieSI6MzY1LCJ3Ijo3MCwiaCI6MTAwLCJsYWJlbCI6IlBvd2VyIiwibW9kZWwiOiJ2ZXJ0aWNhbCIsInByb3BzIjp7fX0seyJpZCI6InRpbWVyX2dhbWUiLCJ0IjoidGltZXIiLCJ4IjoxNSwieSI6MzY1LCJ3IjoxMjAsImgiOjcwLCJsYWJlbCI6IkdhbWUgVGltZSIsIm1vZGVsIjoiZGlnaXRhbCIsImF1dG9TdGFydCI6ZmFsc2UsInByb3BzIjp7fX0seyJpZCI6ImdhdWdlX3RlbXAiLCJ0IjoiZ2F1Z2UiLCJ4Ijo0NTAsInkiOjE1LCJ3IjoxNDAsImgiOjE2MCwibGFiZWwiOiJUZW1wIiwibWluIjowLCJtYXgiOjUwLCJ1bml0cyI6IsKwQyIsImRlY2ltYWxzIjoxLCJtb2RlbCI6ImNsYXNzaWMiLCJ3YXJuIjpudWxsLCJkYW5nZXIiOm51bGwsInByb3BzIjp7fX0seyJpZCI6ImdhdWdlX2xldmVsIiwidCI6ImdhdWdlIiwieCI6NjA1LCJ5IjoxNSwidyI6MTQwLCJoIjoxNjAsImxhYmVsIjoiTGV2ZWwiLCJtaW4iOjAsIm1heCI6MTAwLCJ1bml0cyI6IiUiLCJkZWNpbWFscyI6MCwibW9kZWwiOiJuZW9uIiwid2FybiI6bnVsbCwiZGFuZ2VyIjpudWxsLCJwcm9wcyI6e319LHsiaWQiOiJncmFwaF9lbnYiLCJ0IjoiZ3JhcGgiLCJ4IjoxNSwieSI6MTUsInciOjQyMCwiaCI6MTUwLCJsYWJlbCI6IkxpdmUgRGF0YSIsInNlcmllcyI6Miwid2luZG93U2VjIjozMCwiYXV0b1NjYWxlIjp0cnVlLCJtb2RlbCI6ImdyaWQiLCJtaW4iOjAsIm1heCI6MTAwLCJzaG93TGVnZW5kIjp0cnVlLCJwcm9wcyI6e319XX0=";
  
  
//"eyJ0aXRsZSI6IlN1cGVyIERlbW8gUmVtb3RlIiwid2lkZ2V0cyI6W3siaWQiOiJzbGlkZXIzNiIsInQiOiJzbGlkZXIiLCJ4IjoyMCwieSI6MjAsInciOjkwLCJoIjoxODAsImxhYmVsIjoiQXJtIDEiLCJtb2RlbCI6InRyYWNrIiwibWluIjowLCJtYXgiOjEwMCwic3RlcCI6MX0seyJpZCI6InNsaWRlcjM3IiwidCI6InNsaWRlciIsIngiOjEzMCwieSI6MjAsInciOjkwLCJoIjoxODAsImxhYmVsIjoiQXJtIDIiLCJtb2RlbCI6InRyYWNrIiwibWluIjowLCJtYXgiOjEwMCwic3RlcCI6MX0seyJpZCI6InNsaWRlcjM4IiwidCI6InNsaWRlciIsIngiOjI0MCwieSI6MjAsInciOjkwLCJoIjoxODAsImxhYmVsIjoiQXJtIDMiLCJtb2RlbCI6InRyYWNrIiwibWluIjowLCJtYXgiOjEwMCwic3RlcCI6MX0seyJpZCI6InRvZ2dsZTM5IiwidCI6InRvZ2dsZSIsIngiOjMzOCwieSI6ODAsInciOjkwLCJoIjo5MCwibGFiZWwiOiJHcmlwIiwibW9kZWwiOiJzcXVhcmUifSx7ImlkIjoiZ2F1Z2U0MCIsInQiOiJnYXVnZSIsIngiOjQ5MiwieSI6NDIsInciOjE0MCwiaCI6MTYwLCJsYWJlbCI6IiIsIm1vZGVsIjoiY2xhc3NpYyIsIm1pbiI6MCwibWF4IjoxMDAsImRlY2ltYWxzIjoxLCJ1bml0cyI6IiIsIndhcm4iOm51bGwsImRhbmdlciI6bnVsbH0seyJpZCI6ImdhdWdlNDEiLCJ0IjoiZ2F1Z2UiLCJ4Ijo2NjksInkiOjQwLCJ3IjoxNDAsImgiOjE2MCwibGFiZWwiOiIiLCJtb2RlbCI6ImNsYXNzaWMiLCJtaW4iOjAsIm1heCI6MTAwLCJkZWNpbWFscyI6MSwidW5pdHMiOiIiLCJ3YXJuIjpudWxsLCJkYW5nZXIiOm51bGx9XX0=";  
  
  /*
  "eyJ0aXRsZSI6Ik15IFJlbW90ZSIsIndpZGdldHMiOlt7ImlkIjoiYnRuX3Rlc3"
  "QiLCJ0IjoiYnV0dG9uIiwieCI6NTAsInkiOjUwLCJ3IjoxMDAsImgiOjEwMCwi"
   "bGFiZWwiOiJUZXN0IiwibW9kZWwiOiJuZW8ifV19";
  */

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
// Guards against loop()'s periodic demo output racing sendCfg()'s burst
// now that both run on the same task (see gGetCfgRequested below) — a
// real micro:bit's MakeCode firmware does the same with "if (cfgSent)".
static volatile bool         gSendingCfg = false;
static String                gRxBuffer;

// Set from onWrite() (NimBLE's own host task), consumed from loop() (the
// main Arduino task). THIS WAS THE ACTUAL BUG behind the whole rc=6
// (BLE_HS_ENOMEM) saga: onWrite() used to call sendCfg() directly and
// synchronously — running the ~900ms/60-notify burst ON NimBLE's host
// task while still inside its own callback. That blocks the host task
// from processing its own buffer-completion housekeeping for the whole
// burst, starving the very pool sendCfg() depends on. It was never the
// MTU, indicate() vs notify(), connection interval, or the demo loop —
// isolated testing (see platformio_ble_probe/) proved a 60-packet
// notify() burst works perfectly (60/60) as long as it runs from
// loop(), and fails identically to the real firmware the moment it
// runs from inside a BLE callback instead.
static volatile bool         gGetCfgRequested = false;

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
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    gConnected = true;
    gRxBuffer  = "";
    Serial.printf("[BLE] Client connected  peer=%s\n",
                  info.getAddress().toString().c_str());
    // Request a fast connection interval (7.5-15ms, units of 1.25ms) so
    // the controller drains notify()'s buffer pool quickly enough to
    // survive sendCfg()'s ~60-chunk burst. Left at whatever slower
    // default the central negotiated, notify() calls pile up faster
    // than the radio can actually transmit them, exhausting NimBLE's
    // mbuf pool and causing it to refuse (return false on) most sends
    // mid-burst — observed as near-universal "dropped" log lines even
    // though most chunks still trickled through opportunistically.
    server->updateConnParams(info.getConnHandle(), 6, 12, 0, 400);
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
  // Plain NOTIFY, single attempt, no retry, no wait-for-ack. We spent a
  // long time chasing rc=6 (BLE_HS_ENOMEM) through indicate()+confirm
  // handshakes, MTU tuning, and connection-interval tweaks — none of it
  // was the real cause. Isolated testing (platformio_ble_probe/) proved
  // a 60-packet notify() burst succeeds 100% of the time on this exact
  // chip/library, as long as it doesn't run from inside a BLE callback
  // (see gGetCfgRequested and loop() for why that mattered). With the
  // burst correctly deferred to loop(), plain notify() is reliable and
  // needs none of the complexity we tried before.
  gTxChar->notify((const uint8_t*)out.c_str(), out.length());
}

static void sendCfg() {
  gSendingCfg = true;  // block loop()'s demo output for the whole burst
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
  gSendingCfg = false;
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

  // Defer to loop() — do NOT call sendCfg() directly here. handleLine()
  // runs from onWrite() on NimBLE's own host task; see gGetCfgRequested
  // for why running the burst synchronously from that task was the
  // actual bug all along.
  if (line == "GETCFG") { gGetCfgRequested = true; return; }

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
//   Select    "select_..."  val: the chosen option's text
//   Edit Field "editfield_..." val: whatever text was typed
//
// To update an output widget (LED, label, gauge, graph, battery, sound,
// notification) call sendValue() from anywhere:
//
//   sendValue("led_status",  "1");
//   sendValue("gauge_temp",  "23");
//   sendValue("label_score", "Score: 42");
//   sendValue("graph_data",  "12,7,18");
//   sendValue("battery_lvl", "75");
//   sendValue("sound_fx",    "beep");    // beep|success|warn|danger|toggle
//   sendValue("alert_box",   "Uh oh!");
//
// A live, running example of all seven is in loop() below (search for
// "Output widget demo") — add an LED/Label/Gauge/Graph/Battery/Sound/
// Notification widget with a matching *_demo ID in the Build tab to see
// it update once connected.
//
// EVERY input widget type below already does something with the one actuator
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

  // ------- Select: val is the chosen option's text — LED flips once -------
  // handleWidget() runs on NimBLE's own host task (see handleLine() above),
  // so no delay()/blink-then-wait here — just flip state, same as timer_.
  if (id.startsWith("select_")) {
    digitalWrite(LED_PIN, digitalRead(LED_PIN) == LED_ON ? LED_OFF : LED_ON);
    Serial.printf("[WIDGET] select   '%s' = '%s'\n", id.c_str(), val.c_str());
    return;
  }

  // ------- Edit Field: val is whatever text was typed — LED flips once ----
  if (id.startsWith("editfield_")) {
    digitalWrite(LED_PIN, digitalRead(LED_PIN) == LED_ON ? LED_OFF : LED_ON);
    Serial.printf("[WIDGET] editfield '%s' = '%s'\n", id.c_str(), val.c_str());
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
  // No explicit setMTU() call — matches a real micro:bit, which never
  // calls anything like setMTU() either. NimBLE-Arduino's own built-in
  // default preferred MTU (255, per nimconfig.h) already gives plenty
  // of room for a 23-byte "CFG <18-char>\n" line, so nothing further
  // is needed here.

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
  Serial.println("[BLE] Open https://abourdim.github.io/bit-rxy/ and click Connect.");
  Serial.println("=================================================");
  Serial.flush();
}

void loop() {
  const uint32_t now = millis();

  // -----------------------------------------------------------------
  // Handle a pending GETCFG here, not from onWrite() — see
  // gGetCfgRequested for why running the burst directly from the BLE
  // callback was the actual cause of the CFG-transfer failures.
  // -----------------------------------------------------------------
  if (gGetCfgRequested) {
    gGetCfgRequested = false;
    sendCfg();
  }

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
  // Output widget demo — the other half of the widget catalog. LED,
  // Label, Gauge, Graph, and Battery widgets are OUTPUTS (micro:bit -> app):
  // they never appear in handleWidget() because they don't send SET
  // messages, they only receive sendValue() updates. The firmware can't
  // see what you named them in the Build tab (LAYOUT_CFG_BASE64 is an
  // opaque blob to it), so add a widget of the matching type using one
  // of the IDs below (or edit the strings here to match yours) to see
  // it update live once connected. Runs once a second — the rxy app
  // rate-limits incoming messages to about one per 200 ms, so don't
  // go faster than that.
  // -----------------------------------------------------------------
  static uint32_t lastOutputDemo = 0;
  if (gConnected && !gSendingCfg && now - lastOutputDemo >= 1000) {
    lastOutputDemo = now;
    static int demoTick = 0;
    demoTick++;
    sendValue("led_demo",     (demoTick % 2) ? "1" : "0");             // blinks every tick
    sendValue("label_demo",   "Tick " + String(demoTick));             // free-text label
    sendValue("gauge_demo",   String((int)temperatureRead()));         // chip temp in °C
    sendValue("graph_demo",   String(demoTick % 100) + "," + String((demoTick * 3) % 100));
    sendValue("battery_demo", String(100 - (demoTick % 100)));         // fake drain 100 -> 0
    // Sound/Notification are attention-grabbing effects, not continuous
    // telemetry — fire once every 5 ticks instead of every tick like the
    // others above, so the demo doesn't spam beeps/banners nonstop.
    if (demoTick % 5 == 0) {
      sendValue("sound_demo", "beep");                                 // beep | success | warn | danger | toggle
      sendValue("notification_demo", "Hello from ESP32!");
    }
  }
}
