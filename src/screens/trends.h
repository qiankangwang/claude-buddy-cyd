#pragma once
#include <TFT_eSPI.h>
#include "ui/theme.h"

namespace screens {

// Trends card: the second page of the home screen's bottom card (swipe to
// switch). Clawd and the top status bar stay put; only the card area changes.
// Shows a bar per day from the usage-history ring (newest right, in coral).
// full=true paints the whole card; full=false is the throttled live refresh
// (today's bar grows) and repaints only the plot + summary, flicker-free.
void renderTrends(bool full);

// Draw the full trends page onto any canvas (yOrg translates screen-y into
// canvas space; 0 = the screen). Used by the full repaint and by the
// card-slide snapshots (ui::PAL_IDX + 4bpp palette sprite).
void drawTrendsPage(TFT_eSPI &c, int yOrg, const ui::CardPal &p);

} // namespace screens
