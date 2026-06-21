#pragma once
#include <Arduino.h>
#include <functional>

namespace net {

// Shared app state, updated by HTTP handlers (which run in the loop() thread via
// handleClient) and read by the renderer. Single-threaded, no locking needed.
struct AppState {
  bool wifiUp = false;
  String ip;
  String token; // shared secret; hooks must send it as X-Buddy-Token
  // latest session snapshot
  int total = 0, running = 0, waiting = 0;
  String msg;
  long tokens = 0;
  // pending permission prompt
  bool hasPrompt = false;
  String promptId, tool, hint;
  int decision = 0; // 0 none, 1 allow, 2 deny
  bool dirty = true; // renderer should repaint
};

// WiFi (captive-portal provisioned) + HTTP server. Endpoints:
//   POST /event     -> session snapshot / permission prompt (JSON body)
//   GET  /decision  -> {"decision":"allow"|"deny"|"pending"} (clears on read)
//   GET  /          -> health
class Server {
public:
  // onPortal(apName) is called if WiFi needs provisioning, so the caller can
  // draw setup instructions while autoConnect blocks.
  void setToken(const String &t); // call before begin()
  void begin(std::function<void(const String &)> onPortal);
  void loop();
  AppState &state();
  void setDecision(int d); // 1=allow, 2=deny (from touch)
};

extern Server server;

} // namespace net
