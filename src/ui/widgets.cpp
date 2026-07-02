#include "widgets.h"
#include "text.h"
#include "theme.h"

namespace ui {

bool inRect(const Rect &r, int x, int y) {
  return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

void drawButton(const Rect &r, const char *label, uint16_t col) {
  TFT_eSPI &t = tft();
  t.fillRoundRect(r.x, r.y, r.w, r.h, 12, col);
  t.drawRoundRect(r.x, r.y, r.w, r.h, 12, 0x4A69); // subtle light border
  gtext(label, r.x + r.w / 2, r.y + r.h / 2, &FreeSansBold9pt7b, C_TEXT, col,
        MC_DATUM);
}

} // namespace ui
