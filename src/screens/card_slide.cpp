#include "card_slide.h"
#include "home.h"
#include "trends.h"
#include "render/character.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace screens {

// Snapshot one page into a 4bpp palette sprite covering the whole band below
// the character region (240x140 at 4bpp = ~16.8 KB; 16bpp would be 67 KB and
// blow the largest free block). 4bpp draw colors are palette indices, which
// is why the page renderers take ui::PAL_IDX here and ui::PAL_RGB on screen.
static bool snapshotPage(TFT_eSprite &spr, int page, const char *st, int top) {
  spr.setColorDepth(4);
  if (!spr.createSprite(ui::tft().width(), ui::tft().height() - top))
    return false;
  spr.createPalette((uint16_t *)ui::CARD_PALETTE, 16);
  spr.fillSprite(0); // index 0 = the black band around the rounded card
  if (page == 0)
    drawStatsPage(spr, top, st, ui::PAL_IDX, false);
  else
    drawTrendsPage(spr, top, ui::PAL_IDX);
  return true;
}

// ease-out cubic mapped to 0..256 fixed-point
static int easeOut256(int i, int n) {
  float t = (float)i / n;
  float e = 1 - (1 - t) * (1 - t) * (1 - t);
  return (int)(e * 256.0f + 0.5f);
}

void slideCard(int target, const char *st) {
  int from = card();
  if (target == from)
    return;
  int top = render::REG_Y + render::REG_H;
  TFT_eSPI &t = ui::tft();
  int W = t.width();
  TFT_eSprite cur(&t), nxt(&t);
  bool ok =
      snapshotPage(cur, from, st, top) && snapshotPage(nxt, target, st, top);
  setCard(target);
  if (!ok) { // low heap: fall back to the old instant switch
    cur.deleteSprite();
    nxt.deleteSprite();
    if (target == 0)
      drawStatsPage(t, 0, st, ui::PAL_RGB, true);
    else
      drawTrendsPage(t, 0, ui::PAL_RGB);
    return;
  }
  int dir = target > from ? 1 : -1; // +1: the new page arrives from the right
  const int FRAMES = 10;            // ~25 ms per 2-sprite push -> ~250 ms
  for (int i = 1; i <= FRAMES; i++) {
    int off = W * easeOut256(i, FRAMES) / 256;
    cur.pushSprite(-dir * off, top);
    nxt.pushSprite(dir * (W - off), top);
  }
  cur.deleteSprite();
  nxt.deleteSprite();
}

void bounceCard(int dir, const char *st) {
  int top = render::REG_Y + render::REG_H;
  TFT_eSPI &t = ui::tft();
  int W = t.width(), H = t.height();
  TFT_eSprite cur(&t);
  if (!snapshotPage(cur, card(), st, top))
    return; // no heap -> skip the flourish; nothing breaks
  const int AMP = 24, FRAMES = 10; // out and back, ~120 ms each way
  for (int i = 1; i <= FRAMES; i++) {
    int ph = i <= FRAMES / 2 ? i : FRAMES - i; // 0..peak..0
    int off = AMP * easeOut256(ph, FRAMES / 2) / 256;
    cur.pushSprite(dir * off, top);
    if (dir > 0) // blank the sliver the nudge uncovers
      t.fillRect(0, top, off, H - top, TFT_BLACK);
    else
      t.fillRect(W - off, top, off, H - top, TFT_BLACK);
  }
  cur.pushSprite(0, top);
  cur.deleteSprite();
}

} // namespace screens
