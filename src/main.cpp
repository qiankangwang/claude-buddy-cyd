#include <Arduino.h>
#include "hal/display.h" // pulls in TFT_eSPI (GFX free fonts come with LOAD_GFXFF)
#include "hal/storage.h"
#include "hal/touch.h"
#include "hal/led.h"
#include "render/character.h"
#include "net/server.h"
#include "app/ctx.h"
#include "app/activity.h"
#include "app/led_language.h"
#include "app/store.h"
#include "app/power.h"
#include "ui/theme.h"
#include "ui/text.h"
#include "ui/widgets.h"
#include "screens/layout.h"
#include "screens/settings.h"
#include "screens/wifi_confirm.h"
#include "screens/ask.h"
#include "screens/stats_panel.h"

using namespace app;
using namespace ui;
using namespace screens;

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
#define TAP_GAP_MS 550 // window for counting a fast triple-tap (was 400)
#define LONG_PRESS_MS 650 // hold time to open Settings (was 900 -> snappier)
// Resistive panels briefly drop below the pressure floor mid-press; without a
// grace that reads as a release, so a held finger looks like a burst of fresh
// taps and the long-press timer never accumulates. Treat the panel as still
// touched for this long after the last solid contact (debounced release).
#define TOUCH_RELEASE_MS 80

// rotating "busy" verb index, advanced in sync with the character animation loop
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
// "Quiet" (Do Not Disturb): a single on/off Settings toggle. When on, the RGB
// LED is silenced and the screen never auto-wakes for a nudge/reaction (only a
// touch wakes it). Off = normal. dnd / brightness / intensity live in app::ctx
// (the settings screen renders them; the loop below writes them).
static bool ledSilenced() { return ctx.dnd; }
static bool autoWakeBlocked() { return ctx.dnd; }
// "Your turn" waiting nudge: when Claude hands the turn back and idles, the LED
// (and eventually the screen) escalate the longer you don't respond.
static uint32_t waitStart = 0;     // millis the current wait began (0 = none)
static uint32_t lastWaitId = 0;    // edge-detect a fresh wait from the hook
static uint32_t lastNudgeWake = 0; // throttle escalated screen wakes
// "Got it": the user can tap to dismiss the current "your turn" episode without
// going back to Claude. The device drops straight back to idle (LED off, calm
// home screen) and stops nudging. Bound to the wait episode: a fresh wait (new
// waitId) re-arms the full escalation.
static bool waitAcked = false;
static int idleIdx = 0; // idle micro-behaviour: rotating stand-by line

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
  if (s.waiting && !waitAcked)
    return "attention"; // Claude handed the turn back -> sticky "your turn" nudge
  if (s.total > 0)
    return "idle"; // (acknowledged waits land here too: dark LED, calm home)
  return "sleep";
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

// Draw one centered value cell. Rendered via an off-screen sprite (blitText) so a
// rolling counter updates without flashing its cleared background each frame.
static void drawCell(int cx, int y, const char *v, uint16_t col,
                     const GFXfont *vf, int voff, int maxW) {
  blitText(cx - maxW / 2, y + voff - 1, maxW, 20, v, cx, y + voff, vf, col,
           C_CARD, TC_DATUM, maxW);
}

// Draw the six value cells from the animated counters (labels are drawn once by
// renderStatic). force=true redraws all (after a full card repaint); otherwise
// only cells whose text changed are redrawn, so a settled value never flickers.
static void drawStatValues(int W, int cy, bool force) {
  static char pT[12], pA[12], pD[14], pNt[8], pNu[8], pNs[8];
  char tok[12], all[12], dur[14], nt[8], nu[8], ns[8];
  fmtTok(dToday, tok, sizeof(tok));
  fmtTok(dAll, all, sizeof(all));
  fmtDur(ctx.sessionStart ? (millis() - ctx.sessionStart) : 0, dur, sizeof(dur));
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
  for (int i = 0; i < ctx.intensity; i++)
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
  int W = display.tft().width(), cy = REG_Y + REG_H + 4;
  // sprite-blit the headline band so the rotating verb/idle line never flashes.
  blitText(12, cy + 5, W - 24, 28, headlineText(st), W / 2, cy + 18,
           &FreeSansBold12pt7b, C_TEXT, C_CARD, MC_DATUM, W - 32);
}

