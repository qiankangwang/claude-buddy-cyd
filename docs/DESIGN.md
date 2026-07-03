# CYD Claude Buddy — Design Notes

A desk companion for Claude Code on the ESP32-2432S028R "Cheap Yellow Display"
(CYD): the Clawd mascot plus a live usage dashboard, driven by Claude Code hooks
over WiFi. This document describes the design as shipped.

## 1. Overview

Claude Code emits hook events on the PC. A small Python helper (`buddy_hook.py`),
registered as a hook, reads the session transcript, computes a usage rollup, and
POSTs a snapshot to the device over the LAN. The device renders the Clawd
character for the current activity and a stats card. All stats events are
non-blocking and never affect Claude's own permission flow. One **optional,
opt-in** exception: registering the `PermissionRequest` hook adds on-device
tap-to-approve for a pending tool call (`POST /ask` + polled `GET /decision`);
it fails open to Claude's normal prompt on timeout or an unreachable device.

## 2. Hardware

- MCU: ESP32-WROOM-32, 4 MB flash, **no PSRAM**, ~520 KB SRAM.
- **Display: ILI9341 (`ILI9341_2_DRIVER`), 240×320, HSPI** — MOSI13 / SCLK14 /
  MISO12 / CS15 / DC2 / RST-1 / BL21; SPI 55 MHz. This dual-USB (CYD2USB) unit is
  **ILI9341, not ST7789** — ST7789 produced a pure-white screen; ILI9341 with the
  default RGB order / no inversion renders correctly.
- **Touch: XPT2046 on VSPI** (separate bus) — CLK25 / CS33 / MOSI32 / MISO39 /
  IRQ36 (PENIRQ, used only as the deep-sleep wake pin). Resistive; the chip is
  driven directly (no touch library) with press/hold pressure hysteresis and
  median-of-5 sampling, tuned so light fingertip taps register. A fixed factory
  calibration ships in firmware, overridable via Settings → Recalibrate
  (persisted to NVS).
- **RGB LED:** R4 / G16 / B17, active-LOW. USB-serial: CH340.

## 3. Transport & protocol

- Device runs `WiFiManager` (captive-portal provisioning) + a `WebServer`:
  - `POST /event` — JSON snapshot; requires header `X-Buddy-Token`.
  - `POST /ask` — show the opt-in Allow/Deny prompt for a pending tool call.
  - `GET /decision` — the tap for the current ask (`allow`/`deny`/`""`).
  - `GET /` — health.
- `POST /event` body fields (all optional; last value sticks):
  `total`, `running` (session/activity flags), `msg` (activity text),
  `project`, `tokens` (today), `tokensAll`, `tools`, `turns`, `sessions`,
  `date` (PC-local `YYYY-MM-DD`; keys the on-device 30-day usage history — the
  device has no clock of its own).
- Auth is a shared token generated on the device (NVS) and shown on screen; the
  helper bypasses any system HTTP proxy since the device is on the LAN.

## 4. PC helper (`tools/buddy_hook.py`)

Invoked by Claude Code hooks (see `tools/HOOKS.md`). For each event it:

- derives the project name from the cwd,
- scans the session transcript once to sum tokens and count tool-use blocks and
  assistant turns,
- aggregates today's totals across sessions (persisted in
  `~/.claude/buddy_tokens.json`, reset at local midnight) plus an all-time token
  counter, and
- POSTs a `(running, total, activity)` snapshot with that rollup.

All events are non-blocking; device/network errors are swallowed (fail-open).

## 5. UI & states

- **Top bar:** state dot + label (`ASLEEP` / `READY` / `WORKING` / `DIZZY`) and a
  link indicator.
- **Character region:** the Clawd GIF for the current state. While `busy`, a
  whimsical activity verb rotates in sync with the animation (a multi-clip state
  replays a clip smoothly and switches to the next every few seconds; the switch
  is the rotation signal). Only the headline repaints on rotation, so the stats
  grid never flickers.
- **Stats card:** a 3×2 grid — today / all-time tokens (`k`/`M`), tool calls,
  sessions, turns, session duration. A sideways swipe pages the card to the
  **trends card** (a bar per day, last 14 days, today live in coral; 7-day
  total + average). The card returns to stats when the screen next sleeps.
- **Settings** (long-press): Stats panel (full detail), touch Recalibrate, and a
  non-destructive WiFi reconfigure. Triple-tap anywhere = `dizzy` easter egg;
  a single tap on the character = a brief `heart` (petting).
