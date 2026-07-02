#include "layout.h"
#include "render/character.h"

namespace screens {

ui::Rect denyBtn, approveBtn;
ui::Rect ackBtn;
ui::Rect setBtns[7];

void computeButtons(TFT_eSPI &t) {
  int W = t.width(), H = t.height();
  int bh = 56, m = 8;
  denyBtn = {m, H - bh - 6, (W - 3 * m) / 2, bh};
  approveBtn = {denyBtn.x + denyBtn.w + m, H - bh - 6, (W - 3 * m) / 2, bh};
  int sy = 46, sbh = 32, gap = 6; // 7 rows fit 240x320 (46 + 7*38 = 312)
  for (int i = 0; i < 7; i++)
    setBtns[i] = {20, sy + i * (sbh + gap), W - 40, sbh};
  // "Got it" button: the cardless needs-you screen frees the whole lower third,
  // so centre a roomy pill there as the dismiss call-to-action.
  int aw = 150, ah = 46, cardTop = render::REG_Y + render::REG_H;
  ackBtn = {(W - aw) / 2, cardTop + (H - cardTop - ah) / 2, aw, ah};
}

} // namespace screens
