# Battery gauge (software-estimated) — design spec

Status: approved. Date: 2026-07-03.

## Problem

The buddy now runs from a 2000 mAh Li-ion cell behind a generic charge+boost
module. The module has no data interface and the user wants **zero hardware
changes**, so the true state of charge is unreadable (the boost + LDO hold the
rails flat until the battery dies). The device should still give a useful
"time to recharge" signal.

## Approach: consumption-model fuel gauge

Integrate the device's own estimated draw over time and count down from a
known full charge. This is an *estimate*, not a measurement — the UI copy and
this spec both treat it as such.

- `used_mAh += I(state) * dt`, ticked every 10 s in the main loop.
- `percent = 100 * (1 - used / USABLE_MAH)`, clamped to 0..100.
- "Charged" is a manual event: the user taps a Settings row after charging;
  there is no way to detect the charger electrically.

### Current model (constants at top of `src/app/battery.cpp`, all tunable)

Battery-side mA at 3.7 V nominal, boost efficiency folded in
(I_batt ≈ I_5V × 5.0 / (3.7 × 0.85)):

| State                     | Est. draw |
|---------------------------|-----------|
| Screen on (backlight b %) | 143 + 95 × b/100 mA |
| Screen off, loop running  | 143 mA    |
| Deep sleep (powered off)  | 10 mA     |

- `CAPACITY_MAH 2000`, `USABLE_FRACTION 0.85` → 1700 mAh usable.
  Full-brightness runtime ≈ 7 h, which matches the class of hardware.
- Backlight percent comes from the same `effectiveBright()` the display uses
  (auto-dim nights are cheaper and the gauge should know it).
- Constants are first-guess numbers; calibrate later with a USB power meter
  if the estimate drifts badly. Getting within ±20 % is the bar.

### Deep-sleep accounting

`powerOff()` stamps the RTC-persistent clock (`gettimeofday`, which ESP-IDF
restores across deep sleep) into `RTC_DATA_ATTR` memory before
`esp_deep_sleep_start()`. Deep-sleep wake is a cold boot; at `begin()`, if the
stamp is valid, charge the elapsed time at the deep-sleep rate. RTC RC drift
(~5 %) is irrelevant at this accuracy.

### Auto-calibration (added 2026-07)

Charging stays invisible (no signal path), but a genuine flat battery IS a
ground-truth event: the boost output sags and the ESP32 dies by **brownout**.
On the next boot, `esp_reset_reason() == ESP_RST_BROWNOUT` with a substantially
discharged gauge (> 40% of usable counted) means "the cell was actually empty
when we died" — so the counted mAh at that moment is the cell's real usable
capacity at the modeled rates. It is blended into a learned capacity
(50/50 EMA, NVS key `bcap`, sanity floor 200 mAh) and the gauge reads 0% until
the next manual "Charged". The counter may overrun the learned capacity (up to
2x, percent clamps at 0): if the model is pessimistic, a user who keeps waking
the device past the low-battery shutdown until it truly dies gives the learner
an above-capacity sample, which *raises* the estimate — so calibration works
in both directions. A wall-power brownout glitch can't poison the estimate
below the 40% discharge guard.

### Persistence

- NVS key `batt_used` (mAh × 10, uint32). Saved by piggybacking the existing
  `saveStatsIfChanged` cadence (no new wear pattern), plus force-saves in
  `powerOff()` and at the low-battery shutdown.
- First boot / missing key = assume full (100 %).
- OTA reflash keeps NVS, so the gauge survives updates.

## UI

- **Status bar glyph**: small battery outline (~20×10 px) right of the label
  band, left of the intensity pips; label band shrinks from `W-60` to `W-84`.
  Fill level in 5 % buckets; repaint only on bucket/color change (same
  no-flicker rule as the rest of the bar).
  Colors: `C_MUTED` ≥ 20 %, budget-bar amber < 20 %, `C_NO` red < 10 %.
- **Stats panel** (Settings → Stats): one line, e.g.
  `Battery ~63% · ≈4.2h left (est)`.
- **Settings row**: `Battery: Charged ✓` — tap = reset gauge to 100 %
  (persist immediately). This is the only way to tell the device it charged.
- **Low-battery shutdown**: at ≤ 5 % force-save stats/history and enter the
  existing `powerOff()` deep-sleep screen with copy "Battery low — charge me"
  (protects the stats and stops the boost from flat-draining the cell).
  **Boot grace period**: the shutdown check is armed only 3 min after boot,
  so a freshly-charged device that still *reads* ≤ 5 % can be woken and its
  gauge reset via Settings → Charged instead of shutting down in a loop.

## Explicit non-goals / rejected ideas

- **Real SoC or charge detection** — impossible with zero hardware; the spec
  keeps a clean upgrade path (swap `battery.cpp` internals for a MAX17048
  I2C driver later; UI and NVS contract unchanged).
- **Brownout-ISR flash writes** — rejected: flash writes during brownout risk
  NVS corruption, and the periodic save + low-battery shutdown already bound
  data loss to a few minutes of stats.
- **Charging-while-using compensation** — undetectable; documented behavior
  is "gauge keeps counting down while plugged in; tap Charged after a full
  charge."

## Testing

1. Build both envs; flash over OTA (`cyd-ota`).
2. Debug walkthrough: temporary `USABLE_MAH` of ~20 mAh compresses a full
   discharge into minutes — verify bucket colors, Stats line, low-battery
   shutdown, and that "Charged" resets to 100 %.
3. Persistence: reboot mid-discharge, confirm `batt_used` restores; power off
   (deep sleep) for a known interval, confirm the sleep-rate charge applies.
4. Restore real constants, reflash, sanity-check the ~7 h full-bright figure
   against a real afternoon of use.
