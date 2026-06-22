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
  // latest session snapshot from the hook
  int total = 0, running = 0;
  String msg;
  String project;      // current project (cwd basename)
  long tokens = 0;     // tokens used today
  long tokensAll = 0;  // tokens used all-time (cumulative)
  int tools = 0;       // tool calls today
  int turns = 0;       // assistant turns today
  int sessions = 0;    // sessions today
  bool dirty = true;   // renderer should repaint
};

// WiFi (captive-portal provisioned) + HTTP server. Endpoints:
//   POST /event  -> session/stats snapshot (JSON body)
//   GET  /       -> health
class Server {
public:
  // onPortal(apName) is called if WiFi needs provisioning, so the caller can
  // draw setup instructions while autoConnect blocks.
  void setToken(const String &t); // call before begin()
  void begin(std::function<void(const String &)> onPortal);
  void loop();
  AppState &state();
  void wifiPortal();       // open config portal WITHOUT erasing saved creds
                           // (keeps the old password unless you enter a new one)
};

extern Server server;

} // namespace net
