#pragma once
#include <TFT_eSPI.h>

namespace hal {

// Thin wrapper around the CYD's ILI9341 panel (HSPI). Owns the TFT_eSPI
// instance so the rest of the firmware never talks to TFT_eSPI globals.
class Display {
public:
  void begin();
  TFT_eSPI& tft() { return tft_; }
  void backlight(bool on);          // full on (at the user brightness) / off
  void backlightLevel(uint8_t pct); // 0..100% via PWM (dimming, pre-sleep fade)
  void setBrightness(uint8_t pct);  // user "full on" level (remembers it)

private:
  TFT_eSPI tft_;
  uint8_t brightness_ = 100; // user's chosen "on" brightness (%)
};

} // namespace hal
