#include "power.h"
#include <esp_sleep.h>
#include "store.h"
#include "history.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace app {

void powerOff(hal::Display &display, hal::Touch &touch, hal::Led &led,
              hal::Storage &storage) {
  TFT_eSPI &t = display.tft();
  int W = t.width(), H = t.height();
  led.off();
  saveStatsIfChanged(storage, true); // persist before deep sleep (wakes as a cold boot)
  historySaveIfChanged(storage, true);
  t.fillScreen(TFT_BLACK);
  ui::gtext("Powering off", W / 2, H / 2 - 12, &FreeSansBold18pt7b, C_CORAL,
            TFT_BLACK, MC_DATUM);
  ui::gtext("tap screen or RST to wake", W / 2, H / 2 + 20, &FreeSans9pt7b,
            C_MUTED, TFT_BLACK, MC_DATUM);
  delay(1400); // let the message register before the screen cuts
  // wait for the selecting tap to lift, or the held touch would wake us at once
  for (uint32_t t0 = millis(); touch.rawPressed() && millis() - t0 < 15000;)
    delay(10);
  delay(200);
  display.backlight(false);
  led.off();
  esp_sleep_enable_ext0_wakeup((gpio_num_t)36, 0); // wake when PENIRQ goes low
  esp_deep_sleep_start();                           // does not return
}

} // namespace app
