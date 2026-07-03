#include "led_language.h"
#include "activity.h"

namespace app {

// Smooth triangle envelope, lo..hi over `period` -- the "breathing" waveform
// (the gamma correction that makes it look sinusoidal lives in Led::setLevel).
static uint8_t breathe(uint32_t now, uint32_t period, uint8_t lo, uint8_t hi) {
  uint32_t ph = now % period;
  uint32_t half = period / 2;
  uint32_t tri = ph < half ? ph : period - ph; // 0..half..0
  return lo + (uint8_t)((uint32_t)(hi - lo) * tri / half);
}

void driveLed(hal::Led &led, const char *st, uint32_t now, int intensity,
              uint32_t waitStart, bool silenced) {
  if (silenced) {
    led.off();
    return;
  }
  bool r = false, g = false, b = false;
  uint32_t period = 0, onMs = 0; // square blink (period 0 -> solid)
  int level = -1;                // >=0 -> smooth PWM envelope instead

  if (!strcmp(st, "attention") || !strcmp(st, "notification")) {
    r = g = true; // amber alert; escalation keeps hard blinks (urgency reads
                  // as a blink, calm reads as a breath)
    uint32_t waited = waitStart ? now - waitStart : 0;
    if (waited > 45000) {
      period = 240;
      onMs = 120;
    } // urgent
    else if (waited > 15000) {
      period = 600;
      onMs = 300;
    } // insistent
    else {
      level = breathe(now, 1500, 8, 100); // gentle true breath
    }
  } else if (!strcmp(st, "error")) {
    r = true;
    period = 360;
    onMs = 180; // red blink
  } else if (!strcmp(st, "heart")) {
    r = b = true; // magenta heartbeat: thump-thump ... rest
    uint32_t ph = now % 900;
    level = ph < 110 ? 100 : ph < 220 ? 20 : ph < 330 ? 70 : 10;
  } else if (!strcmp(st, "dizzy")) {
    r = b = true; // solid magenta
  } else if (!strcmp(st, "celebrate")) {
    g = true;
    level = breathe(now, 500, 40, 100); // festive shimmer
  } else if (isWork(st)) {
    b = true; // blue focus
    if (intensity >= 2) {
      g = true; // cyan, quick -- high gear stays a crisp pulse
      period = 520;
      onMs = 300;
    } else if (intensity == 1) {
      level = breathe(now, 1100, 10, 100);
    } else {
      level = breathe(now, 1800, 6, 90); // calm breath
    }
  } else {
    led.off();
    return; // idle / sleep: dark
  }

  if (level >= 0)
    led.setLevel(r, g, b, (uint8_t)level);
  else if (period == 0 || (now % period) < onMs)
    led.set(r, g, b);
  else
    led.off();
}

} // namespace app
