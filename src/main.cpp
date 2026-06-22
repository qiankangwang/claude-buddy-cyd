#include <Arduino.h>
#include <esp_random.h>
#include "hal/display.h" // pulls in TFT_eSPI (GFX free fonts come with LOAD_GFXFF)
#include "hal/storage.h"
#include "hal/touch.h"
#include "hal/led.h"
#include "render/character.h"
#include "net/server.h"

// Palette (RGB565). Coral = Anthropic accent (#D97757).
#define C_CORAL 0xDBAA
#define C_TEXT TFT_WHITE
#define C_MUTED 0x8C71
#define C_OK 0x256C   // refined green
#define C_NO 0xC1C5   // muted red
#define C_FACE 0x2966 // neutral button face (dark slate)
#define C_CARD 0x10A3 // info-card fill (very dark slate)

static hal::Display display;
static hal::Storage storage;
static hal::Touch touch;
static hal::Led led;

static const int REG_Y = render::REG_Y, REG_H = render::REG_H; // single source

// transient animation window (triple-tap easter egg)
static uint32_t dizzyUntil = 0;
// power / interaction
static uint32_t lastInteraction = 0;
static uint32_t sessionStart = 0; // millis when the current Claude session began
static bool screenOn = true, forceRedraw = false, wasTouched = false, haveChar = false;
// triple-tap detection (shift register of the last 3 tap times)
static uint32_t tapTimes[3] = {0, 0, 0};

// rotating "busy" verb, advanced in sync with the character animation loop
static const char *WHIMSY[] = {
    "Whirring...",  "Pondering...",   "Brewing...",   "Churning...",
    "Noodling...",  "Cogitating...",  "Conjuring...", "Percolating...",
    "Simmering...", "Marinating...",  "Mulling...",   "Vibing...",
    "Crunching...", "Stewing...",     "Spinning...",  "Forging...",
    "Hatching...",  "Musing...",      "Cooking...",   "Tinkering..."};
static const int N_WHIMSY = sizeof(WHIMSY) / sizeof(WHIMSY[0]);
static int verbIdx = 0;
static uint32_t lastLoops = 0;

// settings / stats / wifi-confirm screens (long-press to open settings)
static bool settingsOpen = false;
static bool statsOpen = false;
static bool wifiConfirmOpen = false;
static uint32_t pressStart = 0;
static bool longFired = false;

#define SCREEN_OFF_MS 30000UL

struct Rect {
  int x, y, w, h;
};
static Rect denyBtn, approveBtn;
static Rect setBtns[4]; // Settings: Stats / Recalibrate / WiFi setup / Close

static void computeButtons() {
  int W = display.tft().width(), H = display.tft().height();
  int bh = 56, m = 8;
  denyBtn = {m, H - bh - 6, (W - 3 * m) / 2, bh};
  approveBtn = {denyBtn.x + denyBtn.w + m, H - bh - 6, (W - 3 * m) / 2, bh};
  int sy = 56, sbh = 46, gap = 14;
  for (int i = 0; i < 4; i++)
    setBtns[i] = {20, sy + i * (sbh + gap), W - 40, sbh};
}

