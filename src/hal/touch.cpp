#include "touch.h"
#include "display.h"
#include <SPI.h>

// CYD resistive touch (XPT2046) lives on its own (V)SPI bus, separate from the
// display's HSPI. Pins are fixed by the board.
//
// The chip is driven directly here (the XPT2046_Touchscreen library was
// dropped): the library gates every read on a PENIRQ interrupt flag that a
// mid-press pressure dip clears while the finger still holds PENIRQ low -- no
// new falling edge ever arrives, so the rest of that contact is invisible and
// taps randomly "miss" until the finger lifts and presses again. It also
// hardcodes a firm-press pressure floor (220 after a build-time patch; 300
// upstream) deep in its .cpp. Polling the ADC directly removes the stuck gate
// and makes the pressure thresholds ours to tune.
#define T_CLK 25
#define T_CS 33
#define T_MOSI 32
#define T_MISO 39
#define T_IRQ 36 // PENIRQ: used only as the deep-sleep wake pin (app/power)

#define CAL_MAGIC 0x43594431UL // "CYD1"
#define CAL_MARGIN 24          // screen inset of calibration targets

// Pressure metric: z = z1 + 4095 - z2 (bigger = firmer); untouched reads ~0.
// Z_PRESS opens a new contact; the lower Z_HOLD keeps an established one alive
// through the brief mid-press dips resistive panels produce (hysteresis), so
// holds and swipes don't shatter into phantom taps. Light fingertip taps sit
// in the 100-300 range that the old library floor rejected outright.
#define Z_PRESS 120
#define Z_HOLD 70
// A contact whose 5-sample median window still spreads more than this many raw
// units (~13 px) is a grazing touch with unsettled coordinates -- don't aim a
// tap with it (an established contact just keeps its last good position).
#define RAW_JITTER_MAX 120

namespace hal {

static SPIClass touchSPI(VSPI);
static const SPISettings XPT_SPI(2000000, MSBFIRST, SPI_MODE0);

// XPT2046 command bytes, 12-bit differential reads. 0x91 / 0xD1 sample the two
// position channels powered (PD=01, PENIRQ disabled while converting); 0xD0 is
// the 0xD1 read with PD=00, which powers the chip down and re-arms PENIRQ --
// every transaction must end with it or the deep-sleep touch wake goes dead.
// Each transfer16 clocks out the RESULT of the previous command while sending
// the next one (results are 12 bits left-aligned, hence the >> 3).

static void sort5(int16_t v[5]) {
  for (int i = 1; i < 5; i++) {
    int16_t k = v[i];
    int j = i - 1;
    for (; j >= 0 && v[j] > k; j--)
      v[j + 1] = v[j];
    v[j + 1] = k;
  }
}

// Cheap pressure-only probe (4 transfers, ~30 us): enough for "is a finger on
// the panel" polls (sleep wake, waiting for a release).
static int zQuick() {
  touchSPI.beginTransaction(XPT_SPI);
  digitalWrite(T_CS, LOW);
  touchSPI.transfer(0xB1);                    // start Z1
  int z1 = touchSPI.transfer16(0xC1) >> 3;    // Z1; start Z2
  int z2 = touchSPI.transfer16(0xD0) >> 3;    // Z2; start power-down read
  touchSPI.transfer16(0);                     // flush (PENIRQ re-armed)
  digitalWrite(T_CS, HIGH);
  touchSPI.endTransaction();
  int z = z1 + 4095 - z2;
  return z < 0 ? 0 : z;
}

// Full sample burst: pressure + 5 interleaved reads per axis in one ~150 us
// transaction. Returns median coordinates in the SAME raw convention the old
// library produced at rotation 0 (rx = 4095 - median(0xD1), ry = median(0x91)),
// so factory constants and any calibration saved in NVS stay valid. Returns
// false when the medians haven't settled (grazing contact); z is always out.
static bool rawSample(int16_t &rx, int16_t &ry, int &z) {
  int16_t a[5], b[5];
  touchSPI.beginTransaction(XPT_SPI);
  digitalWrite(T_CS, LOW);
  touchSPI.transfer(0xB1);                    // start Z1
  int z1 = touchSPI.transfer16(0xC1) >> 3;    // Z1; start Z2
  int z2 = touchSPI.transfer16(0x91) >> 3;    // Z2; start ch-A
  touchSPI.transfer16(0x91);                  // drop 1st ch-A read (always noisy)
  for (int i = 0; i < 5; i++) {
    a[i] = touchSPI.transfer16(0xD1) >> 3;    // ch-A result; start ch-B
    b[i] = touchSPI.transfer16(0x91) >> 3;    // ch-B result; start ch-A
  }
  touchSPI.transfer16(0xD0);                  // drop pending ch-A; start power-down read
  touchSPI.transfer16(0);                     // flush (PENIRQ re-armed)
  digitalWrite(T_CS, HIGH);
  touchSPI.endTransaction();
  z = z1 + 4095 - z2;
  if (z < 0)
    z = 0;
  sort5(a);
  sort5(b);
  if (a[3] - a[1] > RAW_JITTER_MAX || b[3] - b[1] > RAW_JITTER_MAX)
    return false;
  rx = 4095 - b[2];
  ry = a[2];
  return true;
}

void Touch::begin(Display &disp, Storage &storage) {
  store_ = &storage;
  w_ = disp.tft().width();
  h_ = disp.tft().height();
  pinMode(T_CS, OUTPUT);
  digitalWrite(T_CS, HIGH);
  pinMode(T_IRQ, INPUT); // input-only pin; the panel provides the pull-up
  touchSPI.begin(T_CLK, T_MISO, T_MOSI, T_CS);
  zQuick(); // one throwaway transaction leaves the chip powered down, PENIRQ armed
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

bool Touch::rawPressed() { return zQuick() >= (down_ ? Z_HOLD : Z_PRESS); }

bool Touch::readRaw(int16_t &rx, int16_t &ry) {
  int z;
  int16_t sx, sy;
  bool stable = rawSample(sx, sy, z);
  if (z < (down_ ? Z_HOLD : Z_PRESS)) {
    down_ = false;
    return false;
  }
  if (stable) {
    lastRX_ = sx;
    lastRY_ = sy;
  } else if (!down_) {
    return false; // don't open a contact on unsettled coordinates
  }
  down_ = true;
  rx = lastRX_;
  ry = lastRY_;
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
  while (zQuick() < Z_PRESS) {
    if (millis() - t0 > WAIT_MS)
      return false; // no tap arrived in time -> abort calibration
    delay(10);
  }
  long sx = 0, sy = 0;
  int n = 0;
  uint32_t tc = millis();
  while (millis() - tc < 350) {
    int z;
    int16_t rx, ry;
    if (rawSample(rx, ry, z) && z >= Z_HOLD) {
      sx += rx;
      sy += ry;
      n++;
    }
    delay(10);
  }
  ox = n ? (int16_t)(sx / n) : 0;
  oy = n ? (int16_t)(sy / n) : 0;
  uint32_t tr = millis();
  while (zQuick() >= Z_HOLD) {
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
    while (zQuick() >= Z_HOLD) {
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
