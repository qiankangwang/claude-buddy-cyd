#pragma once
#include <Arduino.h>

namespace hal {

// CYD onboard RGB LED (R=4, G=16, B=17), common-anode / active-LOW.
class Led {
public:
  void begin();
  void set(bool r, bool g, bool b);
  void off();
};

} // namespace hal
