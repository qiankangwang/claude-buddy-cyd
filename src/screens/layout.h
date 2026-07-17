#pragma once
#include <TFT_eSPI.h>
#include "ui/widgets.h"

namespace screens {

// Shared tap-target layout, computed once from the panel size at boot: the
// bottom Deny/Approve action bar (ask screen), the Settings rows, and the
// "Got it" pill on the needs-you screen.
extern ui::Rect denyBtn, approveBtn;
extern ui::Rect ackBtn; // "Got it" pill on the needs-you screen (dismisses -> idle)
// Settings: Power off / Stats / Quiet / Brightness / Recalibrate / Close
extern ui::Rect setBtns[6];

void computeButtons(TFT_eSPI &t);

} // namespace screens
