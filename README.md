# CYD Claude Buddy

<p align="center">
  <img src="assets/painting.gif" width="116" alt="painting">
  <img src="assets/sweeping.gif" width="116" alt="sweeping">
  <img src="assets/stirring.gif" width="116" alt="stirring">
  <img src="assets/building.gif" width="116" alt="building">
  <img src="assets/typing.gif" width="116" alt="typing">
</p>
<p align="center"><sub>Clawd hard at work — painting&nbsp;·&nbsp;sweeping&nbsp;·&nbsp;stirring&nbsp;·&nbsp;building&nbsp;·&nbsp;typing</sub></p>

A desk companion for Claude Code: the orange **Clawd** mascot on a **Cheap
Yellow Display** (ESP32) that mirrors your live Claude Code activity and usage
stats — driven entirely by Claude Code **hooks** over plain WiFi, with **no
Bluetooth and no always-on PC process**.

Clawd reacts to what Claude is doing (sleeping / ready / working, plus little
reactions when Claude needs you, finishes, or a session starts) while a stats
card tracks your usage: tokens today and all-time, tool calls, sessions, turns,
and the current session's duration. A tiny Python helper, invoked by Claude Code
hooks, reads each session's transcript and pushes a snapshot to the device.

Built for the **CYD** — the cheapest all-in-one ESP32 + screen + touch board.
The app sits on a thin HAL over `TFT_eSPI`, so it can also be adapted to other
ESP32 + TFT panels if you want — see [Adapting to other
boards](#adapting-to-other-boards).

> The official "Hardware Buddy" Bluetooth feature isn't exposed in the Claude
> desktop app build used here, so this project reproduces the experience over a
> self-hosted transport: a tiny HTTP server on the device plus Claude Code hooks
> on the PC.
>
> _Unofficial, personal fan project — **not affiliated with or endorsed by
> Anthropic.** "Clawd" is Anthropic's character; see [License &
> credits](#license--credits)._

---

## Contents

- [How it works](#how-it-works)
- [What it shows](#what-it-shows)
- [Hardware](#hardware) · [Adapting to other boards](#adapting-to-other-boards)
- [Build &amp; flash](#build--flash)
- [First-time setup](#first-time-setup)
- [Use it from any computer (no repo required)](#use-it-from-any-computer-no-repo-required)
- [On-device controls](#on-device-controls)
- [How usage is counted](#how-usage-is-counted)
- [Power use](#power-use)
- [Troubleshooting](#troubleshooting)
- [Repository layout](#repository-layout) · [License &amp; credits](#license--credits)

## How it works

```
Claude Code (PC)  ──hook──▶  buddy_hook.py  ──HTTP POST /event (LAN)──▶  device
  SessionStart / UserPromptSubmit / PreToolUse / PostToolUse /            (Clawd +
  Stop / SessionEnd / Notification                                        dashboard)
```

Two halves, joined only by a token-authenticated HTTP call on your LAN:

- **Device (firmware).** Boots, joins your WiFi (captive-portal on first run),
  and runs a small `WebServer`: `POST /event` (a JSON status+stats snapshot,
  authenticated with an `X-Buddy-Token` header) and `GET /` (health). It renders
  the Clawd GIF pack from on-board flash (LittleFS) with `AnimatedGIF`. It is
  single-threaded; by default it's purely a display. (An **optional** opt-in adds
  on-device *tap-to-approve* for a pending tool call — off unless you register the
  `PermissionRequest` hook; see [tools/HOOKS.md](tools/HOOKS.md).)
- **PC (`tools/buddy_hook.py`).** A single self-contained Python script that
  Claude Code runs on each hook event. It figures out what Claude is doing, reads
  the session transcript for the usage rollup, and `POST`s it to the device.
  Stats events are **non-blocking and fail open**: if the device is unreachable
  the error is swallowed, so they can never slow down or break a Claude session.
  (The optional approval hook briefly waits for your tap and also fails open — on
  a timeout or an unreachable device it falls back to Claude's normal prompt.)

The device is the source of truth for its own auth token; the PC just needs to
know the device's IP + token (see [setup](#first-time-setup)).

## What it shows

**Character states** (the mascot in the middle):

| State | When | Look |
|---|---|---|
| `sleep` (ASLEEP) | offline, or no activity yet | calm, dim |
| `idle` (READY) | connected, no work running | resting |
| `busy` (WORKING) | Claude is working | a rotating set of "working" clips + a whimsical verb ("Pondering…", "Brewing…") that changes in sync with the animation. Tool-aware: editing, running, reading, delegating… |
| `attention` (NEEDS YOU) | the turn was handed back to you — a **Notification**, or **Stop** with nothing to do next | sticky alert; the LED nudge escalates the longer it waits |
| `celebrate` (DONE!) | a turn just finished (**Stop**) | brief celebration |
| `heart` (HELLO) | a new session started (**SessionStart**), or you pet Clawd (tap the character) | brief hello |
| `error` (OOPS) | a tool reported an error | brief wince |
| `dizzy` | triple-tap the screen | easter egg |

`celebrate` / `heart` / `error` are short reactions that play for a few seconds
(and wake the screen if it's off), then fall back to the normal state.
`attention` ("Needs you") is sticky until Claude resumes — or until you dismiss
it on the device. While it waits, the screen drops the stats card for a clean
alert (just the amber Clawd and a **Got it** button); tapping **Got it** drops
the device straight back to idle (LED off) until the next time Claude needs you.
Dismissing is local — it doesn't reply to Claude.

**Stats card** (bottom): two headline figures — **Today** and **Total** tokens —
over four compact counts: **Tools** (tool calls), **Turns** (assistant turns),
**Sess** (sessions today), **Time** (current session duration). The numbers
roll like an odometer when they change. The card has two pages sitting side by
side — **swipe left** and the **Trends card** slides in from the right: a bar
per day for the last 14 days (today in coral, still growing live) with a 7-day
total and daily average — the device keeps a 30-day history in flash, dated by
the PC so it needs no clock of its own. **Swipe right** to slide back; swiping
past the end just rubber-bands. A fuller, live-updating panel is under
long-press → **Settings → Stats** (adds project name, battery estimate, uptime,
free heap, IP).

**Ambient cues.** The onboard RGB LED speaks a colour language — a slow blue
breath while working (cooler/quicker as the session heats up), a gentle amber
breath when it needs you (escalating to hard blinks the longer it waits), red
on error, green when a turn lands, and a little magenta heartbeat while you pet
Clawd — silenced by the **Quiet** (Do Not Disturb) setting, and off whenever
the screen is asleep. Session intensity shows as 1–2 pips in the top bar, next
to a small **battery glyph** (the estimated charge on battery builds).
Set an optional daily token `"budget"` in `buddy.json` and the stats-card
divider becomes a usage gauge (coral → amber near the cap → red over).

## Hardware

**Reference board — ESP32-2432S028R "Cheap Yellow Display" (CYD):**

- ESP32-WROOM-32, 4 MB flash, no PSRAM.
- Display: **ILI9341** 240×320 — the dual-USB "CYD2USB" unit is ILI9341, *not*
  ST7789 (feeding it the ST7789 driver gives a white screen).
- Resistive touch (XPT2046), onboard RGB LED, light sensor (GPIO34), the BOOT
  key doubling as a runtime button, CH340 USB-serial.

It's the cheapest all-in-one board with a screen + touch (≈US$10), which is why
it's the default — but nothing about the app is CYD-specific.

### Power: wired or battery

The board wants **5 V**, over its micro-USB port or the `P1` header's VIN/GND
pins. Two ways to feed it:

- **Wired (simplest).** Any USB power source — a phone charger, a PC port, a
  power strip with USB. Nothing to configure. The top-bar battery glyph and the
  Settings **Battery** row assume the battery setup below; on wall power just
  ignore them.
- **Battery.** Reference setup: a **2000 mAh Li-ion cell + a cheap
  charge/discharge boost module** (the "charge + 5 V boost in one board" kind).
  The cell plugs into the module; the module's 5 V output feeds the CYD (its
  USB-A output into the CYD's micro-USB cable, or OUT+/OUT− wired to `P1`
  VIN/GND). No electrical changes to the CYD itself. Notes from the field:
  - **Charge the module's input port**, not the CYD's USB. Cheap modules'
    USB-C input usually lacks the CC handshake resistors, so a **USB-C PD
    charger with a C-to-C cable delivers nothing** (no LED, no charge) — use a
    USB-A charger / power-bank A-port with an A-to-C (or A-to-micro) cable.
  - **Charge with the buddy powered off** (Settings → Power off) if you want
    the module's "full" LED to be truthful — the running device's draw keeps
    cheap chargers from ever terminating.
  - The firmware ships a **software battery gauge** for exactly this setup:
    the device has no data path to the cell, so it estimates charge from its
    own consumption model (see `docs/battery-gauge-spec.md`). Top-bar glyph
    (amber &lt;20%, red &lt;10%) and a **Battery (est)** row in Settings →
    Stats. It's **fully automatic and death-anchored**: run the device until
    the cell actually dies (the module's protection board guards the cell;
    stats checkpoint every minute near the end), charge it, power it on — the
    gauge learns the cell's real capacity from each death and refills to 100%
    on the first boot after one. Mid-cycle top-ups are invisible to it, so
    the reading runs low until the next full die-charge-boot cycle — it's an
    estimate, treat it as one.

CYD is the target, but the firmware is a thin HAL (`src/hal/`: display, touch,
led, storage) over `TFT_eSPI`, and everything above it — networking, hooks,
stats, the GIF character system — is hardware-independent. To run it on another
ESP32 + TFT:

- **Display:** set the matching `*_DRIVER` flag and pins in `platformio.ini`
  (`TFT_eSPI` supports ILI9341 / ST7789 / ST7735 / ILI9488 / …). The character
  region and UI lay themselves out from `display.width()/height()`.
- **Touch (optional):** adjust the XPT2046 pins in `src/hal/touch.cpp`, or stub
  `hal::Touch` — touch only drives the Settings menu and the easter egg.
- **LED (optional):** `src/hal/led.cpp`; safe to no-op if your board has none.
- **Flash / partition:** the Clawd pack needs ~1.2 MB of LittleFS — size the
  data partition to your board's flash (drop some `busy_*` clips from the pack
  and manifest if you're tight).

The Clawd art is a plain GIF pack (`data/clawd/`, 120 px-wide, black background),
so you can drop in your own character without touching code.

## Build & flash

You need [PlatformIO](https://platformio.org/) (the `pio` CLI, or the VS Code
extension) and a USB cable to the board.

```bash
pio run -e cyd -t upload      # 1) firmware  -> app partition
pio run -e cyd -t uploadfs    # 2) GIF pack  -> LittleFS (data/clawd/)
```

Run both the first time (firmware *and* the filesystem image). After that,
re-flash only what changed — `upload` for code, `uploadfs` for new/edited GIFs.

> **Upgrading from a pre-OTA build (mid-2026 or earlier):** the partition
> layout changed to dual OTA slots, so this one flash must be over **USB** and
> must run **both** commands above — the LittleFS region moved, and firmware
> alone would boot to a missing character. WiFi credentials, the token and the
> touch calibration live in NVS and survive the migration.

### Reflash over WiFi (OTA)

Once the OTA layout is on the board, the cable is optional. Put your device
token (the one in `buddy.json`) into the `--auth=` flag of the `cyd-ota`
environment in `platformio.ini` (don't commit it), then:

```bash
pio run -e cyd-ota -t upload      # firmware over WiFi
pio run -e cyd-ota -t uploadfs    # GIF pack over WiFi
```

The device shows a progress bar while it updates and reboots by itself; if the
transfer fails it just reboots back into the old firmware.
The display driver is a build flag (`ILI9341_2_DRIVER` in `platformio.ini`); on a
different panel that shows a white or garbled image, switch to your controller's
driver/colour-order flags (e.g. `ST7789_DRIVER` + `TFT_RGB_ORDER=TFT_BGR`).

> **First build on a slow/blocked network.** The initial espressif32 toolchain +
> framework download can stall. If it does, fetch those archives out-of-band
> (e.g. a parallel, resumable downloader) and point PlatformIO at them with
> `platform_packages = …@file://…` in `platformio.ini`.

## First-time setup

1. **Flash** firmware + filesystem (above). On first boot the device shows
   **"Join WiFi hotspot: Claude-CYD-Setup"**.
2. **Join WiFi:** connect a phone/PC to the `Claude-CYD-Setup` hotspot, pick your
   network and enter its password (captive portal). The device reboots onto your
   WiFi and remembers it.
3. **Read its address + token:** the device shows its **IP** and a **token** on
   screen (also any time under long-press → **Settings → Stats**). The token is a
   random secret generated on the device.
4. **Tell your PC where the device is** — `~/.claude/buddy.json`:
   ```json
   { "ip": "claude-cyd.local", "token": "<device token>" }
   ```
   `claude-cyd.local` is the device's mDNS name and keeps working when DHCP
   changes its address (Windows 10+/macOS resolve `.local` natively); use the
   literal IP instead if your network blocks mDNS.
5. **Register the hooks** in `~/.claude/settings.json` so Claude Code drives the
   device. Full snippet + explanation: **[tools/HOOKS.md](tools/HOOKS.md)**.

That's it — start a Claude Code session and Clawd should wake up.

## Use it from any computer (no repo required)

**The flashed device is fully standalone.** Firmware and the animation pack live
in its own flash; it needs no PC, no repo, and no cloud — it just boots, joins
your WiFi, and waits for events. Any computer on the network can then drive it.

To drive it from a machine that **doesn't have this repository**, you only need
the **one** self-contained helper file — `buddy_hook.py` depends on nothing but
the Python 3 standard library and reads only `~/.claude/buddy.json` /
`~/.claude/buddy_tokens.json` (never the repo). So:

1. **Copy the single file** `tools/buddy_hook.py` to that machine — a good
   repo-independent home is `~/.claude/buddy_hook.py`.
2. **Create `~/.claude/buddy.json`** with the device `ip` + `token` (from the
   device's Settings → Stats).
3. **Add the hooks** to that machine's `~/.claude/settings.json`, with the
   command pointing at wherever you put the file, e.g.
   `python "~/.claude/buddy_hook.py"` (see [tools/HOOKS.md](tools/HOOKS.md) for
   the full block).

Requirements: **Python 3 on `PATH`**, and the machine must be able to **reach the
device** (same WiFi is simplest; off-network works via a mesh VPN such as
Tailscale with a subnet router advertising the device's LAN). `buddy_tokens.json`
is created automatically on first run.

Each machine keeps its **own** counts (`buddy_tokens.json` is per-machine, not
merged); if two machines push at once, the device shows whichever pushed last.

## On-device controls

- **Tap** while asleep — wake the screen.
- **Tap Clawd** — pet the character: a brief `heart` hello.
- **Swipe left / right** — slide the bottom card between **stats** (left page)
  and **trends** (right page); swiping past the end rubber-bands. Returns to
  stats when the screen next sleeps.
- **Tap "Got it"** on the *Needs you* screen — dismiss the nudge: the device
  drops back to idle (LED off) until the next time Claude needs you.
- **Triple-tap** — `dizzy` easter egg.
- **BOOT key** (the physical button next to RST) — short press wakes the screen
  or taps **Got it** for you; holding it toggles **Quiet** (one red blink = on,
  green = off). Handy when tapping the resistive panel is inconvenient.
- **Long-press (~1 s)** — open **Settings**: **Power off** (top row, in red —
  deep sleep: screen, LED and WiFi off; tap the screen or press the board's
  **RST** button to turn it back on), **Stats** (full live panel),
  **Quiet** (on/off Do Not Disturb — silences the RGB LED and stops the screen
  auto-waking for nudges; only your touch wakes it), **Brightness** (cycle the
  backlight 100 / 70 / 40 % / **auto** — auto night-dims to 25% when the onboard
  light sensor says the room went dark, and eases back up when the lights come
  on), **Recalibrate** (3-point touch calibration; times out safely
  if you walk away), **WiFi setup** (re-open the captive portal — keeps the saved
  password unless you enter a new network), **Close**. Quiet and brightness
  persist across reboots.
- Auto **screen-off after 30 s** of calm — or **3 min while Claude is working**,
  so long grinds go dark too; a touch, a fresh turn starting, or a nudge wakes
  it. After **an hour** with no touch and no Claude activity at all the device
  deep-sleeps itself (tap to wake).

## How usage is counted

`buddy_hook.py` reads the current session's transcript and rolls up the day:

- **Tokens** = `input + output + cache_creation`. It deliberately **excludes
  `cache_read`** — that's the cached context re-read on *every* turn, which on a
  long session is ~95%+ of the raw token throughput and would balloon "today" to
  absurd numbers without reflecting real use.
- Assistant messages are **de-duplicated by id** (the transcript re-logs a
  message several times as it streams), so tokens / turns / tool-calls aren't
  double-counted.
- The scan is **incremental**: each event resumes from a per-session byte
  offset (kept in `buddy_tokens.json`) and parses only what was appended, so
  hooks stay fast even when a long session's transcript reaches tens of MB.
- **Today** counts persist in `~/.claude/buddy_tokens.json` and reset at local
  midnight; the previous day rolls into the **all-time** total. A session that
  spans midnight isn't double-counted.

## Power use

Ordered by how much they save (all automatic):

- **Auto screen-off.** The backlight is by far the largest draw. 30 s idle when
  calm; **3 min while Claude is working** (so a marathon turn goes dark instead
  of burning the backlight for an hour — the LED events and any fresh turn
  still relight it). While off, the CPU drops 240 → **80 MHz** and the idle
  loop throttles to ~25 Hz; both jump back on wake.
- **Auto deep sleep.** After **1 hour** with no touch *and* no hook events the
  device powers itself fully off — on the battery setup that's ~10 mA
  (including the boost module's idle draw) instead of ~100 mA idling dark.
  Tap the screen to wake. WiFi can't wake a deep-sleeping board, which is why
  the leash is a full hour: any Claude activity inside it still lights the
  screen the moment work starts.
- **WiFi modem-sleep.** The radio dozes between beacons. Because power-save can
  drop the link on some APs, reconnection is aggressive: a ladder in the
  background (`reconnect()` → full supplicant restart) **plus** a fast burst
  fired the moment you wake the screen. (Modem-sleep can also make the first
  OTA invitation go unanswered — just retry the upload.)

For a manual off, **Settings → Power off** deep-sleeps the same way. On
battery the device deliberately runs until the cell's protection cuts power —
that brownout is what calibrates the gauge — checkpointing stats every minute
once the estimate reads ≤3%. Either way a screen tap or the **RST** button
cold-boots straight back into the dashboard.

## Troubleshooting

- **White / garbled screen** — wrong display driver. The CYD2USB unit is
  **ILI9341**, not ST7789; check the `*_DRIVER` flag in `platformio.ini`.
- **Stuck on "Join WiFi hotspot"** — connect to `Claude-CYD-Setup` and complete
  the captive portal; re-open it later via Settings → WiFi setup.
- **Numbers never update** — the device shows what it last received. Check
  `buddy.json` (ip/token match the device), that the hooks are registered, that
  Python 3 is on `PATH`, and that the PC can reach the device IP.
- **IP changed** — point `buddy.json` at `claude-cyd.local` and it stops
  mattering; with a literal IP, update it (or give the device a static DHCP
  lease).
- **OTA upload rejected ("Authentication failed")** — the `--auth=` value in
  the `cyd-ota` env must be the device token from `buddy.json`.
- **OTA says "No response from device"** but the device pings fine — WiFi
  modem-sleep missed the first UDP invitation (common when the screen is off).
  Just run the upload again; the retry almost always lands.
- **Charging does nothing (battery setup)** — don't use a USB-C PD charger
  with a C-to-C cable on a cheap charge module (no CC resistors → no power);
  use a USB-A source. And charge the module's input, not the CYD's USB.
- **Battery reading looks wrong** — normal after mid-cycle top-ups (charging
  is invisible to the gauge). It re-syncs itself on the next full
  die → charge → power-on cycle.

## Repository layout

```
src/            firmware: main.cpp (orchestrator), app/ (state tables, LED
                language, NVS store, power, battery gauge), ui/ (theme, text,
                widgets), screens/ (home, trends, card slide, stats, settings,
                wifi, ask), hal/ (display, touch, led, storage), net/ (server),
                render/ (Clawd GIF)
data/clawd/     Clawd GIF character pack (flashed as the LittleFS image)
assets/         README preview GIFs
tools/          buddy_hook.py + HOOKS.md (the single-file PC helper + setup)
docs/           design notes
platformio.ini  build configuration
```

## License & credits

- **Code & tooling** (firmware + `tools/buddy_hook.py`): **MIT** — see
  [LICENSE](LICENSE). © 2026 Qiankang Wang.
- **Clawd character art** (`data/clawd/` and `assets/`): **not MIT.** "Clawd" is
  the property of **Anthropic, PBC**; all rights reserved. The pixel sprites are
  adapted from [rullerzhou-afk/clawd-on-desk](https://github.com/rullerzhou-afk/clawd-on-desk)
  (source code AGPL-3.0; artwork all-rights-reserved). Swap in your own
  black-background GIF pack to redistribute the project freely.
- **Concept & event model:** inspired by Anthropic's maker reference
  [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
  (MIT), reproduced here over WiFi + Claude Code hooks instead of Bluetooth.
- **CYD pinouts & community:** [witnessmenow/ESP32-Cheap-Yellow-Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display) (MIT).

> **Disclaimer.** This is an unofficial, personal fan project. It is **not
> affiliated with, sponsored by, or endorsed by Anthropic.** "Claude" and
> "Clawd" are trademarks/IP of Anthropic, PBC, used here only to interoperate
> with Claude Code for a non-commercial maker build.
