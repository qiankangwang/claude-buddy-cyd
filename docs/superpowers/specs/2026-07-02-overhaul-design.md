# CYD Claude Buddy — 2026-07 Overhaul Design

Status: **all phases implemented** on the `overhaul` branch. The user
delegated the Phase 2–3 scope ("你看着做"); the decisions taken are recorded
under Open decisions below. Pending: on-device verification by the user
(USB flash of the new partition layout), then merge to main.

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
    power.{h,cpp}        deep-sleep power-off (the screen sleep/dim/CPU-clock
                         sequencing is control flow of the loop itself and
                         stays in main.cpp — moving it would mean threading
                         five flags through a module boundary for no gain)
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
- Measured cost of the split: RAM unchanged (+104 B), flash +~37 KB (54.4% →
  56.3% of the app partition) from losing cross-TU inlining. Acceptable; if
  flash ever gets tight, `-flto` in `build_flags` claws most of it back.

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

## Open decisions — resolved (scope delegated by the user)

1. Overall approach: **A (phased)**, as recommended.
2. Phase 2 features: **mDNS config** (docs-only — the hook already accepts a
   hostname), **OTA** (dual 0x150000 slots; LittleFS moved to 0x2B0000; one
   USB migration flash), **usage history** (hook sends the local date; device
   keeps a 30-day NVS ring), **pet-the-character easter egg**. **Sound alerts
   skipped** — needs a speaker on the P4 connector that the user hasn't
   confirmed owning; revisit when the hardware exists.
3. UI: **two-page bottom card** (stats ⇄ trends via horizontal swipe). The
   tuned home layout is untouched as page 0; Clawd and the top bar never move.
   Card resets to page 0 when the screen sleeps.
4. Touch interactions: cheap easter egg only (tap Clawd → heart, fired on a
   still release so swipes and menu-closing taps don't trigger it; a
   triple-tap's dizzy outranks it).
5. `.scratch-busy/`: left as-is locally (git-ignored; user's call whenever).

## Error handling & testing

- Refactor is verified by compilation after every extraction step plus a
  behavior walkthrough on the device (flash deferred to the user's session).
- Existing runtime safeguards preserved verbatim: activity watchdog timeouts,
  out-of-order event drop by `ts`, NVS write throttling, sprite-allocation
  fallbacks, WiFi reconnect ladder, fail-open hook semantics.
- No unit-test scaffolding is added in Phase 1 (the dead `[env:native]` env is
  removed); if tests are wanted later they should target `buddy_hook.py`'s
  pure functions (transcript scan, day rollup) with pytest on the host.
