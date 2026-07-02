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
  IRQ36. Resistive; a fixed factory calibration ships in firmware, overridable
  via Settings → Recalibrate (persisted to NVS).
- **RGB LED:** R4 / G16 / B17, active-LOW. USB-serial: CH340.

## 3. Transport & protocol

- Device runs `WiFiManager` (captive-portal provisioning) + a `WebServer`:
  - `POST /event` — JSON snapshot; requires header `X-Buddy-Token`.
  - `POST /ask` — show the opt-in Allow/Deny prompt for a pending tool call.
  - `GET /decision` — the tap for the current ask (`allow`/`deny`/`""`).
  - `GET /` — health.
- `POST /event` body fields (all optional; last value sticks):
  `total`, `running` (session/activity flags), `msg` (activity text),
  `project`, `tokens` (today), `tokensAll`, `tools`, `turns`, `sessions`.
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
  sessions, turns, session duration.
- **Settings** (long-press): Stats panel (full detail), touch Recalibrate, and a
  non-destructive WiFi reconfigure. Triple-tap anywhere = `dizzy` easter egg.
- 30 s auto screen-off when idle; a touch or new activity wakes it.

State selection: `dizzy` (recent triple-tap) → `sleep` (no WiFi/session) →
`busy` (`running>0`) → `idle`/ready (`total>0`) → `sleep`.

## 6. Firmware architecture

```
src/
  main.cpp              loop pump: net -> touch -> state -> render; UI + screens
  hal/
    display.{h,cpp}     TFT_eSPI (ILI9341, HSPI) wrapper + backlight
    touch.{h,cpp}       XPT2046 (VSPI) read + fixed/NVS calibration mapping
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
- Partition table (`partitions.csv`, 4 MB, single-app): `nvs`,
  `app0` (factory, ~1.81 MB), `littlefs` (~2.06 MB, holds the GIF pack). The
  LittleFS partition is labelled `littlefs` and mounted explicitly.

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
