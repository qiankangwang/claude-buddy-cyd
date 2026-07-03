#include "battery.h"
#include <esp_system.h>
#include <sys/time.h>

namespace app {
namespace battery {

// ---- draw model: battery-side mA at 3.7 V nominal, boost efficiency folded
// in (I_batt ~= I_5V x 5.0 / (3.7 x 0.85)). First-guess numbers -- tune with a
// USB power meter if the estimate drifts; within +-20% is the bar.
static const float CAPACITY_MAH = 2000.0f;
static const float USABLE_FRACTION = 0.85f; // don't count the cell's last gasp
static const float MA_BASE = 143.0f;        // board + ESP32 + WiFi, screen dark
static const float MA_BL_FULL = 95.0f;      // backlight's own draw at 100%
static const float MA_SLEEP = 10.0f;        // deep sleep incl. boost idle draw
static const float USABLE_MAH = CAPACITY_MAH * USABLE_FRACTION;

static hal::Storage *g_store = nullptr;
static float g_used = 0;      // mAh consumed since the last "charged" mark
static float g_savedUsed = 0; // last value mirrored to NVS
static float g_usable = USABLE_MAH; // learned real capacity (see begin())
static uint32_t g_lastTick = 0;

// Deep-sleep gap: gettimeofday() is restored from the RTC clock across deep
// sleep, and RTC_DATA_ATTR survives it too -- stamp the moment we go down and
// charge the gap at the sleep rate on the next boot. A real power loss clears
// RTC memory (magic mismatch) and resets the clock, so a stale stamp can only
// under-count, never explode.
#define SLEEP_MAGIC 0xB417C0DE
RTC_DATA_ATTR static uint32_t g_sleepMagic = 0;
RTC_DATA_ATTR static uint32_t g_sleepAtSec = 0;

static uint32_t nowSec() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint32_t)tv.tv_sec;
}

static float drawMa(bool screenOn, int brightPct) {
  return screenOn ? MA_BASE + MA_BL_FULL * brightPct / 100.0f : MA_BASE;
}

void begin(hal::Storage &storage) {
  g_store = &storage;
  int cap = storage.getInt("bcap", 0); // learned capacity, mAh x 10
  g_usable = cap > 0 ? cap / 10.0f : USABLE_MAH;
  g_used = storage.getInt("bused", 0) / 10.0f; // stored as mAh x 10
  g_savedUsed = g_used;
  if (g_sleepMagic == SLEEP_MAGIC && nowSec() >= g_sleepAtSec) {
    uint32_t slept = nowSec() - g_sleepAtSec;
    if (slept < 30UL * 24 * 3600) // sanity: clock restarted = no charge
      g_used += MA_SLEEP * slept / 3600.0f;
  }
  g_sleepMagic = 0;
  // Auto-calibration from a real death: when the cell actually runs dry the
  // boost output sags and the ESP32 dies by BROWNOUT -- at that instant the
  // true charge was 0, so the mAh the gauge had counted IS this cell's real
  // usable capacity at the modeled rates. Blend it in (50/50 EMA) so every
  // genuine flat-battery event tunes the model. Guard: only learn from a
  // substantially discharged gauge, so a wall-power glitch or a fresh-cell
  // hiccup can't shrink the capacity estimate.
  if (esp_reset_reason() == ESP_RST_BROWNOUT && g_used > 0.4f * g_usable) {
    g_usable = g_usable * 0.5f + g_used * 0.5f;
    if (g_usable < 200.0f)
      g_usable = 200.0f; // sanity floor
    storage.putInt("bcap", (int)(g_usable * 10));
    g_used = g_usable; // it died empty: read 0% until the next "Charged"
    saveIfChanged(true);
    Serial.printf("[batt] brownout death -> learned usable=%.0f mAh\n",
                  g_usable);
  }
  if (g_used > 2.0f * g_usable) // loose cap: overrun is calibration signal
    g_used = 2.0f * g_usable;
  Serial.printf("[batt] restored used=%.1f mAh of %.0f (%d%%)\n", g_used,
                g_usable, percent());
}

void tick(uint32_t now, bool screenOn, int brightPct) {
  if (g_lastTick == 0) {
    g_lastTick = now;
    return;
  }
  if (now - g_lastTick < 10000) // 10 s cadence is plenty for a gauge
    return;
  uint32_t dt = now - g_lastTick;
  g_lastTick = now;
  g_used += drawMa(screenOn, brightPct) * dt / 3600000.0f;
  // deliberately allowed to overrun g_usable (up to 2x): if the model was
  // pessimistic the overrun is exactly what brownout learning needs to see
  if (g_used > 2.0f * g_usable)
    g_used = 2.0f * g_usable;
  saveIfChanged(false);
}

int percent() {
  int p = (int)(100.0f * (1.0f - g_used / g_usable) + 0.5f);
  return p < 0 ? 0 : (p > 100 ? 100 : p);
}

float hoursLeft(bool screenOn, int brightPct) {
  float left = (g_usable - g_used) / drawMa(screenOn, brightPct);
  return left > 0 ? left : 0;
}

void resetFull() {
  g_used = 0;
  saveIfChanged(true);
}

void noteDeepSleep() {
  g_sleepAtSec = nowSec();
  g_sleepMagic = SLEEP_MAGIC;
  saveIfChanged(true);
}

void saveIfChanged(bool force) {
  if (!g_store)
    return;
  int cur = (int)(g_used * 10);
  if (cur == (int)(g_savedUsed * 10))
    return; // no-op write: spare the flash even when forced
  float delta = g_used - g_savedUsed;
  if (delta < 0)
    delta = -delta;
  if (!force && delta < 5.0f)
    return; // ~2-3 min of runtime per write -> NVS-friendly
  g_store->putInt("bused", cur);
  g_savedUsed = g_used;
}

} // namespace battery
} // namespace app
