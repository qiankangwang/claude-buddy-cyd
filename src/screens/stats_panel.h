#pragma once

namespace screens {

// Full live Stats panel (Settings -> Stats). full=true draws the whole panel;
// full=false only refreshes the value cells (labels stay) so the panel can
// live-update without a full-screen flash.
void renderStats(bool full);

} // namespace screens
