#include <Arduino.h>
#include <esp_random.h>
#include "hal/display.h" // pulls in TFT_eSPI (GFX free fonts come with LOAD_GFXFF)
#include "hal/storage.h"
#include "hal/touch.h"
#include "hal/led.h"
#include "render/character.h"
#include "net/server.h"

// Palette (RGB565). Coral = Anthropic accent (#D97757). The character art is
// warmed on-screen by render::tintColor (R x1.06, B x0.85); the raw #D97757
// reads pinker than that, so the UI accent uses the SAME-tinted coral (~#E6774A)
// so headline/number text matches the Clawd you actually see. (raw was 0xDBAA)
#define C_CORAL 0xE3A8
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
// transient hook-driven effect (attention/celebrate/heart), shown for a few sec
static uint32_t fxUntil = 0;
static String fxState;
// power / interaction
static uint32_t lastInteraction = 0;
static uint32_t sessionStart = 0; // millis when the current Claude session began
// activity watchdog: millis of the most recent hook event (any /event POST).
// The running state is hook-driven, so if a turn is interrupted (Esc, a crash, a
// dropped packet) and its closing Stop never arrives, we'd otherwise sit on a
// WORKING clip forever. We fall back to calm once an activity goes silent past
// its plausible runtime. 0 = no event seen yet.
static uint32_t lastEventMs = 0;
static bool screenOn = true, forceRedraw = false, wasTouched = false, haveChar = false;
// triple-tap detection: count taps that arrive in quick succession; any gap
// longer than TAP_GAP_MS restarts the count, so it's a real fast triple-tap.
static uint32_t lastTap = 0;
static int tapCount = 0;
#define TAP_GAP_MS 400

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
static bool askOpen = false;       // on-device "Allow this tool?" prompt
static uint32_t askShownAt = 0;    // for the auto-dismiss timeout
static uint32_t pressStart = 0;
static bool longFired = false;

#define SCREEN_OFF_MS 30000UL
#define PRESLEEP_MS 8000UL // dim the backlight this long before the full cut-off

// ---- enrichment state -------------------------------------------------------
// "Quiet" level the user cycles with a single Settings button (three steps):
//   0 = Off     -> full ambient LED; the screen may auto-wake for nudges/reactions
//   1 = LED off -> silence the RGB LED only; the screen still auto-wakes
//   2 = DND     -> LED off AND the screen never auto-wakes (only a touch does)
// Brightness: the user's "on" backlight level (PWM).
static int quietLevel = 0;
static bool ledSilenced() { return quietLevel >= 1; }    // levels 1 and 2
static bool autoWakeBlocked() { return quietLevel >= 2; } // level 2 only (DND)
static int brightPct = 100;
// Session intensity tier (0 calm / 1 busy / 2 intense), from the hook's rolling
// tool-call burst + active-subagent count. Drives clip speed, tint and LED.
static int intensity = 0;
// "Your turn" waiting nudge: when Claude hands the turn back and idles, the LED
// (and eventually the screen) escalate the longer you don't respond.
static uint32_t waitStart = 0;     // millis the current wait began (0 = none)
static uint32_t lastWaitId = 0;    // edge-detect a fresh wait from the hook
static uint32_t lastNudgeWake = 0; // throttle escalated screen wakes
// Idle micro-behaviour: gently rotate a friendly line while standing by.
static const char *IDLE_MSGS[] = {"Ready when you are", "All caught up",
                                  "Standing by", "At your service",
                                  "Idle \xE2\x80\x94 tap me"};
static const int N_IDLE = sizeof(IDLE_MSGS) / sizeof(IDLE_MSGS[0]);
static int idleIdx = 0;

// Map the hook's burst (tool calls/min) + active subagents to a 0/1/2 tier.
static int intensityTier(int burst, int agents) {
  if (burst >= 8 || agents >= 2) return 2;
  if (burst >= 3 || agents >= 1) return 1;
  return 0;
}

struct Rect {
  int x, y, w, h;
};
static Rect denyBtn, approveBtn;
// Settings: Stats / Quiet / Brightness / Recalibrate / WiFi setup / Close
static Rect setBtns[6];

