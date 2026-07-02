#pragma once
#include <Arduino.h>
#include "hal/led.h"

namespace app {

// Ambient LED "language": a colour + blink rhythm per state (binary RGB, so a
// breath is a slow blink). blue = working (cooler/quicker as the session heats
// up), amber = your turn (escalates the longer you ignore it), red = error,
// green = done, magenta = play, dark = idle. `silenced` (Quiet) turns it off.
void driveLed(hal::Led &led, const char *st, uint32_t now, int intensity,
              uint32_t waitStart, bool silenced);

} // namespace app
