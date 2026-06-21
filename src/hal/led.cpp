#include "led.h"

#define LED_R 4
#define LED_G 16
#define LED_B 17

namespace hal {

void Led::begin() {
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  off();
}

// Active-LOW: writing LOW turns the channel on.
void Led::set(bool r, bool g, bool b) {
  digitalWrite(LED_R, r ? LOW : HIGH);
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, b ? LOW : HIGH);
}

void Led::off() { set(false, false, false); }

} // namespace hal
