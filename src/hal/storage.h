#pragma once
#include <Arduino.h>

namespace hal {

// Thin NVS (Preferences) wrapper for persisted settings: touch calibration,
// stats, owner name, species choice, etc. One namespace ("buddy") for all keys.
class Storage {
public:
  void begin();
  bool putBytes(const char *key, const void *buf, size_t len);
  bool getBytes(const char *key, void *buf, size_t len);
  void remove(const char *key);
};

} // namespace hal
