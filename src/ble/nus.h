#pragma once
#include <Arduino.h>
#include <functional>

namespace ble {

// Nordic UART Service server (the wire transport Claude Desktop's Hardware
// Buddy speaks). LE Secure bonding with a display-only passkey. BLE callbacks
// run on the NimBLE host task: they only touch queues / volatiles here, never
// the display — loop() polls and renders. See REFERENCE.md.
class Nus {
public:
  using LineHandler = std::function<void(const String &)>;

  void begin();
  void pollLines(LineHandler handler); // drain RX lines on the app task
  void send(const String &line);       // appends '\n', chunked at MTU

  bool connected() const;
  bool secure() const;
  bool consumePasskey(uint32_t &out);  // true once when a passkey must be shown
  void unpair();                        // erase all stored bonds
  const char *deviceName() const;
};

extern Nus nus;

} // namespace ble
