#include "settings.h"
#include "layout.h"
#include "app/ctx.h"
#include "app/battery.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace screens {

void renderSettings() {
  TFT_eSPI &t = ui::tft();
  int W = t.width();
  t.fillScreen(TFT_BLACK);
  ui::gtext("Settings", W / 2, 16, &FreeSansBold18pt7b, C_CORAL, TFT_BLACK,
            TC_DATUM);
  char quiet[20], bri[20], batt[24];
  snprintf(quiet, sizeof(quiet), "Quiet: %s", app::ctx.dnd ? "on" : "off");
  if (app::ctx.autoDim)
    snprintf(bri, sizeof(bri), "Brightness: auto");
  else
    snprintf(bri, sizeof(bri), "Brightness: %d%%", app::ctx.brightPct);
  // estimate readout doubles as the reset action: tapping it marks "charged"
  snprintf(batt, sizeof(batt), "Battery: ~%d%%", app::battery::percent());
  const char *labels[8] = {"Stats",       quiet,        bri,         batt,
                           "Recalibrate", "WiFi setup", "Power off", "Close"};
  for (int i = 0; i < 8; i++)
    ui::drawButton(setBtns[i], labels[i],
                   (i == 1 && app::ctx.dnd) ? 0x7B40 : C_FACE);
}

} // namespace screens
