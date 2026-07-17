# BLE Migration — replace WiFi transport with Bluetooth LE

**Date:** 2026-07-16
**Status:** approved design, pre-implementation

## Why

WiFi is the pain point of the current architecture, on all four axes:

- **Re-provisioning when the device moves** (captive portal per venue; hopeless on
  campus/portal networks).
- **Idle power** (~143 mA base is dominated by WiFi; hurts the battery build).
- **Architecture weight** (WiFiManager portal + WebServer + token header + IP
  config in `buddy.json` for what is, at heart, a one-way stats feed).
- **IP instability** (DHCP churn, "is it reachable" debugging).

BLE removes the network as a concept: the device advertises, the PC connects.
No SSID, no IP, no portal, anywhere.

## Architecture

```
Claude Code hooks
      │  (unchanged hook contract, all async/fail-open)
      ▼
tools/buddy_hook.py
      │  HTTP POST http://127.0.0.1:8787  (was: http://<device-ip>)
      ▼
tools/buddy_bridge.py          ← NEW, on-demand resident process
      │  BLE GATT (bleak / WinRT)
      ▼
ESP32 CYD — NimBLE GATT server ← replaces WiFiManager + WebServer + ArduinoOTA
```

The bridge **replicates the device's exact HTTP surface** (`POST /event`,
`POST /ask`, `GET /decision`, `GET /` health) on localhost, so `buddy_hook.py`
keeps its request/response contract byte-for-byte and only retargets the host.

## 1. Bridge lifecycle — event-driven, zero autostart

Constraint from the user: the laptop must gain **no startup entries and no
standing drain**. A background "presence scanner" would itself burn battery, so
presence detection is replaced by demand detection:

- **Spawn:** `buddy_hook.py` POSTs to `127.0.0.1:8787`; on connection-refused it
  spawns `pythonw buddy_bridge.py` (DETACHED_PROCESS | CREATE_NO_WINDOW),
  drops the current event, and returns. Dropped events are harmless — every
  event carries the full stats snapshot, so the next one heals the display.
- **Singleton:** the bridge binds `127.0.0.1:8787` on startup; bind failure
  means another instance is alive → exit silently. The port is the mutex.
- **Radio states:**
  - `SCANNING` — active BLE scan (filtered on the service UUID) for up to 90 s.
  - `CONNECTED` — no scanning; forward traffic. On disconnect → `SCANNING`.
  - `DORMANT` — scan gave up (device off/away). Radio quiet; while events keep
    arriving, retry a 10 s scan every 5 min.
- **Exit (single condition):** no HTTP request for **10 minutes** → disconnect
  BLE, exit. Claude idle ⇒ laptop is clean.

Net effect: Claude not running → zero processes. Claude running but device
absent → one near-idle process, radio mostly silent. Device present → bridge
holds one BLE connection (far cheaper than scanning).

## 2. GATT protocol

One service, two characteristics (128-bit UUIDs, fixed constants):

- Service   `177b0001-6f32-4ea3-b878-866e7628de1f`
- `ingress`  `177b0002-6f32-4ea3-b878-866e7628de1f`
- `decision` `177b0003-6f32-4ea3-b878-866e7628de1f`

(Frozen constants — random base `177b2aec-…de1f` with the low 16 bits of the
first field replaced by a 0001/0002/0003 index.)

- `ingress` — **Write (with response).** JSON envelope from the bridge:
  `{"k":"event"|"ask", "tok":"<device token>", "d":{...original HTTP body...}}`.
  Writes larger than the negotiated MTU (target 512) ride ATT long writes —
  no custom framing. Typical event payload is ~300 B, single packet.
- `decision` — **Notify + Read.** Device pushes
  `{"askId":N, "decision":"allow"|"deny"}` when the user taps; Read returns the
  latest value as a poll fallback.

**Auth:** no bonding/pairing. The existing device token moves into the envelope
(`tok`); the device drops envelopes with a bad token. Accepted risks for a desk
toy with ~10 m radio range: a nearby attacker could occupy the connection slot
(DoS) but cannot spoof events without the token. The hook keeps sending
`X-Buddy-Token` to the bridge; the bridge copies it into the envelope.

**Ask/decision flow:** hook POSTs `/ask` → bridge forwards envelope (bypasses
event coalescing) and subscribes to `decision` notifies → hook polls
`GET /decision` on the bridge exactly as it used to poll the device. Device
disconnected → `/ask` returns 502 → hook returns "" → **fails open** to
Claude's normal permission prompt, same UX as an unreachable device today.

## 3. Device firmware

- **Add:** `NimBLE-Arduino` (`h2zero/NimBLE-Arduino@^1.4`, known good on
  Arduino core 2.x). New `src/net/ble.{h,cpp}` replaces `src/net/server.{h,cpp}`.
  `AppState` stays as-is. NimBLE callbacks run on the NimBLE host task, so
  writes are pushed onto a small FreeRTOS queue and **applied in the `loop()`
  thread** — preserves the existing "single-threaded AppState, no locking"
  contract that the WebServer version relied on.
