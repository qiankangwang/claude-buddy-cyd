#include "display.h"
#include <Arduino.h>

namespace hal {

void Display::begin() {
  pinMode(TFT_BL, OUTPUT);
  tft_.init();
  tft_.setRotation(0); // portrait 240x320, USB at bottom
  backlight(true);
  tft_.fillScreen(TFT_BLACK);
}

void Display::backlight(bool on) {
  digitalWrite(TFT_BL, on ? HIGH : LOW);
}

} // namespace hal