static void computeButtons() {
  int W = display.tft().width(), H = display.tft().height();
  int bh = 56, m = 8;
  denyBtn = {m, H - bh - 6, (W - 3 * m) / 2, bh};
  approveBtn = {denyBtn.x + denyBtn.w + m, H - bh - 6, (W - 3 * m) / 2, bh};
  int sy = 50, sbh = 36, gap = 8; // 6 rows fit 240x320 (50 + 6*44 = 314)
  for (int i = 0; i < 6; i++)
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
  if (now < fxUntil)
    return fxState.c_str(); // transient hook effect (attention/celebrate/heart)
  if (now < dizzyUntil)
    return "dizzy";
  if (!s.wifiUp)
    return "sleep";
  if (s.running > 0)
    return s.act.length() ? s.act.c_str() : "busy"; // tool-specific clip, else carousel
  if (s.waiting)
    return "attention"; // Claude handed the turn back -> sticky "your turn" nudge
  if (s.total > 0)
    return "idle";
  return "sleep";
}

// the running "work" clips share one look (WORKING / coral / blue LED) and a verb
static bool isWork(const char *st) {
  static const char *W[] = {"busy",      "typing",  "building",  "thinking",
                            "juggling",  "groove",  "carrying",  "debugger",
                            "reading"};
  for (auto w : W)
    if (!strcmp(st, w)) return true;
  return false;
}
// How long an activity may go silent (no new hook event) before we treat it as
// stale/abandoned and drop back to calm. Tuned per clip: tools that legitimately
// run long (Bash builds/tests, subagents, web fetches, compaction) get a roomy
// window; quick edits/reads recover fast. A genuinely long tool that crosses its
// window just shows idle until its PostToolUse lands — a minor cosmetic cost in
// exchange for never being permanently stuck on a WORKING screen.
static uint32_t actTimeout(const char *st) {
  if (!strcmp(st, "juggling")) return 600000UL; // subagents (Task) run longest
  if (!strcmp(st, "building")) return 360000UL; // Bash: builds/installs/tests
  if (!strcmp(st, "thinking")) return 180000UL; // deep reasoning / web fetch
  if (!strcmp(st, "sweeping")) return 180000UL; // context compaction
  if (!strcmp(st, "typing") || !strcmp(st, "reading"))
    return 90000UL; // edits/reads are quick; recover promptly
  return 180000UL;  // generic busy / carousel / unknown
}
static const char *actVerb(const char *st) {
  if (!strcmp(st, "typing")) return "Editing...";
  if (!strcmp(st, "building")) return "Running...";
  if (!strcmp(st, "thinking")) return "Thinking...";
  if (!strcmp(st, "reading")) return "Reading...";
  if (!strcmp(st, "juggling")) return "Delegating...";
  if (!strcmp(st, "groove")) return "Vibing...";
  if (!strcmp(st, "carrying")) return "Moving...";
  if (!strcmp(st, "debugger")) return "Inspecting...";
  return "Working...";
}

// Ambient LED "language": a colour + blink rhythm per state (binary RGB, so a
// breath is a slow blink). blue = working (cooler/quicker as the session heats
// up), amber = your turn (escalates the longer you ignore it), red = error,
// green = done, magenta = play, dark = idle. Silenced when Quiet >= "LED off".
static void driveLed(const char *st, uint32_t now) {
  if (ledSilenced()) {
    led.off();
    return;
  }
  bool r = false, g = false, b = false;
  uint32_t period = 0, onMs = 0; // period 0 -> solid

  if (!strcmp(st, "attention") || !strcmp(st, "notification")) {
    r = g = true; // amber alert
    uint32_t waited = waitStart ? now - waitStart : 0;
    if (waited > 45000) {
      period = 240;
      onMs = 120;
    } // urgent
    else if (waited > 15000) {
      period = 600;
      onMs = 300;
    } // insistent
    else {
      period = 1500;
      onMs = 850;
    } // gentle breath
  } else if (!strcmp(st, "error")) {
    r = true;
    period = 360;
    onMs = 180; // red blink
  } else if (!strcmp(st, "dizzy") || !strcmp(st, "heart")) {
    r = b = true; // magenta
  } else if (!strcmp(st, "celebrate")) {
    g = true; // green
  } else if (isWork(st)) {
    b = true; // blue focus
    if (intensity >= 2) {
      g = true;
      b = true;
      r = false; // cyan, quick
      period = 520;
      onMs = 300;
    } else if (intensity == 1) {
      period = 1100;
      onMs = 650;
    } else {
      period = 1800;
      onMs = 1050; // calm breath
    }
  } else {
    led.off();
    return; // idle / sleep: dark
  }

  bool on = (period == 0) ? true : ((now % period) < onMs);
  if (on)
    led.set(r, g, b);
  else
    led.off();
}

