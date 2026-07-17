# CYD Claude Buddy — Design Notes

A desk companion for Claude Code on the ESP32-2432S028R "Cheap Yellow Display"
(CYD): the Clawd mascot plus a live usage dashboard, driven by Claude Code hooks
over Bluetooth LE. This document describes the design as shipped.

## 1. Overview

Claude Code emits hook events on the PC. A small Python helper (`buddy_hook.py`),
registered as a hook, reads the session transcript, computes a usage rollup, and
POSTs a snapshot to a local on-demand bridge (`buddy_bridge.py`), which relays
it to the device over BLE. The device renders the Clawd character for the
current activity and a stats card. All stats events are non-blocking and never
affect Claude's own permission flow. One **optional, opt-in** exception:
registering the `PermissionRequest` hook adds on-device tap-to-approve for a
pending tool call (an `ask` envelope + a decision notify relayed by the
bridge); it fails open to Claude's normal prompt on timeout or a disconnected
device.

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
- **Light sensor:** onboard photo-transistor on GPIO34 (ADC1; the reading rises
  in the dark) — drives the optional "Brightness: auto" night-dim.
- **BOOT key (GPIO0):** free after boot; polled as a runtime button.
- **Power: 5 V** over micro-USB or the `P1` VIN/GND pins. Two supported
  schemes: **wired** (any USB source), or **battery** — reference setup is a
  2000 mAh Li-ion behind a charge/discharge boost module feeding the 5 V rail.
  The cell has no data path to the MCU, so charge is *estimated* in software
  (`app/battery`, see `battery-gauge-spec.md`): a consumption-model integrator
  with a fully automatic death-anchored cycle — a real flat-battery brownout
  calibrates the learned capacity and the next clean boot refills to 100%
  (no manual control; a tappable reset proved too easy to fat-finger).

## 3. Transport & protocol

Two hops. Hop 1 (PC-internal): the hook POSTs the device's classic HTTP surface
to the bridge on `127.0.0.1:8787` — `POST /event`, `POST /ask`,
`GET /decision`, `GET /` (health), with the `X-Buddy-Token` header. Hop 2
(radio): the bridge wraps each body in a JSON envelope
`{"k":"event"|"ask","tok":"<token>","d":{…}}` and writes it to a GATT
characteristic (NimBLE server on the device, name `claude-cyd`, MTU 517):

- service `177b0001-6f32-4ea3-b878-866e7628de1f`
- `ingress` `177b0002-…` — write (events are latest-wins coalesced by the
  bridge; a dropped snapshot is healed by the next one)
- `decision` `177b0003-…` — notify + read: `{"askId":N,"decision":"allow"|"deny"}`

Bridge lifecycle: no autostart — the hook spawns it on connection-refused; the
listening port doubles as the single-instance lock; it scans in a budgeted
burst, holds one connection while events flow, goes radio-quiet (short rescan
every 5 min) when the device is away, and exits after 10 min without events.

- `event` body fields (all optional; last value sticks):
  `total`, `running` (session/activity flags), `msg` (activity text),
  `project`, `tokens` (today), `tokensAll`, `tools`, `turns`, `sessions`,
  `date` (PC-local `YYYY-MM-DD`; keys the on-device 30-day usage history — the
  device has no clock of its own).
- Auth is a shared token generated on the device (NVS) and shown on screen,
  carried in every envelope (`tok`); no BLE bonding — a ~10 m radio radius plus
  the token is proportionate for a desk gadget. The helper bypasses any system
  HTTP proxy since the bridge is on localhost.

## 4. PC helper (`tools/buddy_hook.py`)

Invoked by Claude Code hooks (see `tools/HOOKS.md`). For each event it:

- derives the project name from the cwd,
- scans the session transcript **incrementally** (a per-session byte offset in
  `~/.claude/buddy_tokens.json`; only appended lines are parsed, with a bounded
  recent-id window preserving the streaming dedupe) to sum tokens and count
  tool-use blocks and assistant turns,
