#include "settings.h"
#include "layout.h"
#include "app/ctx.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace screens {

void renderSettings() {
  TFT_eSPI &t = ui::tft();
  int W = t.width();
  t.fillScreen(TFT_BLACK);
  ui::gtext("Settings", W / 2, 16, &FreeSansBold18pt7b, C_CORAL, TFT_BLACK,
            TC_DATUM);
  char quiet[20], bri[20];
  snprintf(quiet, sizeof(quiet), "Quiet: %s", app::ctx.dnd ? "on" : "off");
  if (app::ctx.autoDim)
    snprintf(bri, sizeof(bri), "Brightness: auto");
  else
    snprintf(bri, sizeof(bri), "Brightness: %d%%", app::ctx.brightPct);
  // NOTE: no Battery row on purpose -- the gauge is fully automatic (top-bar
  // glyph + Stats panel show it) and a tappable row was too easy to fat-finger.
  const char *labels[7] = {"Stats", quiet, bri, "Recalibrate",
                           "WiFi setup", "Power off", "Close"};
  for (int i = 0; i < 7; i++)
    ui::drawButton(setBtns[i], labels[i],
                   (i == 1 && app::ctx.dnd) ? 0x7B40 : C_FACE);
}

} // namespace screens
