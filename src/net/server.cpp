#include "server.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>

namespace net {

Server server;
static AppState g_state;
static WebServer http(80);
static std::function<void(const String &)> g_portalCb;
static String g_token;

// Reject requests without a matching X-Buddy-Token (when a token is set).
static bool authed() {
  if (g_token.length() == 0)
    return true;
  if (http.header("X-Buddy-Token") == g_token)
    return true;
  http.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
  return false;
}

static void handleEvent() {
  if (!authed())
    return;
  if (http.method() != HTTP_POST) {
    http.send(405, "text/plain", "POST only");
    return;
  }
  String body = http.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    http.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }
  AppState &s = g_state;
  s.total = doc["total"] | s.total;
  s.running = doc["running"] | 0;
  s.waiting = doc["waiting"] | 0;
  if (doc["msg"].is<const char *>())
    s.msg = (const char *)doc["msg"];
  s.tokens = doc["tokens"] | s.tokens;

  if (doc["prompt"].is<JsonObject>()) {
    JsonObject p = doc["prompt"];
    s.hasPrompt = true;
    s.promptId = (const char *)(p["id"] | "");
    s.tool = (const char *)(p["tool"] | "");
    s.hint = (const char *)(p["hint"] | "");
    s.decision = 0; // fresh prompt awaits a decision
  }
  s.dirty = true;
  http.send(200, "application/json", "{\"ok\":true}");
}

static void handleDecision() {
  if (!authed())
    return;
  AppState &s = g_state;
  const char *d =
      s.decision == 1 ? "allow" : s.decision == 2 ? "deny" : "pending";
  String out = String("{\"decision\":\"") + d + "\"}";
  http.send(200, "application/json", out);
  if (s.decision != 0) { // resolved and now reported -> clear
    s.hasPrompt = false;
    s.decision = 0;
    s.dirty = true;
  }
}

void Server::begin(std::function<void(const String &)> onPortal) {
  g_portalCb = onPortal;
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setAPCallback([](WiFiManager *) {
    if (g_portalCb)
      g_portalCb("Claude-CYD-Setup");
  });
  bool ok = wm.autoConnect("Claude-CYD-Setup");
  g_state.wifiUp = ok;
  if (ok) {
    g_state.ip = WiFi.localIP().toString();
    if (MDNS.begin("claude-cyd"))
      MDNS.addService("http", "tcp", 80);
    http.on("/event", handleEvent);
    http.on("/decision", handleDecision);
    http.on("/", []() {
      http.send(200, "text/plain", String("CYD Buddy OK ip=") + g_state.ip);
    });
    const char *hdrs[] = {"X-Buddy-Token"};
    http.collectHeaders(hdrs, 1);
    http.begin();
  }
  g_state.dirty = true;
}

void Server::loop() {
  if (g_state.wifiUp)
    http.handleClient();
}

AppState &Server::state() { return g_state; }

void Server::setDecision(int d) {
  g_state.decision = d;
  g_state.dirty = true;
}

void Server::setToken(const String &t) {
  g_token = t;
  g_state.token = t;
}

} // namespace net