- 30 s auto screen-off when idle; a touch or new activity wakes it.

State selection: `dizzy` (recent triple-tap) → `sleep` (no WiFi/session) →
`busy` (`running>0`) → `idle`/ready (`total>0`) → `sleep`.

## 6. Firmware architecture

```
src/
  main.cpp              orchestrator: state machine + transients, touch gesture
                        pipeline (tap/swipe/long-press), mode dispatch,
                        sleep/wake power loop, WiFi OTA service
  app/
    ctx.{h,cpp}         small shared runtime state (session start, intensity,
                        Quiet, brightness)
    activity.{h,cpp}    pure tables/lookups: busy verbs, idle lines, per-clip
                        timeouts, state colours/labels, intensity tiers
    led_language.{h,cpp} state -> LED colour/rhythm mapping
    store.{h,cpp}       NVS: auth token + stats snapshot (save/restore)
    history.{h,cpp}     NVS: rolling 30-day per-day token history
    power.{h,cpp}       deep-sleep "power off" (touch/RST wakes)
  ui/
    theme.h             RGB565 palette
    text.{h,cpp}        gtext/clamp/sprite-blit text + number formatting
    widgets.{h,cpp}     Rect hit-testing + buttons
  screens/
    layout.{h,cpp}      shared tap-target rects (action bar, settings rows, ack)
    home.{h,cpp}        status bar, headline, stats card + odometer, budget bar;
                        owns the bottom card's page (0 stats / 1 trends)
    trends.{h,cpp}      trends card: 14-day usage bars + 7-day summary
    stats_panel.{h,cpp} full live Stats panel
    settings.{h,cpp}    settings menu
    wifi_confirm.{h,cpp} WiFi-portal confirmation
    ask.{h,cpp}         opt-in "Allow this tool?" prompt
  hal/
    display.{h,cpp}     TFT_eSPI (ILI9341, HSPI) wrapper + backlight
    touch.{h,cpp}       direct XPT2046 (VSPI) driver + fixed/NVS calibration
    led.{h,cpp}         RGB status LED (active-LOW)
    storage.{h,cpp}     NVS (Preferences) wrapper: token, touch calibration
  net/
    server.{h,cpp}      WiFiManager + WebServer + AppState
  render/
    character.{h,cpp}   Clawd GIF pack decode (AnimatedGIF -> off-screen sprite)
```

Single-threaded: HTTP is serviced inside `loop()` via `handleClient()`, so the
renderer and request handlers never race and no locking is needed.

## 7. Memory & flash (no PSRAM)

- No full-screen framebuffer. The character region is an off-screen
  `TFT_eSprite` double-buffer; `AnimatedGIF` decodes one scanline at a time and
  composites into it (nearest-neighbour scaled to the region). On a failed
  sprite allocation the renderer falls back to direct draw.
- Partition table (`partitions.csv`, 4 MB, dual-OTA): `nvs` (kept at its
  pre-OTA offset so credentials/token/calibration survive the migration),
  `otadata`, `app0`/`app1` (ota_0/ota_1, ~1.31 MB each), `littlefs` (~1.31 MB,
  holds the ~1.2 MB GIF pack). The LittleFS partition is labelled `littlefs`
  and mounted explicitly. Firmware and filesystem also flash over WiFi via
  `ArduinoOTA` (env `cyd-ota`; password = the device token).

## 8. Build & GIF assets

- `platform = espressif32@^6.x` (Arduino core 2.x), `board = esp32dev`, with TFT
  pins/driver set entirely via `build_flags`. `board_build.partitions` +
  `board_build.filesystem = littlefs`.
- GIF packs: a `manifest.json` maps `states` to a file or an array of files
  (arrays are the carousel for a state). Full-frame / transparent-over-black
  clips render correctly without a framebuffer.

## 9. Design history

- **BLE → WiFi.** The original target was the official Hardware Buddy BLE
  transport, which is not exposed in the available Claude desktop app build. The
  project pivoted to a self-hosted WiFi + Claude Code hooks transport with the
  same device/UX goals. The unused BLE implementation lived under `src/ble/`
  until mid-2026 (recoverable from git history).
- **Approval → passive dashboard → opt-in approval.** An earlier iteration
  showed permission prompts with on-device Approve/Deny. That was removed in
  favour of a passive usage dashboard, then a leaner version returned as an
  **opt-in**: only registering the `PermissionRequest` hook enables it, and it
  always fails open to Claude's normal prompt.
