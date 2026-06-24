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
  JsonDocument doc;
  if (deserializeJson(doc, http.arg("plain"))) {
    http.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }
  AppState &s = g_state;
  s.total = doc["total"] | s.total;
  s.running = doc["running"] | 0;
  if (doc["msg"].is<const char *>())
    s.msg = (const char *)doc["msg"];
  if (doc["project"].is<const char *>())
    s.project = (const char *)doc["project"];
  s.tokens = doc["tokens"] | s.tokens;
  s.tokensAll = doc["tokensAll"] | (long long)s.tokensAll;
  s.tools = doc["tools"] | s.tools;
  s.turns = doc["turns"] | s.turns;
  s.sessions = doc["sessions"] | s.sessions;
  // sticky per-activity clip name (typing/building/...) for the running state.
  if (doc["act"].is<const char *>())
    s.act = (const char *)doc["act"];
  // optional transient effect (attention/celebrate/heart): bump fxId so the
  // renderer fires it exactly once.
  if (doc["fx"].is<const char *>()) {
    s.fx = (const char *)doc["fx"];
    if (s.fx.length())
      s.fxId++;
  }
  // "Claude is waiting on you" sticky + intensity inputs. Every event sends
  // these explicitly (default 0/false), so e.g. a tool starting clears waiting.
  bool wasWaiting = s.waiting;
  s.waiting = doc["waiting"] | false;
  if (s.waiting && !wasWaiting)
    s.waitId++; // a fresh wait began -> restart the escalating nudge timer
  s.burst = doc["burst"] | 0;
  s.agents = doc["agents"] | 0;
  if (doc["budget"].is<long>() || doc["budget"].is<int>())
    s.budget = doc["budget"] | s.budget; // sticky once provided
  s.dirty = true;
  http.send(200, "application/json", "{\"ok\":true}");
}

// POST /ask {"tool":"Bash"} -> show an Allow/Deny prompt; the (synchronous) hook
// then polls GET /decision for the user's tap.
static void handleAsk() {
  if (!authed())
    return;
  if (http.method() != HTTP_POST) {
    http.send(405, "text/plain", "POST only");
    return;
  }
  JsonDocument doc;
  deserializeJson(doc, http.arg("plain"));
  g_state.askTool = (const char *)(doc["tool"] | "this tool");
  g_state.askId++;
  g_state.decision = ""; // reset; undecided until a tap
  g_state.dirty = true;
  http.send(200, "application/json",
            String("{\"ok\":true,\"id\":") + g_state.askId + "}");
}

// GET /decision -> {"decision":"allow"|"deny"|""} for the current pending ask.
static void handleDecision() {
  if (!authed())
    return;
  String d = (g_state.decidedId == g_state.askId) ? g_state.decision : String("");
  http.send(200, "application/json",
            String("{\"decision\":\"") + d + "\"}");
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
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false); // keep the radio awake — modem-sleep was causing
                          // intermittent drops/deauths; a steady link matters
                          // more here than the small idle-power saving
    g_state.ip = WiFi.localIP().toString();
    if (MDNS.begin("claude-cyd"))
      MDNS.addService("http", "tcp", 80);
    http.on("/event", handleEvent);
    http.on("/ask", handleAsk);
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
  // Keep link state + IP live: WiFi can drop/reconnect or renew its DHCP lease
  // after boot. Without this, wifiUp/ip stay latched at their boot values, so
  // the device shows a green "READY" link while offline and may report a stale
  // IP. (The HTTP server was begun at boot; handleClient resumes on reconnect.)
  static uint32_t lastChk = 0;
  uint32_t now = millis();
  if (now - lastChk > 2000) {
    lastChk = now;
    bool up = (WiFi.status() == WL_CONNECTED);
    if (up != g_state.wifiUp) {
      g_state.wifiUp = up;
      g_state.dirty = true;
    }
    if (up) {
      String ip = WiFi.localIP().toString();
      if (ip != g_state.ip) {
        g_state.ip = ip;
        g_state.dirty = true;
      }
    }
  }
}

AppState &Server::state() { return g_state; }

void Server::setToken(const String &t) {
  g_token = t;
  g_state.token = t;
}

void Server::wifiPortal() {
  // Non-destructive: opens the captive portal but does NOT erase saved creds, so
  // the existing password is remembered unless the user submits a new network.
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setAPCallback([](WiFiManager *) {
    if (g_portalCb)
      g_portalCb("Claude-CYD-Setup");
  });
  wm.startConfigPortal("Claude-CYD-Setup");
}

} // namespace net
