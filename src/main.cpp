#include <Arduino.h>
#include <esp_random.h>
#include "hal/display.h"
#include "hal/storage.h"
#include "hal/touch.h"
#include "hal/led.h"
#include "render/character.h"
#include "net/server.h"

static hal::Display display;
static hal::Storage storage;
static hal::Touch touch;
static hal::Led led;

static uint32_t doneUntil = 0; // show celebratory "heart" until this millis
static bool haveChar = false;

// Character region (must match render/character.cpp).
static const int REG_Y = 34, REG_H = 176;

struct Rect {
  int x, y, w, h;
};
static Rect denyBtn, approveBtn;

static void computeButtons() {
  int W = display.tft().width(), H = display.tft().height();
  int bh = 56, m = 8;
  denyBtn = {m, H - bh - 6, (W - 3 * m) / 2, bh};
  approveBtn = {denyBtn.x + denyBtn.w + m, H - bh - 6, (W - 3 * m) / 2, bh};
}

static bool inRect(const Rect &r, int x, int y) {
  return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

static void drawButton(const Rect &r, const char *label, uint16_t col) {
  TFT_eSPI &t = display.tft();
  t.fillRoundRect(r.x, r.y, r.w, r.h, 8, col);
  t.setTextColor(TFT_WHITE, col);
  t.setTextDatum(MC_DATUM);
  t.drawString(label, r.x + r.w / 2, r.y + r.h / 2, 4);
}

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

static const char *stateName() {
  net::AppState &s = net::server.state();
  if (millis() < doneUntil)
    return "heart";
  if (!s.wifiUp)
    return "sleep";
  if (s.hasPrompt)
    return "attention";
  if (s.running > 0)
    return "busy";
  if (s.total > 0)
    return "idle";
  return "sleep";
}

static void driveLed(const char *st, uint32_t now) {
  bool blink = (now % 600) < 300;
  if (!strcmp(st, "attention"))
    led.set(blink, false, false); // blinking red
  else if (!strcmp(st, "busy"))
    led.set(false, false, true); // blue
  else if (!strcmp(st, "heart"))
    led.set(false, true, false); // green
  else
    led.off();
}

// Repaint only the top bar and the bottom UI; the character region (REG_Y..) is
// owned by the GIF renderer and left untouched.
static void renderStatic() {
  TFT_eSPI &t = display.tft();
  net::AppState &s = net::server.state();
  int W = t.width(), H = t.height();

  t.fillRect(0, 0, W, REG_Y, TFT_BLACK);
  t.setTextDatum(TC_DATUM);
  t.setTextColor(TFT_CYAN, TFT_BLACK);
  t.drawString("CYD Buddy", W / 2, 2, 2);
  t.setTextColor(s.wifiUp ? TFT_GREEN : TFT_RED, TFT_BLACK);
  t.drawString(s.wifiUp ? ("IP " + s.ip).c_str() : "WiFi setup needed", W / 2,
               20, 1);

  int top = REG_Y + REG_H; // bottom area starts here (210)
  t.fillRect(0, top, W, H - top, TFT_BLACK);
  if (s.hasPrompt) {
    t.setTextDatum(TC_DATUM);
    t.setTextColor(TFT_WHITE, TFT_BLACK);
    t.drawString(s.tool.c_str(), W / 2, top + 2, 4);
    t.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    t.drawString(s.hint.c_str(), W / 2, top + 30, 2);
    if (s.decision == 0) {
      drawButton(denyBtn, "Deny", TFT_RED);
      drawButton(approveBtn, "Approve", TFT_DARKGREEN);
    } else {
      t.setTextColor(TFT_YELLOW, TFT_BLACK);
      t.drawString(s.decision == 1 ? "APPROVED" : "DENIED", W / 2, top + 64, 4);
    }
  } else {
    const char *lbl =
        s.running > 0 ? "working..." : (s.total > 0 ? "idle" : "sleeping");
    t.setTextDatum(TC_DATUM);
    t.setTextColor(TFT_WHITE, TFT_BLACK);
    t.drawString(lbl, W / 2, top + 6, 4);
    if (s.msg.length()) {
      t.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      t.drawString(s.msg.c_str(), W / 2, top + 38, 2);
    }
    if (s.tokens > 0) {
      t.setTextColor(TFT_DARKGREY, TFT_BLACK);
      t.drawString(("tokens: " + String(s.tokens)).c_str(), W / 2, top + 64, 1);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[CYD Buddy] WiFi + official GIF character");

  display.begin();
  storage.begin();
  touch.begin(display, storage);
  led.begin();
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

  haveChar = render::character.begin(display, "/clawd");
  Serial.printf("wifi=%d ip=%s char=%d\n", net::server.state().wifiUp,
                net::server.state().ip.c_str(), haveChar);

  display.tft().fillScreen(TFT_BLACK);
  renderStatic();
  if (haveChar)
    render::character.setState(stateName());
}

void loop() {
  net::server.loop();

  net::AppState &s = net::server.state();
  if (s.hasPrompt && s.decision == 0) {
    int16_t x, y;
    if (touch.read(x, y)) {
      if (inRect(denyBtn, x, y))
        net::server.setDecision(2);
      else if (inRect(approveBtn, x, y)) {
        net::server.setDecision(1);
        doneUntil = millis() + 2000;
      }
    }
  }

  const char *st = stateName();
  static String last = "?";
  if (s.dirty || last != st) {
    s.dirty = false;
    last = st;
    renderStatic();
    if (haveChar)
      render::character.setState(st);
  }
  if (haveChar)
    render::character.update();

  static uint32_t ledT = 0;
  uint32_t now = millis();
  if (now - ledT > 100) {
    ledT = now;
    driveLed(st, now);
  }
  delay(2);
}