- **Advertise** whenever not connected. Deep sleep = no advertising (same
  reachability as WiFi today — touch wakes it, then it advertises again).
- **Delete:** WiFiManager (+ portal flow), WebServer, ArduinoOTA (+ on-screen
  OTA progress UI), mDNS, `screens/wifi_confirm.*`, `nudgeReconnect()`,
  NVS WiFi-credential handling. Settings→Stats shows BLE name / connection
  state / token instead of IP.
- **Status dot** in the top bar now means "bridge connected over BLE".
- **Battery model:** `app/battery` base-current constant drops from 143 mA to a
  ~90 mA initial guess; the death-anchored capacity calibration is untouched
  and will converge on its own.
- Rendering, animations, stats ring, touch, LDR dimming, BOOT-key, card slide:
  **all untouched.**

## 4. Partitions & migration (one USB flash, data survives)

New `partitions.csv` — factory-only layout, **nvs and littlefs keep their exact
offsets**, so the token, touch calibration, battery calibration (`bcap`/`bused`),
the 30-day stats ring, and the GIF pack all survive:

```
# Name,     Type, SubType,  Offset,   Size
nvs,        data, nvs,      0x9000,   0x5000
app0,       app,  factory,  0x10000,  0x2A0000   # 2.625 MB (was 2×1.31 MB OTA)
littlefs,   data, spiffs,   0x2B0000, 0x150000   # unchanged offset+size
```

(0x10000 + 0x2A0000 = 0x2B0000; 0x2B0000 + 0x150000 = 0x400000 = 4 MB. The old
`otadata` region becomes a dead gap — harmless.)

- **Migration = one `pio run -e cyd -t upload` over USB (COM5).** No `uploadfs`,
  no re-provisioning, no NVS erase. Firmware updates are USB-only from here on
  (device lives on the desk; espota + the second app slot are removed by
  design — flash headroom goes from ~278 KB to ~1.5 MB, which also absorbs
  NimBLE's ~300–400 KB comfortably vs the ~400–600 KB the WiFi stack frees).
- `platformio.ini`: remove `[env:cyd-ota]`, remove the WiFiManager git dep,
  add NimBLE-Arduino.

## 5. PC side

- **`tools/buddy_bridge.py`** — new. Dependencies: `bleak` (the only new pip
  install) + stdlib. HTTP server thread + asyncio BLE loop, connected by a
  queue. **Event coalescing:** if events back up while (re)connecting, keep
  only the newest (snapshot semantics — stale events have zero value). `/ask`
  bypasses coalescing. Optional `--listen 0.0.0.0` turns the bridge into the
  single ingress for other machines (Tailscale to the laptop replaces "route
  to the ESP32"); default stays localhost-only.
- **`tools/buddy_hook.py`** — retarget host to `127.0.0.1:8787`, add
  spawn-on-refused (fire-and-forget). Everything else — incremental transcript
  scan, `buddy_tokens.json`, dedupe, fail-open behavior — untouched.
- **`~/.claude/buddy.json`** — `{"token": "..."}` (+ optional `"port"`), `ip`
  field retired.
- **Docs:** README setup/troubleshooting rewritten (no provisioning section;
  add "restart Windows Bluetooth" remedy; OTA section → USB flashing),
  `tools/HOOKS.md` multi-machine section updated to point at the bridge.
  Placeholders only — no real tokens/paths (public repo).

## Error handling summary

| Failure | Behavior |
|---|---|
| Bridge not running | hook spawns it, drops one event; next event heals |
| Device off / out of range | bridge dormant (radio quiet); events acknowledged, only the newest kept for delivery on reconnect |
| `/ask` while disconnected | 502 → hook fails open to the normal prompt |
| Windows BT stack wedged after sleep | bridge reconnect loop; documented remedy |
| Bridge crash | next hook event respawns it |
| Two bridges race | port bind is the mutex; loser exits |
| Claude idle 10 min | bridge exits; laptop has zero footprint |

## Out of scope

BLE OTA (USB flashing chosen instead), WiFi/BLE dual mode, bonding/pairing UX,
iOS/Android companions, changes to stats computation or any rendering/UX.

## Success criteria

1. Hooks drive the buddy end-to-end with **zero WiFi code in the firmware**.
2. No autostart entries; bridge self-terminates ≤ 10 min after the last event.
3. Ask → tap → decision round-trip works over BLE.
4. After the migration flash: token, touch cal, battery cal, stats ring, and
   GIF pack are all intact (no uploadfs, no re-provisioning).
5. Measured base current meaningfully below the WiFi build's ~143 mA.
