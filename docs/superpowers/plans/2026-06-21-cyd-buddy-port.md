# CYD Claude Buddy Port — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline, chosen — flashing needs this session's COM5) to implement task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A full-parity Claude Desktop Buddy firmware for the CYD (ESP32-2432S028R, CYD2USB/ST7789), built with PlatformIO + Arduino, flashed over COM5.

**Architecture:** Fresh CYD-native firmware (HAL → BLE → render → UI → state) implementing the Hardware Buddy BLE protocol from REFERENCE.md. The 18 ASCII pets are reused from upstream behind an M5.Lcd→TFT_eSPI shim; GIF packs render via AnimatedGIF line-by-line. Each milestone is independently flashable and ends at a hardware/host verification gate.

**Tech Stack:** PlatformIO (espressif32 v6.x / Arduino core 2.x), TFT_eSPI, XPT2046_Touchscreen, AnimatedGIF, ArduinoJson v7, NimBLE-Arduino, LittleFS + NVS.

## Global Constraints
- Board: ESP32-2432S028R **CYD2USB → ST7789** 240×320; no PSRAM; 4 MB flash. Display on HSPI, touch XPT2046 on VSPI.
- Pins (exact): TFT MISO12/MOSI13/SCLK14/CS15/DC2/RST-1/BL21; touch CLK25/CS33/MOSI32/MISO39/IRQ36; RGB LED R4/G16/B17 **active-LOW**; LDR34; SD CS5/SCK18/MISO19/MOSI23; speaker26.
- TFT_eSPI: `ST7789_DRIVER`, `TFT_RGB_ORDER=TFT_BGR`, `TFT_INVERSION_OFF`, `USE_HSPI_PORT`, `SPI_FREQUENCY=55000000`.
- Platform pinned to `espressif32` **v6.x (Arduino core 2.x)** — never core 3.x / pioarduino.
- Build/flash: `board=esp32dev` + manual build_flags; `upload_speed=460800` (fallback 115200); `upload_port=COM5`; `monitor_speed=115200`. CH340 0x13 → hold BOOT/tap RST.
- Partitions: single-app no-OTA (`partitions.csv` from spec §7), `board_build.filesystem=littlefs`.
- BLE: NUS UUIDs `6e400001/2/3-b5a3-f393-e0a9-e50e24dcca9e`; advertise name starting `Claude`; newline-delimited UTF-8 JSON; LE Secure bonding + on-screen 6-digit passkey; approve = `decision:"once"`; omit `bat` in status.
- GIF packs: 96px wide, global palette, non-interlaced, replace-disposal, pack ≤1.8 MB.
- Code in English; UI/display strings may be Chinese/English. No Co-Authored-By trailer on commits.
- Reference (read-only): `../claude-desktop-buddy-ref` (REFERENCE.md, src/buddies/, characters/bufo/).

---

## Milestone 1 — Scaffold + screen bring-up  *(flash gate: screen shows test pattern + text)*

**Files:**
- Create: `platformio.ini`, `partitions.csv`, `boards/` (optional), `src/main.cpp`, `src/hal/display.h`, `src/hal/display.cpp`
- Reference: spec §7, §9

**Interfaces — Produces:**
- `hal::Display` — `void begin()`, `TFT_eSPI& tft()`, `void backlight(bool on)`, `void splash(const char* line)`

- [ ] **Step 1:** Write `platformio.ini` with `[env:cyd]` (platform `espressif32@^6.9.0`, board `esp32dev`, framework arduino, the Global-Constraints build_flags, lib_deps TFT_eSPI/XPT2046/AnimatedGIF/ArduinoJson/NimBLE, partitions+littlefs, upload/monitor settings) and a `[env:native]` for host tests.
- [ ] **Step 2:** Write `partitions.csv` (spec §7 table).
- [ ] **Step 3:** Write `src/hal/display.{h,cpp}` — wrap a `TFT_eSPI` instance; `begin()` does `tft.init(); tft.setRotation(0); backlight(true); tft.fillScreen(TFT_BLACK)`.
- [ ] **Step 4:** Write `src/main.cpp` — `setup()` calls `Display::begin()`, draws RGB color bars + centered text "CYD Buddy — bring-up"; `loop()` empty.
- [ ] **Step 5:** Build: `python -m platformio run -e cyd`. Expected: SUCCESS, prints flash/RAM usage (app < 1.81 MB).
- [ ] **Step 6:** Flash: `python -m platformio run -e cyd -t upload`. Expected: `Hash of data verified.` / `Hard resetting`. (On `0x13`: hold BOOT, tap RST, rerun.)
- [ ] **Step 7 (HW VERIFY):** Confirm on the physical screen: color bars + readable text, correct colors (not inverted, R/B not swapped). If garbled → switch to `ILI9341_2_DRIVER`; if negative → flip `TFT_INVERSION`; if R/B swapped → flip `TFT_RGB_ORDER`. Re-flash until correct.
- [ ] **Step 8:** Commit `feat: m1 scaffold + ST7789 screen bring-up`.

