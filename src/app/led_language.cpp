#include "led_language.h"
#include "activity.h"

namespace app {

void driveLed(hal::Led &led, const char *st, uint32_t now, int intensity,
              uint32_t waitStart, bool silenced) {
  if (silenced) {
    led.off();
    return;
  }
  bool r = false, g = false, b = false;
  uint32_t period = 0, onMs = 0; // period 0 -> solid

  if (!strcmp(st, "attention") || !strcmp(st, "notification")) {
    r = g = true; // amber alert
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
      period = 1500;
      onMs = 850;
    } // gentle breath
  } else if (!strcmp(st, "error")) {
    r = true;
    period = 360;
    onMs = 180; // red blink
  } else if (!strcmp(st, "dizzy") || !strcmp(st, "heart")) {
    r = b = true; // magenta
  } else if (!strcmp(st, "celebrate")) {
    g = true; // green
  } else if (isWork(st)) {
    b = true; // blue focus
    if (intensity >= 2) {
      g = true;
      b = true;
      r = false; // cyan, quick
      period = 520;
      onMs = 300;
    } else if (intensity == 1) {
      period = 1100;
      onMs = 650;
    } else {
      period = 1800;
      onMs = 1050; // calm breath
    }
  } else {
    led.off();
    return; // idle / sleep: dark
  }

  bool on = (period == 0) ? true : ((now % period) < onMs);
  if (on)
    led.set(r, g, b);
  else
    led.off();
}

} // namespace app
