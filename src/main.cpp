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

static const int REG_Y = render::REG_Y, REG_H = render::REG_H; // single source

// transient animation windows
static uint32_t heartUntil = 0, dizzyUntil = 0, celebrateUntil = 0;
// power / interaction
static uint32_t lastInteraction = 0;
static bool screenOn = true, forceRedraw = false, wasTouched = false, haveChar = false;
// triple-tap detection (shift register of the last 3 tap times)
static uint32_t tapTimes[3] = {0, 0, 0};
// stats are flushed lazily to NVS (avoid a flash commit on every tap)
static bool statsDirty = false;
static uint32_t lastStatsSave = 0;

#define STATS_MAGIC 0x43425331UL
#define SCREEN_OFF_MS 30000UL
#define LEVEL_EVERY 10UL // approvals per level-up (token counts aren't available via hooks)

struct Stats {
  uint32_t magic, appr, deny, level;
};
static Stats g_stats = {STATS_MAGIC, 0, 0, 0};

static void loadStats() {
  Stats s;
  if (storage.getBytes("stats", &s, sizeof(s)) && s.magic == STATS_MAGIC)
    g_stats = s;
}
static void saveStats() { storage.putBytes("stats", &g_stats, sizeof(g_stats)); }

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

static const char *stateName(uint32_t now) {
  net::AppState &s = net::server.state();
  if (now < celebrateUntil)
    return "celebrate";
  if (now < heartUntil)
    return "heart";
  if (now < dizzyUntil)
    return "dizzy";
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
  bool fast = (now % 300) < 150;
  if (!strcmp(st, "attention"))
    led.set(blink, false, false);
  else if (!strcmp(st, "celebrate"))
    led.set(fast, !fast, true); // party
  else if (!strcmp(st, "heart"))
    led.set(false, true, false);
  else if (!strcmp(st, "dizzy"))
    led.set(true, false, true); // magenta
  else if (!strcmp(st, "busy"))
    led.set(false, false, true);
  else
    led.off();
}

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

  int top = REG_Y + REG_H;
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
    t.drawString(lbl, W / 2, top + 4, 4);
    if (s.msg.length()) {
      t.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      t.drawString(s.msg.c_str(), W / 2, top + 34, 2);
    }
    char line[40];
    snprintf(line, sizeof(line), "Lv %u   OK %u / NO %u", g_stats.level,
             g_stats.appr, g_stats.deny);
    t.setTextColor(TFT_DARKGREY, TFT_BLACK);
    t.drawString(line, W / 2, top + 62, 1);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[CYD Buddy] WiFi + official Clawd character");

  display.begin();
  storage.begin();
  touch.begin(display, storage);
  led.begin();
  computeButtons();
  loadStats();

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
    render::character.setState(stateName(millis()));
  lastInteraction = millis();
}

void loop() {
  uint32_t now = millis();
  net::server.loop();
  net::AppState &s = net::server.state();

  int16_t tx, ty;
  bool t = touch.read(tx, ty);

  // ---- asleep: any touch or a new prompt wakes us; else stay dark ----
  if (!screenOn) {
    if (touch.rawPressed() || s.hasPrompt) {
      screenOn = true;
      display.backlight(true);
      lastInteraction = now;
      forceRedraw = true;
      wasTouched = true; // consume this contact, don't fire a tap
    } else {
      wasTouched = false;
      delay(10);
      return;
    }
  }

  // ---- edge-detected tap while awake ----
  bool tap = t && !wasTouched;
  wasTouched = t;
  if (tap) {
    lastInteraction = now;
    if (s.hasPrompt && s.decision == 0) {
      if (inRect(denyBtn, tx, ty)) {
        net::server.setDecision(2);
        g_stats.deny++;
        statsDirty = true;
      } else if (inRect(approveBtn, tx, ty)) {
        net::server.setDecision(1);
        g_stats.appr++;
        uint32_t dt = now - s.promptMs;
        if (g_stats.appr % LEVEL_EVERY == 0) {
          g_stats.level++;
          celebrateUntil = now + 3500; // level up!
        } else if (dt < 5000) {
          heartUntil = now + 2200; // fast approval
        }
        statsDirty = true;
      }
    } else {
      // triple-tap anywhere -> dizzy (replaces the official "shake")
      tapTimes[0] = tapTimes[1];
      tapTimes[1] = tapTimes[2];
      tapTimes[2] = now;
      if (tapTimes[0] != 0 && tapTimes[2] - tapTimes[0] < 900) {
        dizzyUntil = now + 2200;
        tapTimes[0] = tapTimes[1] = tapTimes[2] = 0;
      }
    }
  }

  // M1: drop a stale prompt the hook has abandoned (no tap within its window),
  // so we don't keep showing a dead Approve/Deny or count a phantom approval.
  if (s.hasPrompt && s.decision == 0 && now - s.promptMs > 310000UL) {
    s.hasPrompt = false;
    s.dirty = true;
  }

  const char *st = stateName(now);

  // ---- auto screen-off when calm ----
  bool calm = !s.hasPrompt && s.running == 0 && now >= celebrateUntil &&
              now >= heartUntil && now >= dizzyUntil;
  if (screenOn && calm && now - lastInteraction > SCREEN_OFF_MS) {
    if (statsDirty) {
      saveStats();
      statsDirty = false;
    }
    screenOn = false;
    display.backlight(false);
    led.off();
  }
  if (!screenOn) {
    delay(8);
    return;
  }

  // ---- render ----
  static String last = "?";
  if (s.dirty || forceRedraw || last != st) {
    s.dirty = false;
    forceRedraw = false;
    last = st;
    renderStatic();
    if (haveChar)
      render::character.setState(st);
  }
  if (haveChar)
    render::character.update();

  static uint32_t ledT = 0;
  if (now - ledT > 100) {
    ledT = now;
    driveLed(st, now);
  }
  if (statsDirty && now - lastStatsSave > 5000) {
    saveStats();
    statsDirty = false;
    lastStatsSave = now;
  }
  delay(2);
}
