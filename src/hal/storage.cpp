#include "storage.h"
#include <Preferences.h>

namespace hal {

static Preferences prefs;

void Storage::begin() {
  if (!prefs.begin("buddy", false))
    Serial.println("[storage] NVS open failed (settings won't persist)");
}

bool Storage::putBytes(const char *key, const void *buf, size_t len) {
  return prefs.putBytes(key, buf, len) == len;
}

bool Storage::getBytes(const char *key, void *buf, size_t len) {
  return prefs.getBytes(key, buf, len) == len;
}

} // namespace hal
