#pragma once

namespace screens {

// Trends card: the second page of the home screen's bottom card (swipe to
// switch). Clawd and the top status bar stay put; only the card area changes.
// Shows a bar per day from the usage-history ring (newest right, in coral).
// full=true paints the whole card; full=false is the throttled live refresh
// (today's bar grows) and repaints only the plot + summary, flicker-free.
void renderTrends(bool full);

} // namespace screens
