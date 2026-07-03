#pragma once
#include <Arduino.h>
#include "hal/display.h" // pulls in TFT_eSPI (GFX free fonts come with LOAD_GFXFF)

namespace ui {

// Bind the UI helpers to the display once at boot, before any draw call.
void begin(hal::Display &disp);
TFT_eSPI &tft();

// Draw a string with a GFX free font (smoother than the built-in fonts).
void gtext(const char *s, int x, int y, const GFXfont *f, uint16_t fg,
           uint16_t bg, uint8_t datum);

// Pixel width of a string in a GFX font (selects the font as a side effect).
int textW(const char *s, const GFXfont *f);

// Like gtext, but truncates with a trailing ellipsis so it never exceeds maxW.
void gtextClamp(const char *s, int x, int y, const GFXfont *f, uint16_t fg,
                uint16_t bg, uint8_t datum, int maxW);

// Canvas-explicit variants of gtext/gtextClamp: draw on `c` (the screen or an
// off-screen sprite -- TFT_eSprite inherits TFT_eSPI so both fit). Text width
// for clamping is still measured on the real display's font engine.
void gtextC(TFT_eSPI &c, const char *s, int x, int y, const GFXfont *f,
            uint16_t fg, uint16_t bg, uint8_t datum);
void gtextClampC(TFT_eSPI &c, const char *s, int x, int y, const GFXfont *f,
                 uint16_t fg, uint16_t bg, uint8_t datum, int maxW);

// Flicker-free text update. The CYD has no double buffer, so the usual on-screen
// fillRect(bg)+drawString briefly shows the cleared cell between erase and redraw
// -- that blank frame is the "一闪一闪" flicker. Here we render the cell into an
// off-screen sprite (RAM) and blit it in ONE pass, so old text swaps to new with
// no blank in between. (rx,ry,w,h) = the on-screen cell to repaint; the string is
// positioned by `datum` at absolute (tx,ty) and clamped with an ellipsis to maxW.
void blitText(int rx, int ry, int w, int h, const char *s, int tx, int ty,
              const GFXfont *f, uint16_t fg, uint16_t bg, uint8_t datum,
              int maxW);

// Compact token count: 1.2M / 123k / 45 (bare number; the cell label gives units).
void fmtTok(long long t, char *out, size_t n);

void fmtDur(uint32_t ms, char *out, size_t n);

} // namespace ui
