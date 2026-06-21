# CYD Claude Buddy — Design Spec

- **Date:** 2026-06-21
- **Target board:** ESP32-2432S028R "Cheap Yellow Display" (CYD), **CYD2USB variant (micro-USB + USB-C) → ST7789 controller** (confirmed by user)
- **Status:** Approved — audit verdict **GO**, no hard blockers (2-agent external audit + upstream ground-truth from local clone)
- **Upstream reference:** `anthropics/claude-desktop-buddy` cloned read-only at `../claude-desktop-buddy-ref`

## 1. Goal & scope
Full-parity rewrite of the Claude Desktop Buddy firmware for the CYD. Upstream is used as **protocol/asset reference only** (REFERENCE.md states no repo code is needed). Everything (UI / render / input / state / BLE) is written fresh for CYD; the only reused source is the 18 ASCII pet drawing functions (see §6).

**In scope (full parity):** 18 ASCII pets (7 animations each), GIF character packs + live BLE folder-push upload, NVS stats/owner/species, transcript/info screen, 4 screens, LE Secure bonding with on-screen passkey, RGB-LED status, 30s auto screen-off.

## 2. Hardware (this exact board)
- MCU: ESP32-WROOM-32, 4 MB flash, **NO PSRAM**, ~520 KB SRAM.
- **Display: ILI9341 (`ILI9341_2_DRIVER`), 240×320, on HSPI** — MOSI13 / SCLK14 / MISO12 / CS15 / DC2 / RST-1 / BL21; SPI 55 MHz (drop to 40 MHz if unstable). **CONFIRMED ON HARDWARE (M1):** this dual-USB unit is ILI9341, NOT ST7789 — the dual-USB→ST7789 heuristic did not hold (ST7789 gave a pure-white screen; ILI9341 with default RGB order / no inversion renders correctly). No `TFT_RGB_ORDER`/`TFT_INVERSION` override needed.
- **Touch: XPT2046 on VSPI (separate bus)** — CLK25 / CS33 / MOSI32 / MISO39 / IRQ36. Resistive → **calibration required, persisted to NVS**. Lib: `XPT2046_Touchscreen` (Paul Stoffregen) on its own `SPIClass`.
- **RGB LED:** R4 / G16 / B17, **active-LOW**.
- LDR light sensor: GPIO34 (ADC1). microSD (VSPI): CS5 / SCK18 / MISO19 / MOSI23. Speaker: GPIO26. USB-serial: CH340 → **COM5**.

## 3. BLE protocol (from upstream REFERENCE.md — wire-compatible, implemented fresh)
- **Transport:** Nordic UART Service. Service `6e400001-…`, RX(write) `6e400002-…`, TX(notify) `6e400003-…`. Advertise a name starting with `Claude`. Newline-delimited UTF-8 JSON, one object per line; reassemble RX bytes until `\n`.
- **Inbound:** heartbeat snapshot `{total,running,waiting,msg,entries[],tokens,tokens_today,prompt{id,tool,hint}?}`; turn events `{evt:"turn",role,content[]}`; `{time:[epoch,tzOffSec]}`; `{cmd:"owner",name}`; commands `{cmd:"status"|"name"|"owner"|"unpair"}`; folder push `{cmd:"char_begin"|"file"|"chunk"|"file_end"|"char_end"}`.
- **Outbound:** permission `{cmd:"permission",id,decision:"once"|"deny"}` (approve = **"once"**); acks `{ack:<cmd>,ok,n,error?}`; status ack `{ack:"status",ok,data:{name,sec,sys{up,heap},stats{appr,deny,vel,nap,lvl}}}` (**omit `bat`** — CYD has no battery).
- **Rules:** no snapshot ~30 s ⇒ treat link dead; folder push is sequential (ack each chunk, decode+append, no whole-file buffering); **validate `file.path` — reject `..` and absolute**; whole pack ≤ 1.8 MB.
- **Pairing:** LE Secure Connections bonding, IO cap DisplayOnly, **6-digit passkey shown on the CYD screen**; mark NUS chars + TX CCCD encrypted-only; report `sec:true` in status; handle `{cmd:"unpair"}` by erasing bonds.

