#include "led.h"

#define LED_R 4
#define LED_G 16
#define LED_B 17
// LEDC channels (backlight uses 7; keep these clear of it).
#define CH_R 1
#define CH_G 2
#define CH_B 3
#define LED_FREQ 5000
#define LED_RES 8 // 0..255 duty

namespace hal {

void Led::begin() {
  ledcSetup(CH_R, LED_FREQ, LED_RES);
  ledcAttachPin(LED_R, CH_R);
  ledcSetup(CH_G, LED_FREQ, LED_RES);
  ledcAttachPin(LED_G, CH_G);
  ledcSetup(CH_B, LED_FREQ, LED_RES);
  ledcAttachPin(LED_B, CH_B);
  off();
}

// Active-LOW: the pin sits HIGH when off, so a *lit* channel's PWM spends only
// `bright_`% of each period LOW -> duty = 255 - bright_%. off() parks at 255.
void Led::set(bool r, bool g, bool b) {
  uint8_t on = 255 - (uint16_t)bright_ * 255 / 100;
  ledcWrite(CH_R, r ? on : 255);
  ledcWrite(CH_G, g ? on : 255);
  ledcWrite(CH_B, b ? on : 255);
}

void Led::off() {
  ledcWrite(CH_R, 255);
  ledcWrite(CH_G, 255);
  ledcWrite(CH_B, 255);
}

void Led::setBrightness(uint8_t pct) {
  if (pct > 100)
    pct = 100;
  bright_ = pct;
}

} // namespace hal