static bool inRect(const Rect &r, int x, int y) {
  return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

// Draw a string with a GFX free font (smoother than the built-in fonts).
static void gtext(const char *s, int x, int y, const GFXfont *f, uint16_t fg,
                  uint16_t bg, uint8_t datum) {
  TFT_eSPI &t = display.tft();
  t.setFreeFont(f);
  t.setTextDatum(datum);
  t.setTextColor(fg, bg);
  t.drawString(s, x, y);
}

static void drawButton(const Rect &r, const char *label, uint16_t col) {
  TFT_eSPI &t = display.tft();
  t.fillRoundRect(r.x, r.y, r.w, r.h, 12, col);
  t.drawRoundRect(r.x, r.y, r.w, r.h, 12, 0x4A69); // subtle light border
  gtext(label, r.x + r.w / 2, r.y + r.h / 2, &FreeSansBold9pt7b, C_TEXT, col,
        MC_DATUM);
}

// Pixel width of a string in a GFX font (selects the font as a side effect).
static int textW(const char *s, const GFXfont *f) {
  display.tft().setFreeFont(f);
  return display.tft().textWidth(s);
}

// Like gtext, but truncates with a trailing ellipsis so it never exceeds maxW.
static void gtextClamp(const char *s, int x, int y, const GFXfont *f, uint16_t fg,
                       uint16_t bg, uint8_t datum, int maxW) {
  if (textW(s, f) <= maxW) {
    gtext(s, x, y, f, fg, bg, datum);
    return;
  }
  String str(s);
  while (str.length() > 1 &&
         textW((str + "...").c_str(), f) > maxW)
    str.remove(str.length() - 1);
  gtext((str + "...").c_str(), x, y, f, fg, bg, datum);
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
  if (now < dizzyUntil)
    return "dizzy";
  if (!s.wifiUp)
    return "sleep";
  if (s.running > 0)
    return "busy";
  if (s.total > 0)
    return "idle";
  return "sleep";
}

static void driveLed(const char *st, uint32_t now) {
  if (!strcmp(st, "dizzy"))
    led.set(true, false, true); // magenta
  else if (!strcmp(st, "busy"))
    led.set(false, false, true); // blue
  else
    led.off();
}

static uint16_t stateColor(const char *st) {
  if (!strcmp(st, "busy")) return C_CORAL;  // orange
  if (!strcmp(st, "dizzy")) return 0xA81F;  // purple
  if (!strcmp(st, "idle")) return 0x2DEA;   // green (connected, ready)
  return 0x4208;                            // asleep grey
}
static const char *stateLabel(const char *st) {
  if (!strcmp(st, "busy")) return "WORKING";
  if (!strcmp(st, "dizzy")) return "DIZZY";
  if (!strcmp(st, "idle")) return "READY";
  return "ASLEEP";
}

// Compact token count: 1.2M / 123k / 45 (bare number; the cell label gives units).
static void fmtTok(long t, char *out, size_t n) {
  if (t >= 1000000)
    snprintf(out, n, "%ld.%ldM", t / 1000000, (t % 1000000) / 100000);
  else if (t >= 1000)
    snprintf(out, n, "%ld.%ldk", t / 1000, (t % 1000) / 100);
  else
    snprintf(out, n, "%ld", t);
}

static void fmtDur(uint32_t ms, char *out, size_t n) {
  uint32_t s = ms / 1000;
  if (s < 60)
    snprintf(out, n, "%lus", (unsigned long)s);
  else if (s < 3600)
    snprintf(out, n, "%lum", (unsigned long)(s / 60));
  else
    snprintf(out, n, "%luh%02lum", (unsigned long)(s / 3600),
             (unsigned long)((s % 3600) / 60));
}

// One stacked stat: a small muted label above a bold value, both centered at
// cx. The value owns a full column, so numbers are never truncated.
static void statCol(int cx, int y, const char *label, const char *value,
                    uint16_t vcol) {
  gtext(label, cx, y, &FreeSans9pt7b, C_MUTED, C_CARD, TC_DATUM);
  gtext(value, cx, y + 15, &FreeSansBold9pt7b, vcol, C_CARD, TC_DATUM);
}

// Card headline text: while busy, the device-rotated whimsy verb (synced to the
// animation); otherwise the hook activity msg, else project, else name.
static const char *headlineText(const char *st) {
  net::AppState &s = net::server.state();
  if (!strcmp(st, "busy"))
    return WHIMSY[verbIdx];
  if (s.msg.length())
    return s.msg.c_str();
  if (s.project.length())
    return s.project.c_str();
  return "Claude Buddy";
}

// Repaint ONLY the headline band inside the card (so rotating the busy verb
// doesn't flicker the stats grid, which refreshes on its own data changes).
static void renderHeadline(const char *st) {
  TFT_eSPI &t = display.tft();
  int W = t.width(), cy = REG_Y + REG_H + 4;
  t.fillRect(12, cy + 5, W - 24, 28, C_CARD); // clear the headline band only
  gtextClamp(headlineText(st), W / 2, cy + 18, &FreeSansBold12pt7b, C_TEXT,
             C_CARD, MC_DATUM, W - 32);
}

// Home screen: top status bar + (character region) + bottom stats card.
static void renderStatic(const char *st) {
  TFT_eSPI &t = display.tft();
  net::AppState &s = net::server.state();
  int W = t.width(), H = t.height();

  // top status bar
  t.fillRect(0, 0, W, REG_Y, TFT_BLACK);
  t.fillCircle(13, 13, 5, stateColor(st));
  gtext(stateLabel(st), 26, 14, &FreeSansBold9pt7b, C_TEXT, TFT_BLACK, ML_DATUM);
  t.fillCircle(W - 13, 13, 4, s.wifiUp ? 0x2DEA : 0x9000); // link indicator

  // bottom stats card (just below the character region; cy = REG_Y+REG_H+4)
  int cy = REG_Y + REG_H + 4;
  int chh = H - cy - 6;
  t.fillRect(0, REG_Y + REG_H, W, H - (REG_Y + REG_H), TFT_BLACK);
  t.fillRoundRect(8, cy, W - 16, chh, 12, C_CARD);

  // headline (verb when busy, else activity/project) -- shared with renderHeadline
  gtextClamp(headlineText(st), W / 2, cy + 18, &FreeSansBold12pt7b, C_TEXT,
             C_CARD, MC_DATUM, W - 32);
  t.drawFastHLine(20, cy + 40, W - 40, 0x2945); // divider (gap below the text)

  // 3x2 stacked grid: each value owns a ~80px column -> no ellipsis on numbers.
  char tok[12], all[12], dur[14], nt[8], ns[8], nu[8];
  fmtTok(s.tokens, tok, sizeof(tok));
  fmtTok(s.tokensAll, all, sizeof(all));
  fmtDur(sessionStart ? (millis() - sessionStart) : 0, dur, sizeof(dur));
  snprintf(nt, sizeof(nt), "%d", s.tools);
  snprintf(ns, sizeof(ns), "%d", s.sessions);
  snprintf(nu, sizeof(nu), "%d", s.turns);
  int c1 = 40, c2 = 120, c3 = 200, yA = cy + 46, yB = cy + 90;
  statCol(c1, yA, "Today", tok, C_CORAL);
  statCol(c2, yA, "Total", all, C_CORAL);
  statCol(c3, yA, "Tools", nt, C_TEXT);
  statCol(c1, yB, "Sess", ns, C_TEXT);
  statCol(c2, yB, "Turns", nu, C_TEXT);
  statCol(c3, yB, "Time", dur, C_TEXT);
}

static void renderStats() {
  TFT_eSPI &t = display.tft();
  net::AppState &s = net::server.state();
  int W = t.width();
  t.fillScreen(TFT_BLACK);
  gtext("Stats", W / 2, 16, &FreeSansBold18pt7b, C_CORAL, TFT_BLACK, TC_DATUM);
  // label left, value right-aligned to the screen edge and clamped so long
  // values (IP / project / big token counts) can't run off the right side.
  int lx = 18, rx = W - 18, y = 60, dy = 24, vMax = W - 18 - 96;
  char b[24];
  auto row = [&](const char *k, const String &v) {
    gtext(k, lx, y, &FreeSans9pt7b, C_MUTED, TFT_BLACK, TL_DATUM);
    gtextClamp(v.c_str(), rx, y, &FreeSansBold9pt7b, C_TEXT, TFT_BLACK,
               TR_DATUM, vMax);
    y += dy;
  };
  fmtTok(s.tokens, b, sizeof(b));
  row("Today tok", String(b));
  fmtTok(s.tokensAll, b, sizeof(b));
  row("All-time tok", String(b));
  row("Tool calls", String(s.tools));
  row("Sessions", String(s.sessions));
  row("Turns", String(s.turns));
  fmtDur(sessionStart ? (millis() - sessionStart) : 0, b, sizeof(b));
  row("Session", String(b));
  row("Project", s.project.length() ? s.project : String("-"));
  snprintf(b, sizeof(b), "%lu min", (unsigned long)(millis() / 60000UL));
  row("Uptime", String(b));
  snprintf(b, sizeof(b), "%u KB", (unsigned)(ESP.getFreeHeap() / 1024));
  row("Free heap", String(b));
  row("IP", s.ip);
  gtext("tap to close", W / 2, 310, &FreeSans9pt7b, C_MUTED, TFT_BLACK,
        BC_DATUM);
}

static void renderSettings() {
  TFT_eSPI &t = display.tft();
  int W = t.width();
  t.fillScreen(TFT_BLACK);
  gtext("Settings", W / 2, 18, &FreeSansBold18pt7b, C_CORAL, TFT_BLACK,
        TC_DATUM);
  const char *labels[4] = {"Stats", "Recalibrate", "WiFi setup", "Close"};
  for (int i = 0; i < 4; i++)
    drawButton(setBtns[i], labels[i], C_FACE);
}

static void renderWifiConfirm() {
  TFT_eSPI &t = display.tft();
  int W = t.width();
  t.fillScreen(TFT_BLACK);
  gtext("WiFi setup", W / 2, 40, &FreeSansBold18pt7b, C_CORAL, TFT_BLACK,
        TC_DATUM);
  gtext("Opens a setup hotspot.", W / 2, 96, &FreeSans9pt7b, C_TEXT, TFT_BLACK,
        TC_DATUM);
  gtext("Buddy is offline a few min.", W / 2, 122, &FreeSans9pt7b, C_MUTED,
        TFT_BLACK, TC_DATUM);
  gtext("Old password is kept.", W / 2, 146, &FreeSans9pt7b, C_MUTED, TFT_BLACK,
        TC_DATUM);
  drawButton(denyBtn, "Cancel", C_FACE);
  drawButton(approveBtn, "Open", C_OK);
}

static void handleSettingsTap(int x, int y) {
  for (int i = 0; i < 4; i++) {
    if (!inRect(setBtns[i], x, y))
      continue;
    if (i == 0) { // open the stats panel
      settingsOpen = false;
      statsOpen = true;
      renderStats();
    } else if (i == 1) { // recalibrate touch (shows visible targets)
      settingsOpen = false;
      touch.calibrate(display);
      forceRedraw = true;
    } else if (i == 2) { // WiFi setup -> confirm first (avoids accidental taps)
      settingsOpen = false;
      wifiConfirmOpen = true;
      renderWifiConfirm();
    } else { // close
      settingsOpen = false;
      forceRedraw = true;
    }
    return;
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
  renderStatic(stateName(millis()));
  if (haveChar)
    render::character.setState(stateName(millis()));
  lastInteraction = millis();
}

void loop() {
  uint32_t now = millis();
  net::server.loop();
  net::AppState &s = net::server.state();

  // Track the session's start (total 0->1 edge) every loop -- even while asleep,
  // so a session that begins during sleep stamps the real start, not wake time.
  static int prevTotal = 0;
  if (s.total > 0 && prevTotal == 0)
    sessionStart = now;
  else if (s.total == 0)
    sessionStart = 0;
  prevTotal = s.total;

  int16_t tx, ty;
  bool t = touch.read(tx, ty);

  // ---- asleep: a touch, or Claude starting work, wakes us; else stay dark ----
  if (!screenOn) {
    if (touch.rawPressed() || s.running > 0) {
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

  // ---- edge-detected tap ----
  bool tap = t && !wasTouched;
  if (tap) {
    pressStart = now;
    longFired = false;
  }

  // ---- a full-screen mode left idle past the timeout closes back to home,
  //      so the backlight still powers down if the user walks away from it ----
  if ((settingsOpen || statsOpen || wifiConfirmOpen) &&
      now - lastInteraction > SCREEN_OFF_MS) {
    settingsOpen = statsOpen = wifiConfirmOpen = false;
    forceRedraw = true;
  }

  // ---- settings menu mode ----
  if (settingsOpen) {
    if (tap) {
      lastInteraction = now;
      handleSettingsTap(tx, ty);
    }
    wasTouched = t;
    if (forceRedraw && settingsOpen) {
      forceRedraw = false;
      renderSettings();
    }
    delay(5);
    return;
  }

  // ---- stats panel mode (tap anywhere to close) ----
  if (statsOpen) {
    if (tap) {
      statsOpen = false;
      forceRedraw = true;
      lastInteraction = now;
    }
    wasTouched = t;
    delay(5);
    return;
  }

  // ---- WiFi-setup confirmation (Cancel returns to settings) ----
  if (wifiConfirmOpen) {
    if (tap) {
      lastInteraction = now;
      if (inRect(denyBtn, tx, ty)) {
        wifiConfirmOpen = false;
        settingsOpen = true;
        renderSettings();
      } else if (inRect(approveBtn, tx, ty)) {
        net::server.wifiPortal();
        delay(200);
        ESP.restart();
      }
    }
    wasTouched = t;
    delay(5);
    return;
  }

  // ---- long-press (hold) opens Settings ----
  if (t && !longFired && now - pressStart > 900) {
    longFired = true;
    settingsOpen = true;
    lastInteraction = now;
    wasTouched = t;
    renderSettings();
    delay(5);
    return;
  }

  // ---- normal tap: triple-tap anywhere -> dizzy easter egg ----
  if (tap) {
    lastInteraction = now;
    tapTimes[0] = tapTimes[1];
    tapTimes[1] = tapTimes[2];
    tapTimes[2] = now;
    if (tapTimes[0] != 0 && tapTimes[2] - tapTimes[0] < 900) {
      dizzyUntil = now + 2200;
      tapTimes[0] = tapTimes[1] = tapTimes[2] = 0;
    }
  }
  wasTouched = t;

  const char *st = stateName(now);

  // ---- auto screen-off when calm (idle, no transient animation) ----
  bool calm = s.running == 0 && now >= dizzyUntil;
  if (screenOn && calm && now - lastInteraction > SCREEN_OFF_MS) {
    screenOn = false;
    display.backlight(false);
    led.off();
  }
  if (!screenOn) {
    delay(8);
    return;
  }

  // ---- render full card on state/data change ----
  static String last = "?";
  if (s.dirty || forceRedraw || last != st) {
    s.dirty = false;
    forceRedraw = false;
    last = st;
    renderStatic(st);
    if (haveChar)
      render::character.setState(st);
    if (haveChar)
      lastLoops = render::character.loops(); // sync so it ticks on next switch
  }
  if (haveChar)
    render::character.update();

  // ---- rotate the busy verb when the clip switches (same logic as the
  //      animation); repaint ONLY the headline so the stats grid never flickers
  if (!strcmp(st, "busy")) {
    uint32_t lc = haveChar ? render::character.loops() : (now / 2200);
    if (lc != lastLoops) {
      lastLoops = lc;
      verbIdx = (verbIdx + 1) % N_WHIMSY;
      renderHeadline(st);
    }
  }

  static uint32_t ledT = 0;
  if (now - ledT > 100) {
    ledT = now;
    driveLed(st, now);
  }
  delay(2);
}
