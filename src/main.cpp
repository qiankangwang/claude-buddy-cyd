#include <Arduino.h>
#include "hal/display.h"
#include "hal/storage.h"
#include "hal/touch.h"

static hal::Display display;
static hal::Storage storage;
static hal::Touch touch;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[CYD Buddy] M2 touch + calibration");

  display.begin();
  storage.begin();
  touch.begin(display, storage);

  // Recalibrate when no stored cal, or when a finger is held on boot.
  if (!touch.isCalibrated() || touch.rawPressed()) {
    Serial.println("calibrating...");
    touch.calibrate(display);
    Serial.println("calibration saved");
  }

  TFT_eSPI &t = display.tft();
  t.fillScreen(TFT_BLACK);
  t.setTextDatum(TC_DATUM);
  t.setTextColor(TFT_WHITE, TFT_BLACK);
  t.drawString("Touch test: drag your finger", t.width() / 2, 6, 2);
  t.setTextColor(TFT_DARKGREY, TFT_BLACK);
  t.drawString("hold finger on boot = recalibrate", t.width() / 2, 26, 1);
}

void loop() {
  int16_t x, y;
  if (touch.read(x, y)) {
    display.tft().fillCircle(x, y, 3, TFT_GREEN);
    static uint32_t last = 0;
    if (millis() - last > 150) {
      last = millis();
      Serial.printf("touch %d,%d\n", x, y);
    }
  }
}