## 4. Seven animation states (upstream semantics)
| State | Trigger |
|---|---|
| `sleep` | bridge not connected |
| `idle` | connected, nothing urgent |
| `busy` | `running > 0` |
| `attention` | approval pending (`waiting>0` / `prompt`) — **RGB LED blinks** |
| `celebrate` | level up (every 50K tokens) |
| `dizzy` | gesture (triple-tap) |
| `heart` | approval granted in < 5 s |

## 5. Input mapping (touch only; no physical buttons, no IMU)
Bottom big-button bar, context-sensitive (resistive touch → large targets):
- **Approval screen:** `[ Deny | Approve ]` → `decision:"deny"` / `"once"`.
- **Normal / Pet / Info screens:** bottom bar `[ ◀ | ☰ menu | ▶ ]` to cycle screens / page / open menu (covers upstream A=next-screen, B=scroll/page, hold-A=menu).
- **Gestures:** long-press = screen off / sleep (replaces Power short + face-down nap); **triple-tap = dizzy** (replaces shake).
Touch zones are defined per-screen in the `ui` module; all hit-testing against current screen's rects.

## 6. Architecture / modules
```
src/
  main.cpp              setup wiring + loop pump (ble → input → state → render)
  hal/
    display.{h,cpp}     TFT_eSPI (ST7789, HSPI), draw + backlight + sprite tiles
    touch.{h,cpp}       XPT2046 (VSPI) read, calibration (NVS-persisted), debounced events
    led.{h,cpp}         RGB status LED (active-LOW)
    storage.{h,cpp}     LittleFS (packs) + NVS (stats/settings/calibration)
  input/
    input.{h,cpp}       touch → UI events (deny/approve, prev/next/menu, long-press, triple-tap)
  ble/
    nus.{h,cpp}         NimBLE NUS server + LE Secure bonding + passkey callback
    protocol.{h,cpp}    parse inbound / serialize outbound JSON (ArduinoJson v7)
    xfer.{h,cpp}        folder-push receiver (base64 chunks → LittleFS, path validation)
  render/
    character.{h,cpp}   GIF pack decode (AnimatedGIF, line-by-line GIFDraw + DMA)
    ascii_buddy.{h,cpp} 18 pets reused via M5→TFT_eSPI shim (see below)
    m5lcd_compat.h      shim mapping M5.Lcd.* drawing calls → TFT_eSPI, rescaled 135×240→240×320
  ui/
    ui.{h,cpp}          screens: Home(pet fullscreen)/Prompt/Info(stats+transcript)/Settings + passkey screen
    screens/...
  state/
    state.{h,cpp}       app state machine, sleep/wake, 30s auto-off (kept awake during approval)
  stats.{h,cpp}         NVS-backed stats/settings/owner/species
```
**ASCII pets:** reuse upstream `src/buddies/*.cpp` + `buddy.cpp` (18 species × 7 anims = 126 functions) behind `m5lcd_compat.h`, which re-implements the M5.Lcd drawing surface on TFT_eSPI and rescales coordinates 135×240 → 240×320. Avoids hand-redrawing 126 animations.

## 7. Memory & flash plan (no PSRAM)
- **No full-screen framebuffer** (240×320×2 = 150 KB). TFT_eSPI direct draw + a small `TFT_eSprite` tile for the pet region; AnimatedGIF decodes one scanline at a time via `GIFDraw` (≈22.5 KB static, DMA push). PSRAM not required.
- **Partition table** (4 MB, single-app, no dual-OTA — required to fit 1.8 MB packs):
```csv
# partitions.csv
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  factory, 0x10000,  0x1D0000,   # 1.81 MB app
littlefs, data, spiffs,  0x1E0000, 0x210000,   # 2.06 MB LittleFS (holds ≤1.8MB packs)
```
`board_build.partitions = partitions.csv`, `board_build.filesystem = littlefs` (LittleFS mounts the `spiffs`-subtype partition — standard Arduino-ESP32 convention).

