# CYD Claude Buddy — 2026-07 Overhaul Design

Status: Phase 0–1 approved (user selected "code refactor" as an overhaul goal);
Phase 2–3 scope pending user sign-off (see Open decisions).

## Goals

The project accumulated a dozen features in one file. This overhaul has three
lines, executed in phases so each is independently flashable and verifiable:

1. **Refactor** — split the 1278-line `src/main.cpp` into focused modules with
   zero behavior change; remove dead code and doc drift.
2. **Features** — a selected set from: mDNS-based config (no hardcoded IP),
   OTA updates, usage-history chart, sound alerts, touch interactions.
3. **UI** — swipeable multi-card home screen (current home layout preserved
   as-is; new cards slide in beside it).

## Non-goals / invariants (do not change)

- Animation behavior: clips play at native pace, smooth looping, clip switch
  every ~6 s (+ jitter), verb rotation tied to clip switches, headline-only
  repaint (`renderHeadline`) so the stats grid never flickers.
- Token accounting in `tools/buddy_hook.py`: dedupe by message id; tokens =
  input + output + cache_creation, **excluding** cache_read.
- The busy GIF assets (no re-rendering; `.scratch-busy/clawd.js` is stale).
- `buddy_hook.py` stays a single self-contained stdlib-only file (it is
  copied standalone to other machines).
- Single-threaded firmware model: HTTP serviced inside `loop()`, no locking.

## Phase 0 — Clear the decks (no behavior change)

- Delete `src/ble/` (shelved BLE transport; excluded from build since the WiFi
  pivot; recoverable from git history). Drop the `build_src_filter` exclusion.
- Fix doc drift:
  - `docs/DESIGN.md` still says "no on-device approval"; the `/ask` +
    `/decision` opt-in approval flow is back. Align with README's wording.
  - `src/net/server.h` header comment lists only `POST /event` and `GET /`.
  - `platformio.ini` line 4 comment says "CYD2USB variant -> ST7789" — the
    panel is ILI9341 (the ST7789 driver gives a white screen).
- Remove the `[env:native]` test env: it references tests that were never
  added ("M4"), and nothing under `test/` exists.

## Phase 1 — Modularize the firmware (no behavior change)

Target layout; `main.cpp` shrinks to setup + loop orchestration (~250 lines):

```
src/
  app/
    activity.{h,cpp}     state tables + pure logic: isWork, actTimeout,
                         actVerb, stateColor/Label, WHIMSY/IDLE_MSGS,
                         intensityTier
    led_language.{h,cpp} driveLed(state, now, intensity, waitStart, silenced)
    store.{h,cpp}        NVS: token, stats snapshot (StatsBlob save/restore),
                         prefs (dnd, brightness)
    power.{h,cpp}        screen sleep/wake, pre-sleep dim, CPU clock,
                         deep-sleep power-off
  ui/
    theme.h              palette (C_CORAL et al) + shared font choices
    text.{h,cpp}         gtext, textW, gtextClamp, blitText, fmtTok, fmtDur
    widgets.{h,cpp}      Rect, inRect, drawButton
  screens/
    home.{h,cpp}         status bar, headline, stats grid + odometer state,
                         budget bar, intensity pips, "Got it" ack button
    stats_panel.{h,cpp}  the full Stats panel (renderStats)
    settings.{h,cpp}     settings menu render + tap dispatch
    wifi_confirm.{h,cpp} WiFi-portal confirm screen
    ask.{h,cpp}          "Allow this tool?" approval screen
  hal/  net/  render/    unchanged
```

Notes:

- Modules keep the existing embedded style: file-static state behind a small
  header API. UI modules receive `hal::Display&` once at init.
- Mode flags (`settingsOpen`, `askOpen`, …) and the touch gesture pipeline
  (tap / triple-tap / long-press / release debounce) stay in `main.cpp` — the
  loop is the dispatcher; screens expose `render*` and hit-test helpers.
- Extraction order (each step compiles): theme → text → widgets → activity →
  store → led_language → screens (one per commit) → power.
- Verification: `pio run -e cyd` clean after each step; final on-device check
  by the user (animation pacing, verb rotation, panels, odometer, sleep/wake).

## Phase 2 — Features (scope pending user)

Candidates, with feasibility already checked:

- **mDNS instead of IP** — device already announces `claude-cyd.local`; teach
  `buddy.json`/docs to use it (Windows resolves .local natively). Small.
- **OTA updates** — requires repartition: current app is 1.02 MB; dual app
  slots of 1.31 MB each + 1.31 MB LittleFS (pack is 1.19 MB) fit 4 MB flash
  with the existing `nvs` region untouched (WiFi creds / token / calibration
  survive). One-time USB reflash of firmware + filesystem, then `pio` uploads
  over WiFi. Medium.
- **Usage-history chart** — device gains NTP time; NVS ring of 30 daily token
  totals; 14-day bar chart in the Stats panel (or a home card, see Phase 3).
  Medium.
- **Sound alerts** — short tones on needs-you / done via DAC GPIO26; muted by
  Quiet. Requires a small speaker plugged into the P4 connector (user must
  confirm hardware). Small–medium.
- **Touch interactions** — low-cost easter eggs (e.g. pet the head → heart).
  New GIF clips are out of scope (separate project via the desktop Studio
  pipeline).

## Phase 3 — UI (direction pending user)

Recommended: swipeable multi-card home. The tuned home screen stays untouched
as card 1; horizontal swipe pages to a trends card / full stats card. Optional
surface polish (budget ring, iconified labels) on top.

## Open decisions (blocking Phase 2/3 only)

1. Overall approach A (phased, recommended) vs B (one branch, everything) vs C
   (features first).
2. Which Phase 2 features to include.
3. UI direction: multi-card / multi-card + polish / none this round.
4. Touch-interaction scope: cheap easter eggs only, or none.
5. Speaker hardware present for sound alerts?
6. Local `.scratch-busy/` directory: keep or archive (git-ignored either way).

## Error handling & testing

- Refactor is verified by compilation after every extraction step plus a
  behavior walkthrough on the device (flash deferred to the user's session).
- Existing runtime safeguards preserved verbatim: activity watchdog timeouts,
  out-of-order event drop by `ts`, NVS write throttling, sprite-allocation
  fallbacks, WiFi reconnect ladder, fail-open hook semantics.
- No unit-test scaffolding is added in Phase 1 (the dead `[env:native]` env is
  removed); if tests are wanted later they should target `buddy_hook.py`'s
  pure functions (transcript scan, day rollup) with pytest on the host.
