#pragma once
#include <Arduino.h>

namespace net {

// Shared app state, written ONLY by Ble::loop() (which drains the BLE ingress
// queue on the Arduino loop task) and read by the renderer. Single-threaded,
// no locking — NimBLE callbacks never touch this struct, they only enqueue.
struct AppState {
  bool linkUp = false;     // a bridge (BLE central) is connected
  String token; // shared secret; envelopes must carry it as "tok"
  // latest session snapshot from the hook
  int total = 0, running = 0;
  String msg;
  String project;          // current project (cwd basename)
  String date;             // PC-local "YYYY-MM-DD" from the hook; keys the
                           // on-device usage-history ring (device has no clock)
  long tokens = 0;         // tokens used today
  long long tokensAll = 0; // tokens used all-time (64-bit: won't wrap past ~2.1B)
  int tools = 0;           // tool calls today
  int turns = 0;           // assistant turns today
  int sessions = 0;        // sessions today
  // sticky per-activity clip while running (typing/building/thinking/juggling…)
  // chosen by the hook from the live tool; empty -> the random busy carousel.
  String act;
  // bumps on every hook event, so the renderer can switch the running clip in
  // lock-step with Claude's actions (not just a free-running timer).
  uint32_t actSeq = 0;
  // transient hook-driven effect (attention/celebrate/heart/error/notification);
  // fxId bumps once per effect event so the renderer edge-triggers a short anim.
  String fx;
  uint32_t fxId = 0;
  // Claude is waiting on the user (turn done / a notification with no follow-up).
  // Sticky until Claude resumes; the renderer escalates a nudge the longer it's
  // set. waitId bumps each time a fresh wait begins so the nudge timer restarts.
  bool waiting = false;
  uint32_t waitId = 0;
  // session intensity inputs from the hook: tool calls in the last ~minute and
  // the number of active subagents. Drive the calm/busy/intense tier.
  int burst = 0;
  int agents = 0;
  // optional daily token budget (from buddy.json); 0 = unset -> no budget gauge.
  long budget = 0;
  // on-device approval of a pending tool call (the PermissionRequest hook sends
  // an "ask" envelope, then polls the bridge, which relays our decision notify).
  String askTool;          // tool awaiting a tap; "" = none pending
  uint32_t askId = 0;      // bumps on each ask
  String decision;         // "allow"/"deny" once the user taps; "" = undecided
  uint32_t decidedId = 0;  // the askId the decision belongs to
  // host timestamp (ms) of the last APPLIED event. Async hooks can deliver out
  // of order, so we drop any event older than this -> a late PostToolUse can't
  // re-assert "running" after the Stop that already ended the turn.
  long long lastTs = 0;
  bool dirty = true;       // renderer should repaint
};

// BLE GATT server (NimBLE, peripheral-only). The PC bridge writes JSON
// envelopes {"k":"event"|"ask","tok":"...","d":{...}} to the ingress
// characteristic; decisions flow back on the decision characteristic as
// {"askId":N,"decision":"allow"|"deny"} (notify, with read as poll fallback).
class Ble {
public:
  void setToken(const String &t); // call before begin()
  void begin();                   // init NimBLE, start advertising
  void loop();                    // drain ingress queue, apply state, push decisions
  AppState &state();
};

extern Ble ble;

} // namespace net
