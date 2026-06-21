#pragma once
#include <Arduino.h>
#include "hal/display.h"

namespace render {

// Character draw region (shared with the renderer; main.cpp lays out the
// top/bottom UI around it). Single source of truth so they can't drift.
inline constexpr int REG_X = 0, REG_Y = 34, REG_W = 240, REG_H = 176;

// Official-look character: plays a GIF pack (bufo) from LittleFS via
// AnimatedGIF. State names match upstream manifest.json:
//   sleep / idle (array, rotates) / busy / attention / celebrate / dizzy / heart
class Character {
public:
  bool begin(hal::Display &disp, const char *packDir);
  bool loaded() const { return loaded_; }
  void setState(const char *state); // switch animation (no-op if unchanged)
  void update();                    // advance + draw frames at the GIF's own pace
  void clearArea();                 // black out the character region

private:
  bool openCurrent();
  bool loaded_ = false;
};

extern Character character;

} // namespace render
