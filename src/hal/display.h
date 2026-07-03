#pragma once
#include <TFT_eSPI.h>

namespace hal {

// Thin wrapper around the CYD's ILI9341 panel (HSPI). Owns the TFT_eSPI
// instance so the rest of the firmware never talks to TFT_eSPI globals.
class Display {
public:
  void begin();
  TFT_eSPI& tft() { return tft_; }
  void backlight(bool on);          // full on (at the user brightness) / off; instant
  void backlightLevel(uint8_t pct); // 0..100% via PWM, instant (also stops a glide)
  void glideTo(uint8_t pct);        // ease the backlight toward pct (~250 ms)
  void tick(uint32_t now);          // steps an active glide; call every loop
  void setBrightness(uint8_t pct);  // the "full on" level (remembers it)

private:
  TFT_eSPI tft_;
  uint8_t brightness_ = 100; // the current "on" brightness (%)
  uint8_t cur_ = 0, tgt_ = 0; // backlight glide state (%)
  uint32_t lastStep_ = 0;
};

} // namespace hal