- aggregates today's totals across sessions (persisted in
  `~/.claude/buddy_tokens.json`, reset at local midnight) plus an all-time token
  counter, and
- POSTs a `(running, total, activity)` snapshot with that rollup to the bridge
  (spawning the bridge first if it isn't running).

All events are non-blocking; bridge/device errors are swallowed (fail-open).

## 5. UI & states

- **Top bar:** state dot + label (`ASLEEP` / `READY` / `WORKING` / `DIZZY`), a
  battery-estimate glyph (5% buckets; amber <20%, red <10%), intensity pips and
  a link indicator.
- **Character region:** the Clawd GIF for the current state. While `busy`, a
  whimsical activity verb rotates in sync with the animation (a multi-clip state
  replays a clip smoothly and switches to the next every few seconds; the switch
  is the rotation signal). Only the headline repaints on rotation, so the stats
  grid never flickers.
- **Stats card:** a 3×2 grid — today / all-time tokens (`k`/`M`), tool calls,
  sessions, turns, session duration. The bottom card has two fixed pages side
  by side (0 stats left, 1 trends right): swipe left and the **trends card**
  (a bar per day, last 14 days, today live in coral; 7-day total + average)
  slides in over ~250 ms; swipe right slides back; the wrong direction
  rubber-bands. The transition snapshots both pages into 4bpp palette sprites
  (`screens/card_slide`, ~17 KB each) — the page renderers are canvas- and
  palette-parameterized (`ui::CardPal`) because 4bpp sprite draw colors are
  palette indices, not RGB565. The card returns to stats when the screen next
  sleeps.
- **Settings** (long-press): Stats panel (full detail), Quiet, Brightness
  (100 / 70 / 40 / auto — auto follows the light sensor, capping the backlight
  at a night level while the room is dark), and touch Recalibrate. Triple-tap
  anywhere = `dizzy` easter egg; a single tap on the character = a brief
  `heart` (petting).
- **BOOT key:** short press wakes the screen / acknowledges the nudge; holding
  it toggles Quiet (one-blink LED cue).
- Entering a new state pops the character in (80% → 100% over ~180 ms); clip
  switches inside a state don't pop, so the tuned busy rhythm is untouched.
  Gradual backlight changes (pre-sleep fade, auto-dim) ease over ~250 ms.
- The LED breathes real PWM envelopes for calm states (work blue, gentle
  attention amber, a magenta heartbeat for petting); urgency and errors remain
  crisp hard blinks.
- Auto screen-off: 30 s when calm, 3 min while working (long turns go dark
  too). Wake-on-work is edge-triggered (idle→working), so a sleeping screen
  isn't relit by ongoing work — only by a *fresh* turn, a nudge, or a touch.
  After 1 h with no touch and no hook events the device deep-sleeps itself
  (tap to wake). On battery it runs until the cell's protection cuts power
  (the brownout calibrates the gauge), checkpointing NVS every minute at ≤3%.

State selection: `dizzy` (recent triple-tap) → `sleep` (no bridge connected —
i.e. Claude isn't in use) → `busy` (`running>0`) → `idle`/ready (`total>0`) →
`sleep`.

## 6. Firmware architecture

```
src/
  main.cpp              orchestrator: state machine + transients, touch gesture
                        pipeline (tap/swipe/long-press), mode dispatch,
                        sleep/wake power loop
  app/
    ctx.{h,cpp}         small shared runtime state (session start, intensity,
                        Quiet, brightness)
    activity.{h,cpp}    pure tables/lookups: busy verbs, idle lines, per-clip
                        timeouts, state colours/labels, intensity tiers
    led_language.{h,cpp} state -> LED colour/rhythm mapping
    store.{h,cpp}       NVS: auth token + stats snapshot (save/restore)
    history.{h,cpp}     NVS: rolling 30-day per-day token history
    power.{h,cpp}       deep-sleep "power off" (touch/RST wakes; titled reason)
    battery.{h,cpp}     software battery gauge: consumption-model integrator,
                        NVS persistence, RTC-clock deep-sleep accounting
  ui/
    theme.h             RGB565 palette
    text.{h,cpp}        gtext/clamp/sprite-blit text + number formatting
    widgets.{h,cpp}     Rect hit-testing + buttons
  screens/
    layout.{h,cpp}      shared tap-target rects (action bar, settings rows, ack)
    home.{h,cpp}        status bar (incl. battery glyph), headline, stats card +
                        odometer, budget bar; owns the bottom card's page
                        (0 stats / 1 trends); drawStatsPage renders onto any
                        canvas/palette for the slide snapshots
    trends.{h,cpp}      trends card: 14-day usage bars + 7-day summary;
                        drawTrendsPage mirrors drawStatsPage for snapshots
    card_slide.{h,cpp}  directional page slide + rubber-band bounce (4bpp
                        palette snapshots of both pages, ~250 ms ease-out)
    stats_panel.{h,cpp} full live Stats panel
    settings.{h,cpp}    settings menu
    ask.{h,cpp}         opt-in "Allow this tool?" prompt
  hal/
    display.{h,cpp}     TFT_eSPI (ILI9341, HSPI) wrapper + backlight
    touch.{h,cpp}       direct XPT2046 (VSPI) driver + fixed/NVS calibration
    led.{h,cpp}         RGB status LED (active-LOW)
    storage.{h,cpp}     NVS (Preferences) wrapper: token, touch calibration
  net/
    ble.{h,cpp}         NimBLE GATT server + AppState
  render/
    character.{h,cpp}   Clawd GIF pack decode (AnimatedGIF -> off-screen sprite)
```

AppState stays single-threaded: NimBLE callbacks run on the NimBLE host task
but only enqueue raw payload copies (FreeRTOS queue); `Ble::loop()` parses and
applies them on the Arduino loop task, so the renderer and the transport never
race and no locking is needed.

## 7. Memory & flash (no PSRAM)

- No full-screen framebuffer. The character region is an off-screen
  `TFT_eSprite` double-buffer; `AnimatedGIF` decodes one scanline at a time and
  composites into it (nearest-neighbour scaled to the region). On a failed
  sprite allocation the renderer falls back to direct draw.
- The card-slide transition allocates two transient 4bpp page snapshots
  (240×140 ≈ 16.8 KB each; 16bpp would blow the largest free block), frees
  them at the end of the gesture, and degrades to an instant page switch /
  skipped bounce when either allocation fails.
- Partition table (`partitions.csv`, 4 MB, factory-only): `nvs` and `littlefs`
  are pinned at their historical offsets (token/calibration/stats and the
  ~1.2 MB GIF pack survive layout changes); `app0` is a single 2.625 MB factory
  slot — the BLE build uses ~30% of it. The dual-OTA slots and `ArduinoOTA`
  were removed with WiFi; firmware updates are USB-only, by design.

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
- **WiFi → BLE (2026-07).** WiFi's operational costs — per-venue captive-portal
  provisioning, IP/DHCP fragility, ~143 mA idle draw, and a heavyweight
  device stack — outweighed its one perk (wireless OTA). The transport moved to
  a self-hosted BLE GATT service plus an on-demand PC bridge with no autostart;
  OTA was traded away for USB-only flashing. See
  `docs/superpowers/specs/2026-07-16-ble-migration-design.md`.
- **Approval → passive dashboard → opt-in approval.** An earlier iteration
  showed permission prompts with on-device Approve/Deny. That was removed in
  favour of a passive usage dashboard, then a leaner version returned as an
  **opt-in**: only registering the `PermissionRequest` hook enables it, and it
  always fails open to Claude's normal prompt.
