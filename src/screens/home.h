#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "ui/theme.h"

namespace screens {

// Home screen: top status bar + (character region) + bottom stats card. The
// stats card's counters are animated (eased) copies of the live values, so a
// number rolls toward its new total like an odometer instead of snapping.
// The bottom card has two pages (swipe to switch): 0 = the stats card,
// 1 = the trends card. Clawd and the top bar stay put on both.

int card();          // current card page (0 stats / 1 trends)
void setCard(int c); // switch page; the caller triggers the repaint

// Full repaint of the home chrome (top bar + stats card / needs-you ack pill).
// A full repaint flashes, so callers reserve it for real layout changes.
void renderStatic(const char *st);

// Draw the full stats page onto any canvas (yOrg translates screen-y into
// canvas space; live=true syncs the incremental repaint caches). Used by the
// full home repaint and by the card-slide snapshots (ui::PAL_IDX + 4bpp).
void drawStatsPage(TFT_eSPI &c, int yOrg, const char *st,
                   const ui::CardPal &p, bool live);

// Update ONLY the top-bar status dot + label in place (no flash).
void renderStatusBar(const char *st);

// Repaint ONLY the headline band inside the card (so rotating the busy verb
// doesn't flicker the stats grid, which refreshes on its own data changes).
void renderHeadline(const char *st);

// Session-intensity pips in the top bar (1 dot busy, 2 intense, none calm).
void renderIntensity();

// Top-bar battery glyph (software gauge). renderBattery draws unconditionally
// (full repaints); renderBatteryIfChanged repaints only when the 5% bucket or
// warning color moved, so the loop can call it cheaply.
void renderBattery();
void renderBatteryIfChanged();

// Seed the odometer counters from the (restored) live values at boot, so they
// read true at once instead of rolling up from zero on the first frame.
void seedStats();

// Roll the stat counters toward their live targets (odometer feel) and re-tick
// the session timer; repaints only cells whose text changed. Call every loop.
void rollStats(uint32_t now, const char *st);

// Advance the rotating busy verb / idle line (the caller repaints the headline).
void rotateVerb();
void rotateIdle();

} // namespace screens
