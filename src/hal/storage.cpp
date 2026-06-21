#include "storage.h"
#include <Preferences.h>

namespace hal {

static Preferences prefs;

void Storage::begin() { prefs.begin("buddy", false); }

bool Storage::putBytes(const char *key, const void *buf, size_t len) {
  return prefs.putBytes(key, buf, len) == len;
}

bool Storage::getBytes(const char *key, void *buf, size_t len) {
  return prefs.getBytes(key, buf, len) == len;
}

void Storage::remove(const char *key) { prefs.remove(key); }

} // namespace hal
