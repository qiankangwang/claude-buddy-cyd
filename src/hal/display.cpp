#include "display.h"
#include <Arduino.h>

namespace hal {

// Backlight on a LEDC PWM channel so it can dim (pre-sleep fade + user
// brightness) instead of a hard on/off. TFT_BACKLIGHT_ON=1 -> active HIGH, so a
// higher duty is brighter.
#define BL_CH 7
#define BL_FREQ 5000
#define BL_RES 8 // 0..255 duty

void Display::begin() {
  tft_.init();
  tft_.setRotation(0); // portrait 240x320, USB at bottom
  // Attach the PWM channel AFTER init(): TFT_eSPI::init() can pinMode the
  // backlight pin itself, which would steal it back from LEDC and kill dimming.
  ledcSetup(BL_CH, BL_FREQ, BL_RES);
  ledcAttachPin(TFT_BL, BL_CH);
  backlight(true);
  tft_.fillScreen(TFT_BLACK);
}

void Display::backlightLevel(uint8_t pct) {
  if (pct > 100)
    pct = 100;
  ledcWrite(BL_CH, (uint32_t)pct * 255 / 100);
}

void Display::setBrightness(uint8_t pct) {
  if (pct < 10)
    pct = 10; // keep "on" visibly lit even at the lowest setting
  if (pct > 100)
    pct = 100;
  brightness_ = pct;
}

void Display::backlight(bool on) { backlightLevel(on ? brightness_ : 0); }

} // namespace hal
