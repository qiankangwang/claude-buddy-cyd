#pragma once

namespace screens {

// Confirmation before opening the WiFi captive portal (avoids accidental taps
// taking the buddy offline). Cancel/Open land on layout's deny/approve bar.
void renderWifiConfirm();

} // namespace screens
