#include "battery.h"
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
  g_used = storage.getInt("bused", 0) / 10.0f; // stored as mAh x 10
  g_savedUsed = g_used;
  if (g_sleepMagic == SLEEP_MAGIC && nowSec() >= g_sleepAtSec) {
    uint32_t slept = nowSec() - g_sleepAtSec;
    if (slept < 30UL * 24 * 3600) // sanity: clock restarted = no charge
      g_used += MA_SLEEP * slept / 3600.0f;
  }
  g_sleepMagic = 0;
  if (g_used > USABLE_MAH)
    g_used = USABLE_MAH;
  Serial.printf("[batt] restored used=%.1f mAh (%d%%)\n", g_used, percent());
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
  if (g_used > USABLE_MAH)
    g_used = USABLE_MAH;
  saveIfChanged(false);
}

int percent() {
  int p = (int)(100.0f * (1.0f - g_used / USABLE_MAH) + 0.5f);
  return p < 0 ? 0 : (p > 100 ? 100 : p);
}

float hoursLeft(bool screenOn, int brightPct) {
  return (USABLE_MAH - g_used) / drawMa(screenOn, brightPct);
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
