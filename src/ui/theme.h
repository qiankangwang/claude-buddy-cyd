#pragma once
#include <TFT_eSPI.h>

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
