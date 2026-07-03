#include "trends.h"
#include "app/history.h"
#include "render/character.h"
#include "ui/text.h"
#include "ui/theme.h"

using namespace app;

namespace screens {

#define C_BAR_OLD 0x71C4 // dimmed coral: past days stay in the accent family

// Plot the 14-day bars into `c` with the plot's top-left at (ox, oy). Serves
// the live sprite refresh (local coords) and full-page draws (page coords).
static void plotBars(TFT_eSPI &c, int ox, int oy, const DayStat *win, int show,
                     long maxTok, int pitch, int barW, int bh, uint16_t colNew,
                     uint16_t colOld) {
  const int SLOTS = 14;
  for (int i = 0; i < show; i++) {
    // right-align the window so today always sits in the rightmost slot
    int slot = SLOTS - show + i;
    int x = ox + slot * pitch + (pitch - barW) / 2;
    int h = (int)((long long)win[i].tokens * (bh - 2) / maxTok);
    if (win[i].tokens > 0 && h < 2)
      h = 2; // a used day never disappears entirely
    if (h > 0)
      c.fillRect(x, oy + bh - h, barW, h, i == show - 1 ? colNew : colOld);
  }
}

void drawTrendsPage(TFT_eSPI &c, int yOrg, const ui::CardPal &p) {
  int W = ui::tft().width(), H = ui::tft().height();
  int cyA = render::REG_Y + render::REG_H + 4; // absolute card y on screen
  int cy = cyA - yOrg;
  int chh = H - cyA - 6;
  c.fillRoundRect(8, cy, W - 16, chh, 12, p.card);
  ui::gtextC(c, "Trends", W / 2, cy + 18, &FreeSansBold12pt7b, p.text, p.card,
             MC_DATUM);
  c.drawFastHLine(20, cy + 40, W - 40, p.divider);

  int n;
  const DayStat *days = historyDays(n);
  if (n == 0) {
    ui::gtextC(c, "No history yet", W / 2, cy + 80, &FreeSans9pt7b, p.muted,
               p.card, MC_DATUM);
    return;
  }
  const int SLOTS = 14;
  int show = n < SLOTS ? n : SLOTS;
  const DayStat *win = days + (n - show);
  long maxTok = 1;
  for (int i = 0; i < show; i++)
    if (win[i].tokens > maxTok)
      maxTok = win[i].tokens;
  int bw = 196, bh = 56, bx = (W - bw) / 2, by = cy + 48;
  int pitch = bw / SLOTS, barW = pitch - 5;
  plotBars(c, bx, by, win, show, maxTok, pitch, barW, bh, p.coral, p.barOld);
  c.drawFastHLine(bx, by + bh + 1, bw, p.divider); // baseline

  // summary: last-7-day total and daily average (of the days we have)
  int m = show < 7 ? show : 7;
  long long sum = 0;
  for (int i = show - m; i < show; i++)
    sum += win[i].tokens;
  char a[12], b[12], line[40];
  ui::fmtTok(sum, a, sizeof(a));
  ui::fmtTok(m > 0 ? sum / m : 0, b, sizeof(b));
  snprintf(line, sizeof(line), "%dd: %s   avg: %s", m, a, b);
  ui::gtextC(c, line, W / 2, cy + 118, &FreeSans9pt7b, p.muted, p.card,
             MC_DATUM);
}

void renderTrends(bool full) {
  TFT_eSPI &t = ui::tft();
  int W = t.width(), H = t.height();
  int cy = render::REG_Y + render::REG_H + 4;

  int n;
  const DayStat *days = historyDays(n);
  long today = n > 0 ? days[n - 1].tokens : 0;

  // live refresh: throttle + only when the data actually moved
  static long lastToday = -1;
  static int lastN = -1;
  static uint32_t lastDraw = 0;
  if (!full) {
    uint32_t nowMs = millis();
    if (nowMs - lastDraw < 1000)
      return;
    if (today == lastToday && n == lastN)
      return;
    lastDraw = nowMs;
  }
  lastToday = today;
  lastN = n;

  if (full) {
    drawTrendsPage(t, 0, ui::PAL_RGB);
    return;
  }
  if (n == 0) {
    // opaque bg -> repainting the same message in place never flickers
    ui::gtext("No history yet", W / 2, cy + 80, &FreeSans9pt7b, C_MUTED, C_CARD,
              MC_DATUM);
    return;
  }

  // ---- live bar refresh: up to the last 14 days, newest at the right in full
  // coral. Drawn into an off-screen sprite so a growing "today" bar swaps in
  // one pass (a direct redraw would flicker the whole plot).
  const int SLOTS = 14;
  int show = n < SLOTS ? n : SLOTS;
  const DayStat *win = days + (n - show);
  long maxTok = 1;
  for (int i = 0; i < show; i++)
    if (win[i].tokens > maxTok)
      maxTok = win[i].tokens;
  int bw = 196, bh = 56, bx = (W - bw) / 2, by = cy + 48;
  int pitch = bw / SLOTS, barW = pitch - 5;
  TFT_eSprite spr(&t);
  spr.setColorDepth(16);
  bool haveSpr = spr.createSprite(bw, bh);
  if (haveSpr) {
    spr.fillSprite(C_CARD);
    plotBars(spr, 0, 0, win, show, maxTok, pitch, barW, bh, C_CORAL, C_BAR_OLD);
    spr.pushSprite(bx, by);
    spr.deleteSprite();
  } else {
    // low heap -> direct draw (may flicker)
    t.fillRect(bx, by, bw, bh, C_CARD);
    plotBars(t, bx, by, win, show, maxTok, pitch, barW, bh, C_CORAL, C_BAR_OLD);
  }
  t.drawFastHLine(bx, by + bh + 1, bw, 0x2945); // baseline

  // ---- summary: last-7-day total and daily average (of the days we have)
  int m = show < 7 ? show : 7;
  long long sum = 0;
  for (int i = show - m; i < show; i++)
    sum += win[i].tokens;
  char a[12], b[12], line[40];
  ui::fmtTok(sum, a, sizeof(a));
  ui::fmtTok(m > 0 ? sum / m : 0, b, sizeof(b));
  snprintf(line, sizeof(line), "%dd: %s   avg: %s", m, a, b);
  ui::blitText(12, cy + 108, W - 24, 20, line, W / 2, cy + 118, &FreeSans9pt7b,
               C_MUTED, C_CARD, MC_DATUM, W - 32);
}

} // namespace screens