// Update ONLY the top-bar status dot + label in place (label via sprite so it
// doesn't flash). Used on a state change that keeps the card layout, so we skip
// the full renderStatic() repaint that briefly blanks the whole card. The label
// band stops short of the intensity pips / WiFi dot, which update on their own.
static void renderStatusBar(const char *st) {
  TFT_eSPI &t = display.tft();
  net::AppState &s = net::server.state();
  int W = t.width();
  t.fillCircle(13, 13, 5, stateColor(st));
  blitText(26, 2, (W - 60) - 26, 24, stateLabel(st), 26, 14, &FreeSansBold9pt7b,
           C_TEXT, TFT_BLACK, ML_DATUM, (W - 60) - 26);
  t.fillCircle(W - 13, 13, 4, s.wifiUp ? 0x2DEA : 0x9000);
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
  t.fillRect(0, REG_Y + REG_H, W, H - (REG_Y + REG_H), TFT_BLACK);
  // On the needs-you screen, drop the stats card -> a clean alert: just the amber
  // Clawd and a "Got it" button to dismiss. Drawn ONCE here (the lower area is
  // static -- the animation stays in the region above it -- so no per-frame
  // repaint is needed, which is what used to make the button flicker).
  if (!strcmp(st, "attention") || !strcmp(st, "notification")) {
    // coral fill so it sits in the warm needs-you palette (the brand accent)
    // rather than the out-of-place Allow-green it used before
    drawButton(ackBtn, "Got it", C_CORAL);
    return;
  }
  int chh = H - cy - 6;
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

static void handleSettingsTap(int x, int y) {
  for (int i = 0; i < 7; i++) {
    if (!inRect(setBtns[i], x, y))
      continue;
    if (i == 0) { // open the stats panel
      settingsOpen = false;
      statsOpen = true;
      renderStats(true);
    } else if (i == 1) { // toggle Quiet / Do Not Disturb (persisted)
      ctx.dnd = !ctx.dnd;
      storage.putInt("dnd", ctx.dnd ? 1 : 0);
      if (ctx.dnd)
        led.off(); // apply the LED silence immediately
      renderSettings();
    } else if (i == 2) { // cycle backlight brightness 100 -> 70 -> 40 (persisted)
      ctx.brightPct = ctx.brightPct > 70 ? 70 : (ctx.brightPct > 40 ? 40 : 100);
      display.setBrightness(ctx.brightPct);
      display.backlight(true); // apply immediately
      storage.putInt("bright", ctx.brightPct);
      renderSettings();
    } else if (i == 3) { // recalibrate touch (shows visible targets)
      settingsOpen = false;
      touch.calibrate(display);
      forceRedraw = true;
    } else if (i == 4) { // WiFi setup -> confirm first (avoids accidental taps)
      settingsOpen = false;
      wifiConfirmOpen = true;
      renderWifiConfirm();
    } else if (i == 5) { // power off -> deep sleep (tap screen / RST to wake)
      powerOff(display, touch, led, storage); // does not return
    } else { // close (i == 6)
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
  ui::begin(display);
  storage.begin();
  touch.begin(display, storage);
  led.begin();
  computeButtons(display.tft());

  // restore persisted prefs (Quiet/DND + backlight brightness); migrate the old
  // 3-level "quiet" key (>=1 -> on) for anyone upgrading.
  ctx.dnd = storage.getInt("dnd", storage.getInt("quiet", 0) >= 1 ? 1 : 0) != 0;
  ctx.brightPct = storage.getInt("bright", 100);
  display.setBrightness(ctx.brightPct);
  display.backlight(true);

  net::server.setToken(loadOrCreateToken(storage));

  // restore the last stats snapshot so a replug shows the previous numbers
  // immediately (the next hook event re-asserts the authoritative totals).
  restoreStats(storage);
  // seed the odometer counters from the restored values so they read true at
  // once instead of rolling up from zero on the first frame after WiFi connects.
  {
    net::AppState &s = net::server.state();
    dToday = s.tokens;
    dAll = s.tokensAll;
    dTools = s.tools;
    dTurns = s.turns;
    dSess = s.sessions;
  }

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

  // Mirror the latest counters to NVS (throttled + only-on-change) so an unplug
  // restores them on the next boot. Runs before any early return below so it
  // still ticks while the screen is asleep or a menu/panel is open.
  saveStatsIfChanged(storage, false);

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
      setCpuFrequencyMhz(240); // back to full speed on wake
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
      setCpuFrequencyMhz(240); // back to full speed on wake
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
    waitAcked = false; // a fresh "your turn" re-arms the full nudge
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
    ctx.sessionStart = now;
  else if (s.total == 0)
    ctx.sessionStart = 0;
  prevTotal = s.total;

  // While asleep, skip the full (filtered) touch read -- the wake check below
  // uses the lighter rawPressed(), so this drops a second SPI touch transaction
  // from every idle loop.
  int16_t tx = 0, ty = 0;
  bool tRaw = screenOn ? touch.read(tx, ty) : false;
  // Release debounce: ride through a brief mid-press pressure dropout (reusing the
  // last solid coords) so a hold stays ONE continuous contact -- fixes long-press
  // not registering and stops a single tap splitting into several.
  static uint32_t lastContactMs = 0;
  static int16_t heldX = 0, heldY = 0;
  if (tRaw) { lastContactMs = now; heldX = tx; heldY = ty; }
  bool t = tRaw;
  if (screenOn && !tRaw && lastContactMs && now - lastContactMs < TOUCH_RELEASE_MS) {
    t = true;
    tx = heldX;
    ty = heldY;
  }

  // ---- asleep: a touch, Claude starting work, or an escalated "your turn"
  //      nudge wakes us (once per wait, unless DND); else stay dark ----
  if (!screenOn) {
    bool nudgeWake = s.waiting && !autoWakeBlocked() && !waitAcked && waitStart &&
                     (now - waitStart > 45000) && (lastNudgeWake < waitStart);
    if (touch.rawPressed() || s.running > 0 || nudgeWake) {
      if (nudgeWake)
        lastNudgeWake = now; // fire the screen-wake just once per wait episode
      screenOn = true;
      setCpuFrequencyMhz(240);     // back to full speed on wake
      net::server.nudgeReconnect(); // if we dozed offline, start reconnecting now
      display.backlight(true);
      lastInteraction = now;
      forceRedraw = true;
      wasTouched = true; // consume this contact, don't fire a tap
      pressStart = now;  // reset the hold timer so the held wake-touch isn't
      longFired = true;  // mistaken for a long-press (was popping open Settings)
    } else {
      led.off(); // screen is dark -> keep the LED off too (no RGB while asleep)
      wasTouched = false;
      // idle throttle: ~25 Hz is plenty to catch a tap or an incoming event,
      // and runs the (otherwise idle) loop body 4x less often than before
      delay(40);
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
  if (t && !longFired && now - pressStart > LONG_PRESS_MS) {
    longFired = true;
    settingsOpen = true;
    lastInteraction = now;
    wasTouched = t;
    renderSettings();
    delay(5);
    return;
  }

  // ---- "Got it": tap the pill on the needs-you screen to acknowledge this
  //      episode (calm the LED, stop further nudges) without replying to Claude
  if (tap && !waitAcked && inRect(ackBtn, tx, ty)) {
    const char *tst = stateName(now);
    if (!strcmp(tst, "attention") || !strcmp(tst, "notification")) {
      waitAcked = true;
      lastInteraction = now;
      forceRedraw = true; // repaint: drop the pill, settle to the calm indicator
      wasTouched = t;
      delay(5);
      return; // consume the tap (don't also count it toward the dizzy triple-tap)
    }
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
    saveStatsIfChanged(storage, true); // checkpoint before going dark (likely unplug point)
    display.backlight(false);
    led.off();
    setCpuFrequencyMhz(80); // still tracking over WiFi, but at the WiFi-safe
                            // minimum clock -> lower idle draw with screen off
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
  // Repaint policy: a FULL renderStatic() blanks the top bar + whole card, which
  // flashes -- acceptable when the layout actually changes (to/from the cardless
  // needs-you screen) or on a forced redraw (wake, closing a panel), but NOT for a
  // routine state change like idle<->working. Those keep the same card, so update
  // only what differs (status dot+label, headline) in place via sprites: no blank
  // frame, no flash.
  static String last = "?";
  auto cardLayout = [](const char *s) {
    return strcmp(s, "attention") && strcmp(s, "notification");
  };
  bool firstDraw = (last == "?");
  bool layoutChanged = firstDraw || (cardLayout(last.c_str()) != cardLayout(st));
  if (forceRedraw || layoutChanged) {
    forceRedraw = false;
    s.dirty = false;
    renderStatic(st); // genuine layout change / forced -> full repaint
  } else if (last != st) {
    s.dirty = false; // same card layout, just a state change -> in-place update
    renderStatusBar(st);
    renderHeadline(st);
  } else if (s.dirty) {
    s.dirty = false;
    if (cardLayout(st))
      renderHeadline(st); // data-only change -> headline in place, no flash
  }
  if (last != st && haveChar) { // sync the mascot clip on any state change
    render::character.setState(st);
    lastLoops = render::character.loops(); // sync so it ticks on next switch
  }
  last = st;
  if (haveChar)
    render::character.update();

  // ---- session-intensity tier -> clip speed/tint + top-bar pips ----
  int tier = isWork(st) ? intensityTier(s.burst, s.agents) : 0;
  if (tier != ctx.intensity) {
    ctx.intensity = tier;
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
    driveLed(led, st, now, ctx.intensity, waitStart, ledSilenced());
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
    uint32_t durSec = ctx.sessionStart ? (now - ctx.sessionStart) / 1000 : 0;
    if (durSec != lastDurSec) {
      lastDurSec = durSec;
      ch = true;
    }
    // no card on the needs-you screen -> don't paint stat cells into the void
    if (ch && strcmp(st, "attention") && strcmp(st, "notification")) {
      int W = display.tft().width();
      drawStatValues(W, REG_Y + REG_H + 4, false);
      if (s.budget > 0)
        drawBudgetBar(W, REG_Y + REG_H + 4); // ease the gauge with the counter
    }
  }
  delay(2);
}
