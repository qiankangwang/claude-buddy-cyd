#pragma once
#include <Arduino.h>

namespace ui {

struct Rect {
  int x, y, w, h;
};

bool inRect(const Rect &r, int x, int y);

void drawButton(const Rect &r, const char *label, uint16_t col);

} // namespace ui
