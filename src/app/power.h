#pragma once
#include "hal/display.h"
#include "hal/touch.h"
#include "hal/led.h"
#include "hal/storage.h"

namespace app {

// Deep-sleep "power off": go dark and draw ~no power. Wakes on a screen touch
// (XPT2046 PENIRQ on GPIO36 pulls low when touched) or the board's RST button;
// either way the device cold-boots straight back into the dashboard.
// `title` lets callers say why ("Battery low - charge me"). Does not return.
void powerOff(hal::Display &display, hal::Touch &touch, hal::Led &led,
              hal::Storage &storage, const char *title = "Powering off");

} // namespace app
