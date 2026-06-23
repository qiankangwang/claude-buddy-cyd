#include "touch.h"
#include "display.h"
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

// CYD resistive touch (XPT2046) lives on its own (V)SPI bus, separate from the
// display's HSPI. Pins are fixed by the board.
#define T_CLK 25
#define T_CS 33
#define T_MOSI 32
#define T_MISO 39
#define T_IRQ 36

#define CAL_MAGIC 0x43594431UL // "CYD1"
#define CAL_MARGIN 24          // screen inset of calibration targets
#define Z_MIN 200              // pressure threshold

namespace hal {

static SPIClass touchSPI(VSPI);
static XPT2046_Touchscreen ts(T_CS, T_IRQ);

void Touch::begin(Display &disp, Storage &storage) {
  store_ = &storage;
  w_ = disp.tft().width();
  h_ = disp.tft().height();
  touchSPI.begin(T_CLK, T_MISO, T_MOSI, T_CS);
  ts.begin(touchSPI);
  ts.setRotation(0);
  // Fixed factory calibration for this CYD unit (auto; no manual step / button).
  // Measured once via 3-point calibration; resistive panels are uniform enough
  // that these constants work out of the box.
  cal_.xUsesRawX = 1;
  cal_.yUsesRawX = 0;
  cal_.xLo = 1837;
  cal_.xHi = 3559;
  cal_.yLo = 1590;
  cal_.yHi = 3477;
  cal_.magic = CAL_MAGIC;
  calibrated_ = true;
  // A user recalibration (Settings -> Recalibrate, with visible targets) is
  // saved to NVS and overrides these defaults.
  TouchCal c{};
  if (store_->getBytes("touchcal", &c, sizeof(c)) && c.magic == CAL_MAGIC)
    cal_ = c;
}

bool Touch::rawPressed() { return ts.touched(); }

bool Touch::readRaw(int16_t &rx, int16_t &ry) {
  if (!ts.touched())
    return false;
  TS_Point p = ts.getPoint();
  if (p.z < Z_MIN)
    return false;
  rx = p.x;
  ry = p.y;
  return true;
}

bool Touch::read(int16_t &x, int16_t &y) {
  int16_t rx, ry;
  if (!readRaw(rx, ry))
    return false;
  if (!calibrated_) {
    x = rx;
    y = ry;
    return true;
  }
  if (cal_.xHi == cal_.xLo || cal_.yHi == cal_.yLo) {
    x = rx; // degenerate calibration -> avoid divide-by-zero
    y = ry;
    return true;
  }
  int rawX = cal_.xUsesRawX ? rx : ry;
  int rawY = cal_.yUsesRawX ? rx : ry;
  long span = (long)(w_ - 1 - 2 * CAL_MARGIN);
  long sx = CAL_MARGIN + (long)(rawX - cal_.xLo) * span / (cal_.xHi - cal_.xLo);
  span = (long)(h_ - 1 - 2 * CAL_MARGIN);
  long sy = CAL_MARGIN + (long)(rawY - cal_.yLo) * span / (cal_.yHi - cal_.yLo);
  x = constrain((int)sx, 0, w_ - 1);
  y = constrain((int)sy, 0, h_ - 1);
  return true;
}

// Wait for a press, average samples while held, then wait for release.
// Returns false on timeout so calibrate() can abort instead of hanging loop()
// forever (the web server / hook endpoint / animation all stall while this
// runs). delay() keeps feeding the idle task so no watchdog reset masks it.
static bool captureStableTouch(int16_t &ox, int16_t &oy) {
  const uint32_t WAIT_MS = 15000;
  uint32_t t0 = millis();
  while (!ts.touched()) {
    if (millis() - t0 > WAIT_MS)
      return false; // no tap arrived in time -> abort calibration
    delay(10);
  }
  long sx = 0, sy = 0;
  int n = 0;
  uint32_t tc = millis();
  while (millis() - tc < 350) {
    if (ts.touched()) {
      TS_Point p = ts.getPoint();
      if (p.z >= 150) {
        sx += p.x;
        sy += p.y;
        n++;
      }
    }
    delay(10);
  }
  ox = n ? (int16_t)(sx / n) : 0;
  oy = n ? (int16_t)(sy / n) : 0;
  uint32_t tr = millis();
  while (ts.touched()) {
    if (millis() - tr > WAIT_MS)
      break; // stuck/noisy contact -> stop waiting for release
    delay(10);
  }
  delay(150);
  return n > 0; // false if we never captured a solid sample
}

void Touch::calibrate(Display &disp) {
  TFT_eSPI &t = disp.tft();
  const int M = CAL_MARGIN;
  // The tap that selected "Recalibrate" is likely still held; wait for it to
  // release first (timeout-guarded) so it isn't captured as the first target.
  {
    uint32_t tr = millis();
    while (ts.touched()) {
      if (millis() - tr > 15000)
        break;
      delay(10);
    }
    delay(120);
  }
  struct P {
    int sx, sy;
  } targets[3] = {{M, M}, {w_ - M, M}, {M, h_ - M}};
  int16_t raw[3][2];

  for (int i = 0; i < 3; i++) {
    t.fillScreen(TFT_BLACK);
    t.setTextColor(TFT_WHITE, TFT_BLACK);
    t.setTextDatum(MC_DATUM);
    t.drawString("Calibrate touch", w_ / 2, h_ / 2 - 10, 4);
    t.drawString("tap the yellow dot", w_ / 2, h_ / 2 + 16, 2);
    int cx = targets[i].sx, cy = targets[i].sy;
    t.drawLine(cx - 12, cy, cx + 12, cy, TFT_RED);
    t.drawLine(cx, cy - 12, cx, cy + 12, TFT_RED);
    t.drawCircle(cx, cy, 9, TFT_YELLOW);
    if (!captureStableTouch(raw[i][0], raw[i][1]))
      return; // timed out -> abort, keep the existing working calibration
    t.fillCircle(cx, cy, 7, TFT_GREEN);
    delay(350);
  }

  int dxRawX = raw[1][0] - raw[0][0]; // TR-TL along raw-X
  int dxRawY = raw[1][1] - raw[0][1]; // TR-TL along raw-Y
  cal_.xUsesRawX = abs(dxRawX) >= abs(dxRawY) ? 1 : 0;
  int dyRawX = raw[2][0] - raw[0][0]; // BL-TL along raw-X
  int dyRawY = raw[2][1] - raw[0][1]; // BL-TL along raw-Y
  cal_.yUsesRawX = abs(dyRawX) >= abs(dyRawY) ? 1 : 0;
  cal_.xLo = cal_.xUsesRawX ? raw[0][0] : raw[0][1];
  cal_.xHi = cal_.xUsesRawX ? raw[1][0] : raw[1][1];
  cal_.yLo = cal_.yUsesRawX ? raw[0][0] : raw[0][1];
  cal_.yHi = cal_.yUsesRawX ? raw[2][0] : raw[2][1];
  // Reject a degenerate calibration (e.g. all three targets tapped at the same
  // spot) so we don't persist a divide-by-zero map; just recalibrate next boot.
  if (abs(cal_.xHi - cal_.xLo) < 200 || abs(cal_.yHi - cal_.yLo) < 200) {
    calibrated_ = false;
    return;
  }
  cal_.magic = CAL_MAGIC;
  store_->putBytes("touchcal", &cal_, sizeof(cal_));
  calibrated_ = true;
}

} // namespace hal
