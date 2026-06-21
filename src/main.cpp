#include <Arduino.h>
#include "hal/display.h"

static hal::Display display;

// Color bars + labels to validate driver / color order / inversion at a glance.
static void drawTestPattern(TFT_eSPI &tft) {
  const int w = tft.width();
  const int h = tft.height();

  struct Band { uint16_t color; const char *name; uint16_t label; };
  const Band bands[] = {
      {TFT_RED, "RED", TFT_WHITE},     {TFT_GREEN, "GREEN", TFT_BLACK},
      {TFT_BLUE, "BLUE", TFT_WHITE},   {TFT_WHITE, "WHITE", TFT_BLACK},
      {TFT_YELLOW, "YELLOW", TFT_BLACK}, {TFT_CYAN, "CYAN", TFT_BLACK},
      {TFT_MAGENTA, "MAGENTA", TFT_WHITE}, {TFT_BLACK, "BLACK", TFT_WHITE},
  };
  const int n = sizeof(bands) / sizeof(bands[0]);
  const int bandH = h / n;

  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < n; i++) {
    tft.fillRect(0, i * bandH, w, bandH, bands[i].color);
    tft.setTextColor(bands[i].label, bands[i].color);
    tft.drawString(bands[i].name, 6, i * bandH + 4, 2);
  }

  // Center banner (opaque) to confirm fonts + orientation.
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.drawString("CYD Buddy", w / 2, h / 2 - 14, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("bring-up M1", w / 2, h / 2 + 16, 2);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[CYD Buddy] M1 screen bring-up");
  display.begin();
  drawTestPattern(display.tft());
  Serial.printf("TFT %dx%d initialized\n", display.tft().width(),
                display.tft().height());
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 2000) {
    last = millis();
    Serial.printf("alive, heap=%u\n", ESP.getFreeHeap());
  }
}
