#pragma once
#include <Arduino.h>

namespace hal {

// CYD onboard RGB LED (R=4, G=16, B=17), common-anode / active-LOW. Driven via
// LEDC PWM so its (otherwise glaring) brightness can be dialled down.
class Led {
public:
  void begin();
  void set(bool r, bool g, bool b);
  // Lit channels at pct (0..100) of the configured brightness, gamma-corrected
  // -- the envelope for smooth breathing/heartbeat patterns.
  void setLevel(bool r, bool g, bool b, uint8_t pct);
  void off();
  void setBrightness(uint8_t pct); // 0..100% of full-on for lit channels

private:
  uint8_t bright_ = 10; // the CYD LED is very bright; default to a faint glow
};

} // namespace hal
