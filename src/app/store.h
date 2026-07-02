#pragma once
#include <Arduino.h>
#include "hal/storage.h"

namespace app {

// Device auth token: load from NVS, or generate + persist one on first boot.
String loadOrCreateToken(hal::Storage &storage);

// ---- stats persistence (survive an unplug) ---------------------------------
// The PC hook is the source of truth: it recomputes today/all-time from the
// transcripts and re-pushes a full snapshot on the next event. So the device
// only needs a *display cache* -- mirror the last snapshot to NVS and restore it
// on boot, so a replug shows the last numbers immediately instead of zeros until
// Claude next does something. The first /event after boot overwrites these with
// the authoritative totals, so the device never accumulates locally and a
// restore can't double-count. Persisted counters only (running/waiting stay 0).

// Restore the cached snapshot into AppState (called once at boot).
void restoreStats(hal::Storage &storage);

// Write the snapshot to NVS, but only when a counter actually changed -- NVS has
// finite write endurance and the hook can fire many events a minute, so a blind
// write-per-event would wear the flash. force=true bypasses the time throttle
// (used at natural pauses: screen-off / power-off) but still skips a no-op write.
bool saveStatsIfChanged(hal::Storage &storage, bool force);

} // namespace app
