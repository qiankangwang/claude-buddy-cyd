#pragma once
#include <Arduino.h>
#include "hal/storage.h"

namespace app {
namespace battery {

// Software fuel gauge (docs/battery-gauge-spec.md): integrates a state-based
// draw model against a manually reset "full" mark. An estimate, not a reading
// -- the cell sits behind a dumb boost module with no data path.

void begin(hal::Storage &storage); // restore used-mAh; charge the sleep gap
void tick(uint32_t now, bool screenOn, int brightPct); // integrate, ~10 s
int percent();                                         // 0..100
float hoursLeft(bool screenOn, int brightPct);         // at the present draw
void resetFull();      // Settings "Battery" row: user says it's charged
void noteDeepSleep();  // stamp the RTC clock; call just before deep sleep
void saveIfChanged(bool force); // NVS write, throttled by a 5 mAh delta

} // namespace battery
} // namespace app
