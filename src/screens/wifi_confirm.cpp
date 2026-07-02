#include "wifi_confirm.h"
#include "layout.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace screens {

void renderWifiConfirm() {
  TFT_eSPI &t = ui::tft();
  int W = t.width();
  t.fillScreen(TFT_BLACK);
  ui::gtext("WiFi setup", W / 2, 40, &FreeSansBold18pt7b, C_CORAL, TFT_BLACK,
            TC_DATUM);
  ui::gtext("Opens a setup hotspot.", W / 2, 96, &FreeSans9pt7b, C_TEXT,
            TFT_BLACK, TC_DATUM);
  ui::gtext("Buddy is offline a few min.", W / 2, 122, &FreeSans9pt7b, C_MUTED,
            TFT_BLACK, TC_DATUM);
  ui::gtext("Old password is kept.", W / 2, 146, &FreeSans9pt7b, C_MUTED,
            TFT_BLACK, TC_DATUM);
  ui::drawButton(denyBtn, "Cancel", C_FACE);
  ui::drawButton(approveBtn, "Open", C_OK);
}

} // namespace screens
