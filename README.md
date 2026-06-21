# CYD Claude Buddy

An official-style **Claude desk buddy** (the orange *Clawd* mascot) running on a
**Cheap Yellow Display** (ESP32-2432S028R). It reacts to your real Claude Code
sessions and lets you **approve / deny tool calls from the device** — over
plain WiFi + Claude Code hooks, with **no BLE and no always-on PC process**.

> Why not the official Hardware Buddy (BLE)? That feature isn't exposed in the
> Claude desktop app build available here, so this project reproduces the same
> experience over a self-hosted transport: a tiny HTTP server on the device +
> Claude Code hooks on the PC. See [docs/superpowers/specs](docs/superpowers/specs/).

## What it does
- Shows the animated **Clawd** character reacting to Claude's state:
  `sleep` (idle/asleep) · `idle` · `busy` (working) · `attention` (approval
  pending, LED blinks) · `heart` (approved in <5s) · `celebrate` (level-up every
  10 approvals) · `dizzy` (triple-tap, replaces the official "shake").
- On a permission prompt: shows the tool + args and big **Deny / Approve**
  touch buttons; your tap is returned to Claude Code as the permission decision.
- 30s auto screen-off (kept on during approvals); tap to wake.
- Local stats (approvals / denials / level) on the idle screen.

## Hardware
- **ESP32-2432S028R "Cheap Yellow Display"** — ESP32-WROOM-32, 4 MB flash, no PSRAM.
- Display: **ILI9341** 240×320 (this dual-USB unit is ILI9341, *not* ST7789).
- Resistive touch: XPT2046. RGB LED, CH340 USB-serial.

## Architecture
```
Claude Code (PC) --hook--> buddy_hook.py --HTTP/LAN--> CYD (HTTP server)
   PreToolUse  ───────────────────────────────────────►  shows prompt
   approve/deny ◄──────────── GET /decision ◄──────────  you tap on screen
```
- Device: `WiFiManager` captive-portal provisioning + a `WebServer`
  (`POST /event`, `GET /decision`, `X-Buddy-Token` auth) + `AnimatedGIF`
  rendering the Clawd pack from LittleFS. Single-threaded loop.
- PC: `tools/buddy_hook.py` invoked by Claude Code hooks (see `tools/HOOKS.md`).

## Build & flash (PlatformIO)
```bash
pio run -e cyd -t upload      # firmware
pio run -e cyd -t uploadfs    # Clawd GIF pack (data/clawd -> LittleFS)
```
- Board driver is a build flag (`ILI9341_2_DRIVER`); if the screen is white/
  garbled on a different unit, try `ST7789_DRIVER` (+ `TFT_RGB_ORDER=TFT_BGR`).
- **China / slow proxy:** the first build's toolchain/framework download can stall
  (`IncompleteRead`). Pre-fetch them in parallel and feed pio local archives —
  see [platformio-cn-slow-download note] in the dev memory; `../pio-pkgs/dlpar.sh`
  + `platform_packages = …@file://…` in `platformio.ini`.

## Set up the device (one time)
1. Flash (above). On first boot the CYD shows **"Join WiFi hotspot: Claude-CYD-Setup"**.
2. Connect a phone/PC to `Claude-CYD-Setup`, pick your WiFi, enter the password.
3. The CYD shows its **IP** and a **token** — note both.

## Wire up Claude Code (one time)
1. `~/.claude/buddy.json`: `{ "ip": "<device ip>", "token": "<device token>" }`
2. Add the hooks to `settings.json` — see **[tools/HOOKS.md](tools/HOOKS.md)**.
   Status hooks (global) make the buddy reflect activity; a `PreToolUse` hook
   (scoped to a project) routes tool approvals to the device.

## Credits
- Protocol & concept: [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
- *Clawd* / *Cloudling* sprite art: [rullerzhou-afk/clawd-on-desk](https://github.com/rullerzhou-afk/clawd-on-desk);
  Clawd GIF pack via [TaoXieSZ/claude-code-buddy](https://github.com/TaoXieSZ/claude-code-buddy)
- CYD pinout/config: [witnessmenow/ESP32-Cheap-Yellow-Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)

## Limitations
- Data fidelity is bounded by what hooks expose: token counts / transcript /
  owner name from the official BLE snapshot aren't available, so `celebrate`
  is driven by an approval count rather than tokens.
- LAN transport is plain HTTP guarded by a shared token (fine on a trusted home
  network; the air is already WPA2-encrypted). No TLS.
- The BLE implementation (`src/ble/`) is shelved (excluded from the build).
