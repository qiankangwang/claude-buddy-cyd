#pragma once
#include <TFT_eSPI.h>

namespace hal {

// Thin wrapper around the CYD's ST7789 panel (HSPI). Owns the TFT_eSPI
// instance so the rest of the firmware never talks to TFT_eSPI globals.
class Display {
public:
  void begin();
  TFT_eSPI& tft() { return tft_; }
  void backlight(bool on);

private:
  TFT_eSPI tft_;
};

} // namespace hal
