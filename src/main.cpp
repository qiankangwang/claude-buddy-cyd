#include <Arduino.h>
#include <esp_random.h>
#include "hal/display.h"
#include "hal/storage.h"
#include "hal/touch.h"
#include "net/server.h"

static hal::Display display;
static hal::Storage storage;
static hal::Touch touch;

struct Rect {
  int x, y, w, h;
};
static Rect denyBtn, approveBtn;

static void computeButtons() {
  int W = display.tft().width(), H = display.tft().height();
  int bh = 64, m = 8;
  denyBtn = {m, H - bh - m, (W - 3 * m) / 2, bh};
  approveBtn = {denyBtn.x + denyBtn.w + m, H - bh - m, (W - 3 * m) / 2, bh};
}

static bool inRect(const Rect &r, int x, int y) {
  return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

// Persisted random shared secret (16 hex chars). Hooks send it as X-Buddy-Token.
static String loadOrCreateToken() {
  char buf[17];
  if (storage.getBytes("token", buf, 16)) {
    buf[16] = 0;
    return String(buf);
  }
  char out[17];
  for (int i = 0; i < 8; i++)
    sprintf(out + i * 2, "%02x", (unsigned)(esp_random() & 0xFF));
  out[16] = 0;
  storage.putBytes("token", out, 16);
  return String(out);
}

static void drawButton(const Rect &r, const char *label, uint16_t col) {
  TFT_eSPI &t = display.tft();
  t.fillRoundRect(r.x, r.y, r.w, r.h, 8, col);
  t.setTextColor(TFT_WHITE, col);
  t.setTextDatum(MC_DATUM);
  t.drawString(label, r.x + r.w / 2, r.y + r.h / 2, 4);
}

static void render() {
  TFT_eSPI &t = display.tft();
  net::AppState &s = net::server.state();
  t.fillScreen(TFT_BLACK);
  t.setTextDatum(TC_DATUM);
  t.setTextColor(TFT_CYAN, TFT_BLACK);
  t.drawString("CYD Buddy", t.width() / 2, 6, 2);
  t.setTextColor(s.wifiUp ? TFT_GREEN : TFT_RED, TFT_BLACK);
  t.drawString(s.wifiUp ? ("IP " + s.ip).c_str() : "WiFi down", t.width() / 2,
               26, 1);
  if (s.wifiUp && s.token.length()) {
    t.setTextColor(TFT_DARKGREY, TFT_BLACK);
    t.drawString(("token " + s.token).c_str(), t.width() / 2, 40, 1);
  }

  if (s.hasPrompt) {
    t.setTextColor(TFT_ORANGE, TFT_BLACK);
    t.drawString("Permission needed", t.width() / 2, 62, 4);
    t.setTextColor(TFT_WHITE, TFT_BLACK);
    t.drawString(s.tool.c_str(), t.width() / 2, 98, 4);
    t.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    t.drawString(s.hint.c_str(), t.width() / 2, 132, 2);
    if (s.decision == 0) {
      drawButton(denyBtn, "Deny", TFT_RED);
      drawButton(approveBtn, "Approve", TFT_DARKGREEN);
    } else {
      t.setTextColor(TFT_YELLOW, TFT_BLACK);
      t.drawString(s.decision == 1 ? "APPROVED" : "DENIED", t.width() / 2,
                   t.height() / 2 + 60, 4);
    }
  } else {
    const char *st =
        s.running > 0 ? "working..." : (s.total > 0 ? "idle" : "sleeping");
    t.setTextColor(TFT_WHITE, TFT_BLACK);
    t.setTextDatum(MC_DATUM);
    t.drawString(st, t.width() / 2, t.height() / 2 - 10, 4);
    if (s.msg.length()) {
      t.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      t.drawString(s.msg.c_str(), t.width() / 2, t.height() / 2 + 24, 2);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[CYD Buddy] WiFi + HTTP server");

  display.begin();
  storage.begin();
  touch.begin(display, storage);
  computeButtons();

  net::server.setToken(loadOrCreateToken());

  TFT_eSPI &t = display.tft();
  t.fillScreen(TFT_BLACK);
  t.setTextDatum(MC_DATUM);
  t.setTextColor(TFT_WHITE, TFT_BLACK);
  t.drawString("Connecting WiFi...", t.width() / 2, t.height() / 2, 2);

  net::server.begin([](const String &ap) {
    TFT_eSPI &d = display.tft();
    d.fillScreen(TFT_BLACK);
    d.setTextDatum(MC_DATUM);
    d.setTextColor(TFT_YELLOW, TFT_BLACK);
    d.drawString("WiFi setup", d.width() / 2, d.height() / 2 - 56, 4);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString("Join WiFi hotspot:", d.width() / 2, d.height() / 2 - 14, 2);
    d.drawString(ap.c_str(), d.width() / 2, d.height() / 2 + 12, 4);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString("then pick your network", d.width() / 2, d.height() / 2 + 48,
                 2);
  });

  Serial.printf("wifi=%d ip=%s\n", net::server.state().wifiUp,
                net::server.state().ip.c_str());
  render();
}

void loop() {
  net::server.loop();

  net::AppState &s = net::server.state();
  if (s.hasPrompt && s.decision == 0) {
    int16_t x, y;
    if (touch.read(x, y)) {
      if (inRect(denyBtn, x, y))
        net::server.setDecision(2);
      else if (inRect(approveBtn, x, y))
        net::server.setDecision(1);
    }
  }

  if (s.dirty) {
    s.dirty = false;
    render();
  }
  delay(10);
}