## 8. BLE library
**NimBLE-Arduino** (`h2zero/NimBLE-Arduino`) — far smaller flash/RAM than built-in Bluedroid, first-class LE Secure bonding + passkey display. Suits no-PSRAM classic ESP32.

## 9. Build / flash
- `platform = espressif32` (**v6.x, Arduino core 2.x — NOT core 3.x / pioarduino**). `board = esp32dev` with manual TFT/pin `build_flags` (or drop rzeldent `esp32-2432S028R` board json into `boards/`).
- `upload_speed = 460800` (fallback `115200`), `monitor_speed = 115200`, `upload_port = COM5`.
- CH340 `0x13 wrong boot mode` flakiness → hold BOOT, tap RST, release, re-upload (or 1–10µF EN cap mod).
- `lib_deps`: `bodmer/TFT_eSPI@^2.5.43`, `bitbank2/AnimatedGIF@^2.1.1`, `bblanchon/ArduinoJson@^7`, `https://github.com/PaulStoffregen/XPT2046_Touchscreen.git`, `h2zero/NimBLE-Arduino`.
- TFT_eSPI via `build_flags`: `-DUSER_SETUP_LOADED -DST7789_DRIVER -DTFT_WIDTH=240 -DTFT_HEIGHT=320 -DTFT_MISO=12 -DTFT_MOSI=13 -DTFT_SCLK=14 -DTFT_CS=15 -DTFT_DC=2 -DTFT_RST=-1 -DTFT_BL=21 -DTFT_BACKLIGHT_ON=HIGH -DTFT_RGB_ORDER=TFT_BGR -DTFT_INVERSION_OFF -DUSE_HSPI_PORT -DSPI_FREQUENCY=55000000 -DSPI_READ_FREQUENCY=20000000 -DSPI_TOUCH_FREQUENCY=2500000`.

## 10. GIF asset rules (for packs + prep pipeline)
96 px wide, ≤ ~140 px tall, whole pack ≤ 1.8 MB. **Encode with a single global palette, non-interlaced, "replace/do-not-dispose" frame disposal** — required for correct line-by-line rendering without a framebuffer. `manifest.json` = `{name, colors{}, states{state: file|[files]}}` (arrays rotate per loop). Mirror upstream `tools/prep_character.py`.

## 11. Testing
- **Host unit tests** (PlatformIO `native` env): `protocol` parse/serialize, `xfer` framing + base64, path validation. TDD where practical.
- **On-device milestones (verification gates):** ① screen test pattern + text → ② touch calibration crosshair → ③ LittleFS + decode a test GIF → ④ BLE advertise + pair with Claude Desktop (passkey + echo) → ⑤ protocol + UI integration → ⑥ pack push + stats + settings full parity.

## 12. Error handling
BLE disconnect → return to advertise + sleep state; malformed line → drop + serial log, keep link; GIF decode fail / no pack → ASCII fallback; storage full on push → ack `ok:false,error`; touch noise → debounce + hit-rect only; feed watchdog, chunk flash writes; reject `..`/absolute paths in folder push.

## 13. Risks & mitigations
- **Display variant** → default ST7789 (board confirmed), symptom-based tuning + build flag fallback.
- **CH340 upload flakiness (0x13)** → BOOT/RST workaround documented.
- **GIF frame rate** → encoding rules + DMA; verify on hardware at milestone ③.
- **18-pet shim fidelity at 240×320** → rescale + per-state visual check; ASCII path is non-critical (GIF is the "nice" path).
- **Flash budget** → measured against partition table; app ceiling 1.81 MB.

## 14. Hosting
**Local only** (user chose clone-not-fork). New git repo at `projects/claude-buddy-cyd`. Upstream reference retained at `projects/claude-desktop-buddy-ref`.
