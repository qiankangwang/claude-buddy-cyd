#pragma once
#include <Arduino.h>
#include "hal/storage.h"

namespace hal {

class Display; // fwd

// 3-point calibration result: maps raw XPT2046 readings to screen coords,
// robust to axis swap/flip (determined from the captured corner points).
struct TouchCal {
  uint32_t magic;
  uint8_t xUsesRawX; // screen-X is derived from raw-X (else raw-Y)
  uint8_t yUsesRawX; // screen-Y is derived from raw-X (else raw-Y)
  int16_t xLo, xHi;  // raw value at screen-X = MARGIN and W-MARGIN
  int16_t yLo, yHi;  // raw value at screen-Y = MARGIN and H-MARGIN
};

class Touch {
public:
  void begin(Display &disp, Storage &storage);
  bool read(int16_t &x, int16_t &y);  // mapped screen coords; true if pressed
  bool rawPressed();                   // is the panel being touched right now
  bool isCalibrated() const { return calibrated_; }
  void calibrate(Display &disp);       // interactive 3-point, persists to NVS

private:
  bool readRaw(int16_t &rx, int16_t &ry);
  Storage *store_ = nullptr;
  TouchCal cal_{};
  bool calibrated_ = false;
  int w_ = 240, h_ = 320;
};

} // namespace hal
