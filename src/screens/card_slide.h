#pragma once
#include <TFT_eSPI.h>

namespace screens {

// Bottom-card slide transition (docs/card-slide-spec.md): the two pages sit
// side by side (0 stats on the left, 1 trends on the right). slideCard eases
// the target page in over ~250 ms and commits it via setCard; its last frame
// is pixel-exact, so callers must NOT follow up with a full repaint (that
// would flash). bounceCard is the rubber-band "end of row" nudge (dir +1 =
// nudge right). Both degrade gracefully on low heap.
void slideCard(int target, const char *st);
void bounceCard(int dir, const char *st);

} // namespace screens