## Milestone 2 — Touch + calibration  *(gate: crosshair tracks finger; calibration persists)*

**Files:** Create `src/hal/touch.{h,cpp}`, `src/hal/storage.{h,cpp}` (NVS part); Modify `src/main.cpp`.

**Interfaces — Produces:**
- `hal::Touch` — `void begin()`, `bool read(int16_t& x,int16_t& y)`, `bool pressed()`, `void calibrate()` (interactive), loads/saves cal to NVS via `Storage`.
- `hal::Storage` — `void begin()`, `bool getBlob/putBlob(key,...)`, NVS-backed.

- [ ] Step 1: `Storage` NVS wrapper (Preferences) for cal data + later stats.
- [ ] Step 2: `Touch` on a dedicated `SPIClass(VSPI)` with XPT2046 pins; raw read + `map()` using stored cal (default min~200/max~3700).
- [ ] Step 3: `calibrate()` — draw 4 corner targets, capture raw, store affine map to NVS.
- [ ] Step 4: main.cpp — if no cal in NVS run calibrate(); else draw a crosshair following touch.
- [ ] Step 5: Build + flash + **HW VERIFY** crosshair tracks; power-cycle → cal persists (no re-calibrate).
- [ ] Step 6: Commit `feat: m2 XPT2046 touch + NVS calibration`.

## Milestone 3 — BLE NUS + bonding + passkey + echo  *(gate: pairs with Claude Desktop, passkey on screen, RX echoes)*

**Files:** Create `src/ble/nus.{h,cpp}`; Modify `src/main.cpp`. Lib: NimBLE-Arduino.

**Interfaces — Produces:**
- `ble::Nus` — `void begin(const char* name)`, `void onLine(std::function<void(const String&)>)`, `void send(const String& jsonLine)`, `bool connected()`, `bool secure()`, `void unpair()`; passkey via display callback `void onPasskey(std::function<void(uint32_t)>)`.

- [ ] Step 1: NimBLE init; NUS service+chars with exact UUIDs; RX write callback accumulates bytes, splits on `\n`, dispatches whole lines to `onLine`.
- [ ] Step 2: Security: `NimBLEDevice::setSecurityAuth(bond,mitm,sc)`, IO cap DisplayOnly; passkey callback → show 6-digit on screen; mark chars encrypted; TX notify.
- [ ] Step 3: Advertise name `Claude-CYD-XXXX` (append 2 MAC bytes); auto re-advertise on disconnect.
- [ ] Step 4: main.cpp — echo: on each RX line, `send` it back wrapped as `{"ack":"echo","ok":true}` and print to serial; show connection/secure state on screen.
- [ ] Step 5: Build + flash. **HW VERIFY**: Claude Desktop (Dev mode → Open Hardware Buddy → Connect) finds `Claude-CYD-*`, prompts passkey, screen shows matching 6 digits, link encrypts, serial shows inbound snapshot lines.
- [ ] Step 6: Commit `feat: m3 NimBLE NUS + LE Secure bonding + passkey`.

## Milestone 4 — Protocol parse/serialize + permission/status  *(gate: host unit tests pass; device approves/denies on touch)*

**Files:** Create `src/ble/protocol.{h,cpp}`, `test/test_protocol/test_protocol.cpp` (native); Modify `src/main.cpp`.

**Interfaces — Produces:**
- `proto::Snapshot {int total,running,waiting; String msg; std::vector<String> entries; long tokens,tokens_today; bool hasPrompt; String promptId,tool,hint;}`
- `proto::parseLine(const String&) -> ParsedKind` (snapshot/turn/time/owner/cmd...) filling tagged structs.
- `proto::permission(id,decision) -> String`, `proto::ack(cmd,ok,n,err) -> String`, `proto::statusData(...) -> String` (omit bat).

- [ ] Step 1 (TDD): native tests — parse the REFERENCE.md sample snapshot; assert fields. Run `pio test -e native` → FAIL.
- [ ] Step 2: Implement parser with ArduinoJson v7 (works on host). Re-run → PASS.
- [ ] Step 3 (TDD): tests for permission/ack/status serialization (approve=`once`, status omits bat). FAIL→implement→PASS.
- [ ] Step 4: Wire into main.cpp — on snapshot with prompt, draw tool+hint + bottom Deny/Approve placeholders; touch sends `permission`. Build+flash. **HW VERIFY** a real Claude permission prompt is approved/denied from the device.
- [ ] Step 5: Commit `feat: m4 protocol parse/serialize + permission flow`.

## Milestone 5 — State machine + UI screens + input mapping  *(gate: screens cycle, states drive UI)*

**Files:** Create `src/state/state.{h,cpp}`, `src/input/input.{h,cpp}`, `src/ui/ui.{h,cpp}` (+ `src/ui/screens/*`); Modify main.cpp.

