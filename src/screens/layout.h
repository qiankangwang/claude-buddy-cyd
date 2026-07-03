#pragma once
#include <TFT_eSPI.h>
#include "ui/widgets.h"

namespace screens {

// Shared tap-target layout, computed once from the panel size at boot: the
// bottom Deny/Approve action bar (wifi-confirm + ask screens), the Settings
// rows, and the "Got it" pill on the needs-you screen.
extern ui::Rect denyBtn, approveBtn;
extern ui::Rect ackBtn; // "Got it" pill on the needs-you screen (dismisses -> idle)
// Settings: Power off / Stats / Quiet / Brightness / Recalibrate / WiFi / Close
extern ui::Rect setBtns[7];

void computeButtons(TFT_eSPI &t);

} // namespace screens