static uint16_t stateColor(const char *st) {
  if (isWork(st)) return C_CORAL;                // orange (working)
  if (!strcmp(st, "dizzy")) return 0xA81F;       // purple
  if (!strcmp(st, "attention") || !strcmp(st, "notification")) return 0xFD20; // amber alert
  if (!strcmp(st, "error")) return 0xC1C5;       // muted red
  if (!strcmp(st, "celebrate")) return 0x2DEA;   // green
  if (!strcmp(st, "heart")) return 0xFB56;        // pink
  if (!strcmp(st, "idle")) return 0x2DEA;        // green (connected, ready)
  return 0x4208;                                 // asleep grey
}
static const char *stateLabel(const char *st) {
  if (isWork(st)) return "WORKING";
  if (!strcmp(st, "dizzy")) return "DIZZY";
  if (!strcmp(st, "attention") || !strcmp(st, "notification")) return "NEEDS YOU";
  if (!strcmp(st, "error")) return "OOPS";
  if (!strcmp(st, "celebrate")) return "DONE!";
  if (!strcmp(st, "heart")) return "HELLO";
  if (!strcmp(st, "idle")) return "READY";
  return "ASLEEP";
}

// Compact token count: 1.2M / 123k / 45 (bare number; the cell label gives units).
static void fmtTok(long long t, char *out, size_t n) {
  if (t >= 1000000) {
    long long x = (t + 50000) / 100000; // tenths of a million, rounded
    snprintf(out, n, "%lld.%lldM", x / 10, x % 10);
  } else if (t >= 1000) {
    long long x = (t + 50) / 100; // tenths of a thousand, rounded
    snprintf(out, n, "%lld.%lldk", x / 10, x % 10);
  } else {
    snprintf(out, n, "%lld", t);
  }
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

// Animated (eased) copies of the live counters, so a value rolls toward its new
// total like an odometer instead of snapping.
static long long dToday = 0, dAll = 0;
static int dTools = 0, dTurns = 0, dSess = 0;
static uint32_t lastDurSec = 0xFFFFFFFF;

static bool tickToward(long long &d, long long target) {
  if (d == target) return false;
  long long step = (target - d) / 4; // ease-out roll
  if (step == 0) step = target > d ? 1 : -1;
  d += step;
  return true;
}
static bool tickTowardI(int &d, int target) {
  if (d == target) return false;
  int step = (target - d) / 4;
  if (step == 0) step = target > d ? 1 : -1;
  d += step;
  return true;
}

// Clear + draw one centered value cell (used for the initial render and the
// per-frame counter animation; cleared so a shrinking value leaves no ghost).
static void drawCell(int cx, int y, const char *v, uint16_t col,
                     const GFXfont *vf, int voff, int maxW) {
  display.tft().fillRect(cx - maxW / 2, y + voff - 1, maxW, 20, C_CARD);
  gtextClamp(v, cx, y + voff, vf, col, C_CARD, TC_DATUM, maxW);
}

// Draw the six value cells from the animated counters (labels are drawn once by
// renderStatic). force=true redraws all (after a full card repaint); otherwise
// only cells whose text changed are redrawn, so a settled value never flickers.
static void drawStatValues(int W, int cy, bool force) {
  static char pT[12], pA[12], pD[14], pNt[8], pNu[8], pNs[8];
  char tok[12], all[12], dur[14], nt[8], nu[8], ns[8];
  fmtTok(dToday, tok, sizeof(tok));
  fmtTok(dAll, all, sizeof(all));
  fmtDur(sessionStart ? (millis() - sessionStart) : 0, dur, sizeof(dur));
  snprintf(nt, sizeof(nt), "%d", dTools);
  snprintf(nu, sizeof(nu), "%d", dTurns);
  snprintf(ns, sizeof(ns), "%d", dSess);
  int yA = cy + 54, yB = cy + 96;
  if (force || strcmp(tok, pT)) { strcpy(pT, tok); drawCell(W / 4, yA, tok, C_CORAL, &FreeSansBold12pt7b, 20, W / 2 - 24); }
  if (force || strcmp(all, pA)) { strcpy(pA, all); drawCell(W * 3 / 4, yA, all, C_CORAL, &FreeSansBold12pt7b, 20, W / 2 - 24); }
  if (force || strcmp(nt, pNt)) { strcpy(pNt, nt); drawCell(W / 8, yB, nt, C_TEXT, &FreeSansBold9pt7b, 15, W / 4 - 8); }
  if (force || strcmp(nu, pNu)) { strcpy(pNu, nu); drawCell(W * 3 / 8, yB, nu, C_TEXT, &FreeSansBold9pt7b, 15, W / 4 - 8); }
  if (force || strcmp(ns, pNs)) { strcpy(pNs, ns); drawCell(W * 5 / 8, yB, ns, C_TEXT, &FreeSansBold9pt7b, 15, W / 4 - 8); }
  if (force || strcmp(dur, pD)) { strcpy(pD, dur); drawCell(W * 7 / 8, yB, dur, C_TEXT, &FreeSansBold9pt7b, 15, W / 4 - 8); }
}

// Session-intensity pips in the top bar (just left of the WiFi dot): 1 dot busy,
// 2 dots intense, none when calm. Cleared in place so it never flashes the bar.
static void renderIntensity() {
  TFT_eSPI &t = display.tft();
  int W = t.width();
  t.fillRect(W - 56, 6, 36, 15, TFT_BLACK);
  for (int i = 0; i < intensity; i++)
    t.fillCircle(W - 30 - i * 11, 13, 3, C_CORAL);
}

// Daily-budget gauge: the card's divider becomes a thin progress bar when a
// budget is configured (coral -> amber near the cap -> red over). Uses the
// animated token count (dToday) so it eases with the odometer.
static void drawBudgetBar(int W, int cy) {
  net::AppState &s = net::server.state();
  TFT_eSPI &t = display.tft();
  int bx = 20, bw = W - 40, by = cy + 38, bh = 4;
  t.fillRoundRect(bx, by, bw, bh, 2, 0x2945); // track
  double frac = s.budget > 0 ? (double)dToday / (double)s.budget : 0;
  if (frac > 1)
    frac = 1;
  int fw = (int)(bw * frac);
  uint16_t col = frac >= 1.0 ? C_NO : (frac > 0.85 ? 0xFD20 : C_CORAL);
  if (fw > 0)
    t.fillRoundRect(bx, by, fw, bh, 2, col);
}

// Card headline text: while busy, the device-rotated whimsy verb (synced to the
// animation); otherwise the hook activity msg, else project, else name.
static const char *headlineText(const char *st) {
  net::AppState &s = net::server.state();
  if (!strcmp(st, "busy"))
    return WHIMSY[verbIdx];
  if (isWork(st)) // tool-specific activity -> its own verb
    return actVerb(st);
  // transient / reaction states get their own word so the headline always
  // matches the animation above it (not a stale activity message)
  if (!strcmp(st, "dizzy")) return "Whoa!";
  if (!strcmp(st, "celebrate")) return "Nice work!";
  if (!strcmp(st, "heart")) return "Hello!";
  if (!strcmp(st, "error")) return "Oops...";
  if (!strcmp(st, "attention") || !strcmp(st, "notification"))
    return "Needs you";
  if (!strcmp(st, "idle")) // standing by -> a gently rotating friendly line
    return IDLE_MSGS[idleIdx];
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
  renderIntensity(); // session-intensity pips

  // bottom stats card (just below the character region; cy = REG_Y+REG_H+4)
  int cy = REG_Y + REG_H + 4;
  int chh = H - cy - 6;
  t.fillRect(0, REG_Y + REG_H, W, H - (REG_Y + REG_H), TFT_BLACK);
  t.fillRoundRect(8, cy, W - 16, chh, 12, C_CARD);

  // headline (verb when busy, else activity/project) -- shared with renderHeadline
  gtextClamp(headlineText(st), W / 2, cy + 18, &FreeSansBold12pt7b, C_TEXT,
             C_CARD, MC_DATUM, W - 32);
  if (s.budget > 0)
    drawBudgetBar(W, cy); // divider doubles as the daily-budget gauge
  else
    t.drawFastHLine(20, cy + 40, W - 40, 0x2945); // plain divider

  // Two prominent token figures up top (each owns half the card), then a row of
  // four compact activity counts. Labels are drawn here once; the values below
  // are animated counters (drawStatValues) that roll toward new totals.
  int yA = cy + 54, yB = cy + 96;
  gtext("Today", W / 4, yA, &FreeSans9pt7b, C_MUTED, C_CARD, TC_DATUM);
  gtext("Total", W * 3 / 4, yA, &FreeSans9pt7b, C_MUTED, C_CARD, TC_DATUM);
  gtext("Tools", W / 8, yB, &FreeSans9pt7b, C_MUTED, C_CARD, TC_DATUM);
  gtext("Turns", W * 3 / 8, yB, &FreeSans9pt7b, C_MUTED, C_CARD, TC_DATUM);
  gtext("Sess", W * 5 / 8, yB, &FreeSans9pt7b, C_MUTED, C_CARD, TC_DATUM);
  gtext("Time", W * 7 / 8, yB, &FreeSans9pt7b, C_MUTED, C_CARD, TC_DATUM);
  drawStatValues(W, cy, true);
}

// full=true draws the whole panel; full=false only clears+redraws the value
// cells (labels stay) so the panel can live-update without a full-screen flash.
static void renderStats(bool full) {
  TFT_eSPI &t = display.tft();
  net::AppState &s = net::server.state();
  int W = t.width();
  if (full) {
    t.fillScreen(TFT_BLACK);
    gtext("Stats", W / 2, 16, &FreeSansBold18pt7b, C_CORAL, TFT_BLACK, TC_DATUM);
    gtext("tap to close", W / 2, 310, &FreeSans9pt7b, C_MUTED, TFT_BLACK,
          BC_DATUM);
  }
  // label left, value right-aligned to the screen edge and clamped so long
  // values (IP / project / big token counts) can't run off the right side.
  int lx = 18, rx = W - 18, y = 48, dy = 22, vMax = W - 18 - 108;
  char b[24];
  auto row = [&](const char *k, const String &v) {
    t.fillRect(104, y - 13, W - 104 - 6, 20, TFT_BLACK); // clear value cell only
    if (full)
      gtext(k, lx, y, &FreeSans9pt7b, C_MUTED, TFT_BLACK, TL_DATUM);
    gtextClamp(v.c_str(), rx, y, &FreeSansBold9pt7b, C_TEXT, TFT_BLACK,
               TR_DATUM, vMax);
    y += dy;
  };
  fmtTok(s.tokens, b, sizeof(b));
  row("Today tok", String(b));
  fmtTok(s.tokensAll, b, sizeof(b));
  row("All-time tok", String(b));
  // Rough $ estimate (blended rate; the device only sees totals, so it's a
  // ballpark, hence the "~"). Tune COST_PER_MTOK to your usual model mix.
  const double COST_PER_MTOK = 6.0;
  snprintf(b, sizeof(b), "~$%.2f", (double)s.tokens / 1e6 * COST_PER_MTOK);
  row("Cost today", String(b));
  snprintf(b, sizeof(b), "~$%.2f", (double)s.tokensAll / 1e6 * COST_PER_MTOK);
  row("Cost all", String(b));
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
}

static void renderSettings() {
  TFT_eSPI &t = display.tft();
  int W = t.width();
  t.fillScreen(TFT_BLACK);
  gtext("Settings", W / 2, 16, &FreeSansBold18pt7b, C_CORAL, TFT_BLACK,
        TC_DATUM);
  char quiet[20], bri[20];
  const char *qn = quietLevel == 0 ? "Off" : (quietLevel == 1 ? "LED off" : "DND");
  snprintf(quiet, sizeof(quiet), "Quiet: %s", qn);
  snprintf(bri, sizeof(bri), "Brightness: %d%%", brightPct);
  const char *labels[6] = {"Stats", quiet,        bri,
                           "Recalibrate", "WiFi setup", "Close"};
  for (int i = 0; i < 6; i++)
    drawButton(setBtns[i], labels[i], (i == 1 && quietLevel > 0) ? 0x7B40 : C_FACE);
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

// "Allow this tool?" — shown when a synchronous PermissionRequest hook is waiting
// on the device. A tap approves/denies just this one pending tool call.
static void renderAsk() {
  TFT_eSPI &t = display.tft();
  net::AppState &s = net::server.state();
  int W = t.width();
  t.fillScreen(TFT_BLACK);
  gtext("Allow?", W / 2, 40, &FreeSansBold18pt7b, C_CORAL, TFT_BLACK, TC_DATUM);
  gtext("Claude wants to run", W / 2, 92, &FreeSans9pt7b, C_MUTED, TFT_BLACK,
        TC_DATUM);
  gtextClamp(s.askTool.c_str(), W / 2, 122, &FreeSansBold18pt7b, C_TEXT,
             TFT_BLACK, MC_DATUM, W - 24);
  drawButton(denyBtn, "Deny", C_NO);
  drawButton(approveBtn, "Allow", C_OK);
}

static void handleSettingsTap(int x, int y) {
  for (int i = 0; i < 6; i++) {
    if (!inRect(setBtns[i], x, y))
      continue;
    if (i == 0) { // open the stats panel
      settingsOpen = false;
      statsOpen = true;
      renderStats(true);
    } else if (i == 1) { // cycle Quiet: Off -> LED off -> DND (persisted)
      quietLevel = (quietLevel + 1) % 3;
      storage.putInt("quiet", quietLevel);
      if (ledSilenced())
        led.off(); // apply the LED silence immediately
      renderSettings();
    } else if (i == 2) { // cycle backlight brightness 100 -> 70 -> 40 (persisted)
      brightPct = brightPct > 70 ? 70 : (brightPct > 40 ? 40 : 100);
      display.setBrightness(brightPct);
      display.backlight(true); // apply immediately
      storage.putInt("bright", brightPct);
      renderSettings();
    } else if (i == 3) { // recalibrate touch (shows visible targets)
      settingsOpen = false;
      touch.calibrate(display);
      forceRedraw = true;
    } else if (i == 4) { // WiFi setup -> confirm first (avoids accidental taps)
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

  // restore persisted prefs (Quiet level + backlight brightness); migrate the
  // old boolean "dnd" key (on -> DND) for anyone upgrading.
  quietLevel = storage.getInt("quiet", storage.getInt("dnd", 0) ? 2 : 0);
  brightPct = storage.getInt("bright", 100);
  display.setBrightness(brightPct);
  display.backlight(true);

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

  // ---- transient hook effect (attention/celebrate/heart): edge-trigger once
  //      per event; wakes the screen so a "needs you" / "done" isn't missed ----
  static uint32_t lastFxId = 0;
  if (s.fxId != lastFxId) {
    lastFxId = s.fxId;
    fxState = s.fx;
    fxUntil = now + (s.fx == "attention" ? 5000UL : 3000UL);
    forceRedraw = true;
    if (!screenOn && !autoWakeBlocked()) { // DND: react silently, don't wake
      screenOn = true;
      display.backlight(true);
      lastInteraction = now;
      wasTouched = true; // this wake isn't a tap
      pressStart = now;
      longFired = true;  // don't let a coincident touch fire a long-press
    }
  }

  // ---- on-device approval: a synchronous hook is asking; pop the Allow/Deny
  //      prompt and wake the screen (the hook is blocking until we tap/timeout)
  static uint32_t lastAskId = 0;
  if (s.askId != lastAskId) {
    lastAskId = s.askId;
    askOpen = true;
    askShownAt = now;
    settingsOpen = statsOpen = wifiConfirmOpen = false;
    if (!screenOn) {
      screenOn = true;
      display.backlight(true);
    }
    lastInteraction = now;
    wasTouched = true;
    pressStart = now;
    longFired = true;
    renderAsk();
  }

  // ---- "your turn" waiting nudge: stamp when a fresh wait began (waitId edge),
  //      so the LED and (past a threshold) the screen escalate the longer the
  //      turn sits with you. Cleared the moment Claude resumes. ----
  if (s.waitId != lastWaitId) {
    lastWaitId = s.waitId;
    waitStart = now;
  }
  if (!s.waiting)
    waitStart = 0;

  // ---- activity watchdog: stamp the time of every hook event (actSeq bumps on
  //      each /event). If we're "running" but the current activity has gone quiet
  //      longer than it could plausibly still be working, the closing event never
  //      came (interrupted turn / crash / lost packet) -> abandon it so we fall
  //      back to calm. The next real event re-arms running cleanly. ----
  static uint32_t lastEventSeq = 0;
  if (s.actSeq != lastEventSeq) {
    lastEventSeq = s.actSeq;
    lastEventMs = now;
  }
  if (s.running > 0 && lastEventMs &&
      now - lastEventMs > actTimeout(s.act.length() ? s.act.c_str() : "busy")) {
    s.running = 0; // stale activity: stop pretending Claude is still working
    s.act = "";
    s.dirty = true;
  }

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

  // ---- asleep: a touch, Claude starting work, or an escalated "your turn"
  //      nudge wakes us (once per wait, unless DND); else stay dark ----
  if (!screenOn) {
    bool nudgeWake = s.waiting && !autoWakeBlocked() && waitStart &&
                     (now - waitStart > 45000) && (lastNudgeWake < waitStart);
    if (touch.rawPressed() || s.running > 0 || nudgeWake) {
      if (nudgeWake)
        lastNudgeWake = now; // fire the screen-wake just once per wait episode
      screenOn = true;
      display.backlight(true);
      lastInteraction = now;
      forceRedraw = true;
      wasTouched = true; // consume this contact, don't fire a tap
      pressStart = now;  // reset the hold timer so the held wake-touch isn't
      longFired = true;  // mistaken for a long-press (was popping open Settings)
    } else {
      // stay dark, but keep the RGB nudge alive so a "your turn" still pulses
      // with the screen off (driveLed self-silences for idle/sleep and DND).
      static uint32_t ledOffT = 0;
      if (now - ledOffT > 100) {
        ledOffT = now;
        driveLed(stateName(now), now);
      }
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

  // ---- on-device tool approval: tap Allow/Deny for the one pending call;
  //      no tap in time -> the blocking hook times out -> normal prompt ----
  if (askOpen) {
    if (tap && inRect(approveBtn, tx, ty)) {
      s.decision = "allow";
      s.decidedId = s.askId;
      askOpen = false;
      forceRedraw = true;
      lastInteraction = now;
    } else if (tap && inRect(denyBtn, tx, ty)) {
      s.decision = "deny";
      s.decidedId = s.askId;
      askOpen = false;
      forceRedraw = true;
      lastInteraction = now;
    } else if (now - askShownAt > 24000) {
      // close just BEFORE the hook stops polling (~26s) so a late tap can't land
      // on a prompt the hook has already abandoned; it then falls back cleanly.
      askOpen = false;
      forceRedraw = true;
    }
    wasTouched = t;
    delay(5);
    return;
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

  // ---- stats panel mode (tap anywhere to close; live-refresh values ~1s) ----
  if (statsOpen) {
    if (tap) {
      statsOpen = false;
      forceRedraw = true;
      lastInteraction = now;
    } else {
      static uint32_t lastStatsRefresh = 0;
      if (now - lastStatsRefresh > 1000) {
        lastStatsRefresh = now;
        renderStats(false); // redraw only the value cells (session timer, heap…)
      }
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

  // ---- normal tap: a real triple-tap (3 quick taps) -> dizzy easter egg ----
  if (tap) {
    lastInteraction = now;
    tapCount = (now - lastTap < TAP_GAP_MS) ? tapCount + 1 : 1;
    lastTap = now;
    if (tapCount >= 3) {
      dizzyUntil = now + 2200;
      tapCount = 0;
    }
  }
  wasTouched = t;

  const char *st = stateName(now);

  // ---- auto screen-off when calm; PWM-dim for a few seconds first ----
  bool calm = s.running == 0 && now >= dizzyUntil && now >= fxUntil;
  uint32_t idleFor = now - lastInteraction;
  static bool dimmed = false;
  if (screenOn && calm && idleFor > SCREEN_OFF_MS) {
    screenOn = false;
    dimmed = false;
    display.backlight(false);
    led.off();
  } else if (screenOn) {
    // pre-sleep fade: ease the backlight down in the last PRESLEEP_MS so the
    // cut-off isn't an abrupt blackout (also a subtle "about to sleep" cue).
    bool wantDim = calm && idleFor > (SCREEN_OFF_MS - PRESLEEP_MS);
    if (wantDim && !dimmed) {
      dimmed = true;
      display.backlightLevel(20);
    } else if (!wantDim && dimmed) {
      dimmed = false;
      display.backlight(true);
    }
  }
  if (!screenOn) {
    delay(8);
    return;
  }

  // ---- render: FULL card only on a real state change / forced redraw (a full
  //      fillRect repaint flashes, so we never do it for routine data updates).
  //      A data-only change (new event, same state) just refreshes the headline
  //      in place; the stat cells + budget bar roll on their own below. ----
  static String last = "?";
  if (forceRedraw || last != st) {
    forceRedraw = false;
    s.dirty = false;
    last = st;
    renderStatic(st);
    if (haveChar) {
      render::character.setState(st);
      lastLoops = render::character.loops(); // sync so it ticks on next switch
    }
  } else if (s.dirty) {
    s.dirty = false;
    renderHeadline(st); // activity/project text changed -> in-place, no flash
  }
  if (haveChar)
    render::character.update();

  // ---- session-intensity tier -> clip speed/tint + top-bar pips ----
  int tier = isWork(st) ? intensityTier(s.burst, s.agents) : 0;
  if (tier != intensity) {
    intensity = tier;
    renderIntensity();
    if (haveChar) {
      render::character.setSpeed(tier >= 2 ? 122 : (tier == 1 ? 110 : 100));
      render::character.setTint(tier >= 2 ? 2 : 1);
    }
  }

  // ---- bind clip switching to Claude Code: each new tool event nudges the
  //      running animation to another clip (throttled so a burst of fast calls
  //      doesn't strobe). Long single calls still rotate on the dwell timer. ----
  static uint32_t lastActSeq = 0;
  static uint32_t lastEvSwitch = 0;
  if (s.actSeq != lastActSeq) {
    lastActSeq = s.actSeq;
    if (haveChar && isWork(st) && now - lastEvSwitch > 2500) {
      lastEvSwitch = now;
      render::character.nextClip();
      lastLoops = render::character.loops();
    }
  }

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

  // ---- idle micro-behaviour: gently rotate the standing-by line ----
  if (!strcmp(st, "idle")) {
    static uint32_t lastIdleRot = 0;
    if (now - lastIdleRot > 9000) {
      lastIdleRot = now;
      idleIdx = (idleIdx + 1) % N_IDLE;
      renderHeadline(st);
    }
  }

  static uint32_t ledT = 0;
  if (now - ledT > 100) {
    ledT = now;
    driveLed(st, now);
  }

  // ---- roll the stat counters toward their live targets (odometer feel);
  //      also re-tick the session timer once a second ----
  static uint32_t lastRoll = 0;
  if (now - lastRoll > 40) {
    lastRoll = now;
    bool ch = false;
    ch |= tickToward(dToday, s.tokens);
    ch |= tickToward(dAll, s.tokensAll);
    ch |= tickTowardI(dTools, s.tools);
    ch |= tickTowardI(dTurns, s.turns);
    ch |= tickTowardI(dSess, s.sessions);
    uint32_t durSec = sessionStart ? (now - sessionStart) / 1000 : 0;
    if (durSec != lastDurSec) {
      lastDurSec = durSec;
      ch = true;
    }
    if (ch) {
      int W = display.tft().width();
      drawStatValues(W, REG_Y + REG_H + 4, false);
      if (s.budget > 0)
        drawBudgetBar(W, REG_Y + REG_H + 4); // ease the gauge with the counter
    }
  }
  delay(2);
}
