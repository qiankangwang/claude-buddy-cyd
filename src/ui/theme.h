#pragma once
#include <TFT_eSPI.h>
#include <stdint.h>

// Palette (RGB565). Coral = Anthropic accent (#D97757). The character art is
// warmed on-screen by render::tintColor (R x1.06, B x0.85); the raw #D97757
// reads pinker than that, so the UI accent uses the SAME-tinted coral (~#E6774A)
// so headline/number text matches the Clawd you actually see. (raw was 0xDBAA)
#define C_CORAL 0xE3A8
#define C_TEXT TFT_WHITE
#define C_MUTED 0x8C71
#define C_OK 0x256C   // refined green
#define C_NO 0xC1C5   // muted red
#define C_FACE 0x2966 // neutral button face (dark slate)
#define C_CARD 0x10A3 // info-card fill (very dark slate)

namespace ui {
// One indirection over the card colors so the same page renderer can draw to
// the screen (real RGB565) or into a 4bpp snapshot sprite, where TFT_eSPI
// treats draw colors as palette indices 0..15 (see screens/card_slide.cpp).
struct CardPal {
  uint16_t black, card, text, coral, muted, no, divider, amber, barOld;
};
static const CardPal PAL_RGB = {0x0000, C_CARD, C_TEXT, C_CORAL, C_MUTED,
                                C_NO,   0x2945, 0xFD20, 0x71C4};
static const CardPal PAL_IDX = {0, 1, 2, 3, 4, 5, 6, 7, 8};
// index -> RGB565 for TFT_eSprite::createPalette; order matches PAL_IDX
static const uint16_t CARD_PALETTE[16] = {
    0x0000, C_CARD, C_TEXT, C_CORAL, C_MUTED, C_NO, 0x2945, 0xFD20, 0x71C4,
    0,      0,      0,      0,       0,       0,    0};
} // namespace ui
