# Battery gauge (software-estimated) 鈥?design spec

Status: DRAFT 鈥?awaiting user review. Date: 2026-07-03.

## Problem

The buddy now runs from a 2000 mAh Li-ion cell behind a generic charge+boost
module. The module has no data interface and the user wants **zero hardware
changes**, so the true state of charge is unreadable (the boost + LDO hold the
rails flat until the battery dies). The device should still give a useful
"time to recharge" signal.

## Approach: consumption-model fuel gauge

Integrate the device's own estimated draw over time and count down from a
known full charge. This is an *estimate*, not a measurement 鈥?the UI copy and
this spec both treat it as such.

- `used_mAh += I(state) * dt`, ticked every 10 s in the main loop.
- `percent = 100 * (1 - used / USABLE_MAH)`, clamped to 0..100.
- "Charged" is a manual event: the user taps a Settings row after charging;
  there is no way to detect the charger electrically.

### Current model (constants at top of `src/app/battery.cpp`, all tunable)

Battery-side mA at 3.7 V nominal, boost efficiency folded in
(I_batt 鈮?I_5V 脳 5.0 / (3.7 脳 0.85)):

| State                     | Est. draw |
|---------------------------|-----------|
| Screen on (backlight b %) | 143 + 95 脳 b/100 mA |
| Screen off, loop running  | 143 mA    |
| Deep sleep (powered off)  | 10 mA     |

- `CAPACITY_MAH 2000`, `USABLE_FRACTION 0.85` 鈫?1700 mAh usable.
  Full-brightness runtime 鈮?7 h, which matches the class of hardware.
- Backlight percent comes from the same `effectiveBright()` the display uses
  (auto-dim nights are cheaper and the gauge should know it).
- Constants are first-guess numbers; calibrate later with a USB power meter
  if the estimate drifts badly. Getting within 卤20 % is the bar.

### Deep-sleep accounting

`powerOff()` stamps the RTC-persistent clock (`gettimeofday`, which ESP-IDF
restores across deep sleep) into `RTC_DATA_ATTR` memory before
`esp_deep_sleep_start()`. Deep-sleep wake is a cold boot; at `begin()`, if the
stamp is valid, charge the elapsed time at the deep-sleep rate. RTC RC drift
(~5 %) is irrelevant at this accuracy.

### Persistence

- NVS key `batt_used` (mAh 脳 10, uint32). Saved by piggybacking the existing
  `saveStatsIfChanged` cadence (no new wear pattern), plus force-saves in
  `powerOff()` and at the low-battery shutdown.
- First boot / missing key = assume full (100 %).
- OTA reflash keeps NVS, so the gauge survives updates.

## UI

- **Status bar glyph**: small battery outline (~20脳10 px) right of the label
  band, left of the intensity pips; label band shrinks from `W-60` to `W-84`.
  Fill level in 5 % buckets; repaint only on bucket/color change (same
  no-flicker rule as the rest of the bar).
  Colors: `C_MUTED` 鈮?20 %, budget-bar amber < 20 %, `C_NO` red < 10 %.
- **Stats panel** (Settings 鈫?Stats): one line, e.g.
  `Battery ~63% 路 鈮?.2h left (est)`.
- **Settings row**: `Battery: Charged 鉁揱 鈥?tap = reset gauge to 100 %
  (persist immediately). This is the only way to tell the device it charged.
- **Low-battery shutdown**: at 鈮?5 % force-save stats/history and enter the
  existing `powerOff()` deep-sleep screen with copy "Battery low 鈥?charge me"
  (protects the stats and stops the boost from flat-draining the cell).
  **Boot grace period**: the shutdown check is armed only 3 min after boot,
  so a freshly-charged device that still *reads* 鈮?5 % can be woken and its
  gauge reset via Settings 鈫?Charged instead of shutting down in a loop.

## Explicit non-goals / rejected ideas

- **Real SoC or charge detection** 鈥?impossible with zero hardware; the spec
  keeps a clean upgrade path (swap `battery.cpp` internals for a MAX17048
  I2C driver later; UI and NVS contract unchanged).
- **Brownout-ISR flash writes** 鈥?rejected: flash writes during brownout risk
  NVS corruption, and the periodic save + low-battery shutdown already bound
  data loss to a few minutes of stats.
- **Charging-while-using compensation** 鈥?undetectable; documented behavior
  is "gauge keeps counting down while plugged in; tap Charged after a full
  charge."

## Testing

1. Build both envs; flash over OTA (`cyd-ota`).
2. Debug walkthrough: temporary `USABLE_MAH` of ~20 mAh compresses a full
   discharge into minutes 鈥?verify bucket colors, Stats line, low-battery
   shutdown, and that "Charged" resets to 100 %.
3. Persistence: reboot mid-discharge, confirm `batt_used` restores; power off
   (deep sleep) for a known interval, confirm the sleep-rate charge applies.
4. Restore real constants, reflash, sanity-check the ~7 h full-bright figure
   against a real afternoon of use.