**Interfaces — Produces:**
- `input::Event {None,DenyTap,ApproveTap,Prev,Next,Menu,LongPress,TripleTap}`; `input::poll() -> Event` (hit-rects per active screen, debounce, long-press, triple-tap timing).
- `state::Mode {Sleep,Idle,Busy,Attention,Celebrate,Dizzy,Heart}`; `state::update(Snapshot, Event, now)`; `state::mode()`; 30s auto-off (kept awake during Attention).
- `ui::Screen {Home,Prompt,Info,Settings,Passkey}`; `ui::render(state, snapshot)`.

- [ ] Steps: bottom-button bar widget; screen manager; map snapshot→mode (sleep=disconnected, busy=running>0, attention=waiting>0, celebrate=token level cross, heart=approve<5s, dizzy=triple-tap); long-press→screen off, triple-tap→dizzy. Build+flash+**HW VERIFY** cycling + state transitions. Commit `feat: m5 state machine + UI + input`.

## Milestone 6 — ASCII pets via M5→TFT_eSPI shim  *(gate: pets render all 7 states)*

**Files:** Create `src/render/m5lcd_compat.h`, `src/render/ascii_buddy.{h,cpp}`; copy `../claude-desktop-buddy-ref/src/buddies/*.cpp`, `buddy.cpp`, `buddy*.h` into `src/render/buddies/`; Modify build_src_filter.

**Interfaces — Produces:** `render::AsciiBuddy::draw(species, state, frameClock)`; species cycle persisted to NVS.

- [ ] Steps: implement `m5lcd_compat.h` mapping the M5.Lcd calls the buddies use (fillScreen/fillRect/drawString/setTextColor/fillCircle/...) onto TFT_eSPI, with a 135×240→240×320 coordinate scale. Compile the 18 buddies behind it. Render each state; menu cycles species. Build+flash+**HW VERIFY** several pets/states. Commit `feat: m6 18 ASCII pets via TFT_eSPI shim`.

## Milestone 7 — GIF rendering  *(gate: a test pack plays from LittleFS)*

**Files:** Create `src/render/character.{h,cpp}`; Modify main/state to prefer GIF when a pack is present.

**Interfaces — Produces:** `render::Character::load(manifestPath)`, `::draw(state, clock)` via AnimatedGIF `GIFDraw`→TFT_eSPI (DMA); manifest (colors+states, arrays rotate).

- [ ] Steps: stage `../claude-desktop-buddy-ref/characters/bufo` into `data/`, `pio run -t uploadfs`; parse manifest.json; AnimatedGIF line-by-line render of 96px GIFs centered on 240×320; state→gif mapping + array rotation. Build+flash+**HW VERIFY** bufo animates; measure FPS. Commit `feat: m7 GIF pack rendering`.

## Milestone 8 — Folder push (custom upload) + LittleFS  *(gate: drag a pack from Claude Desktop, it loads live)*

**Files:** Create `src/ble/xfer.{h,cpp}`; Modify nus/main.

**Interfaces — Produces:** `xfer::Receiver` handling char_begin/file/chunk(base64)/file_end/char_end with acks; writes to LittleFS under a pack dir; **path validation rejects `..`/absolute**; ≤1.8 MB guard; on char_end → `Character::load` live.

- [ ] Steps (TDD path-validation on native): base64 decode + sequential write + acks (`n`=bytes); reject bad paths with `ok:false`; storage-full guard. Build+flash+**HW VERIFY** drop a folder in Hardware Buddy window → streams → device switches to GIF live; Settings→delete reverts to ASCII. Commit `feat: m8 folder-push custom character upload`.

## Milestone 9 — Stats/owner/settings + full integration  *(gate: status panel populated; full parity pass)*

**Files:** Create `src/stats.{h,cpp}`; Modify state/ui/nus/main.

**Interfaces — Produces:** `stats::{appr,deny,vel,nap,lvl}` NVS-persisted; owner name; `name`/`owner`/`unpair`/`status` command handling with acks; level-up every 50K tokens → celebrate; factory reset via Settings.

- [ ] Steps: persist stats; status ack `{name,sec,sys{up,heap},stats{...}}` (omit bat); handle name/owner/unpair; Settings screen (owner display, species, delete char, factory reset, recalibrate). Build+flash+**HW VERIFY** Hardware Buddy stats panel populates; run a full session end-to-end. Commit `feat: m9 stats + settings + full integration`.

---

## Self-review
- **Spec coverage:** protocol(§3)→M3/M4/M8/M9; states(§4)→M5/M6/M7; input(§5)→M5; architecture(§6)→M1–M9; memory/partitions(§7)→M1/M7; BLE lib(§8)→M3; build(§9)→M1; GIF rules(§10)→M7/M8; testing(§11)→M4/M8 native + per-milestone HW gates; errors(§12)→M3 reconnect/M4 bad-line/M7 fallback/M8 path+full. All sections mapped.
- **Placeholders:** milestones 1–2 are step-level; 3–9 list concrete files/interfaces/verify gates and are step-expanded at execution per executing-plans (real interfaces given, not TBDs).
- **Type consistency:** `Snapshot`/`Event`/`Mode`/`Screen` names reused consistently across M4/M5; `Display`/`Touch`/`Storage`/`Nus` HAL names stable from M1–M3.
