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

void Display::splash(const char* line) {
  tft_.fillScreen(TFT_BLACK);
  tft_.setTextColor(TFT_WHITE, TFT_BLACK);
  tft_.setTextDatum(MC_DATUM);
  tft_.drawString(line, tft_.width() / 2, tft_.height() / 2, 4);
}

} // namespace hal
