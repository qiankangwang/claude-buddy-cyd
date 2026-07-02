#pragma once

namespace screens {

// "Allow this tool?" — shown when a synchronous PermissionRequest hook is
// waiting on the device. A tap approves/denies just this one pending call.
void renderAsk();

} // namespace screens
