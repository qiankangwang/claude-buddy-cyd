#pragma once
#include <Arduino.h>
#include "hal/storage.h"

namespace app {

// Rolling per-day token history, keyed by the PC-local "YYYY-MM-DD" the hook
// sends with every event -- the device needs no clock/NTP/timezone of its own,
// the PC stays the source of truth. The newest entry mirrors "today" and is
// updated in place; a fresh date appends, dropping the oldest past the cap.
// Like the stats snapshot this is a display cache, not a ledger: last write
// wins, and days the device slept through simply keep their last-seen total.

#define HISTORY_DAYS 30

struct DayStat {
  char date[11]; // "YYYY-MM-DD"
  long tokens;
};

// Feed the latest snapshot (call every loop; cheap when nothing changed).
void historyNote(const String &date, long tokens);

// Days oldest -> newest. Returns the internal ring; valid until the next note.
const DayStat *historyDays(int &count);

// NVS persistence, same shape as the stats snapshot: restore once at boot;
// save throttled on change, force=true at natural pauses (screen-off, power-off).
void historyRestore(hal::Storage &storage);
bool historySaveIfChanged(hal::Storage &storage, bool force);

} // namespace app
