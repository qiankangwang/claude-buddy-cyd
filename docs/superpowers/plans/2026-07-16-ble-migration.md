# BLE Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the WiFi transport (WiFiManager + WebServer + ArduinoOTA) with BLE: a NimBLE GATT server on the device and an on-demand `buddy_bridge.py` on the PC, per `docs/superpowers/specs/2026-07-16-ble-migration-design.md`.

**Architecture:** Hooks keep POSTing the same HTTP surface, but to `127.0.0.1:8787` where a self-terminating bridge relays over BLE. The device swaps its entire WiFi stack for a two-characteristic GATT service; `AppState` and all rendering stay untouched.

**Tech Stack:** ESP32 Arduino core 2.x (espressif32@^6.5.0 — do NOT move to 3.x), NimBLE-Arduino ^1.4, ArduinoJson 7, Python 3.13 + `bleak` (PC).

## Global Constraints

- Public repo: no real tokens, IPs, or `C:/Users/<name>` paths in any committed file — placeholders only.
- Commits: conventional style (`feat:`/`refactor:`/`docs:`), **no Co-Authored-By trailer**.
- Code and comments in English.
- Build command: `python -m platformio run -e cyd` (pio is not on PATH). Expect "No core dump partition found" at boot — benign, do not fix.
- Frozen GATT UUIDs (from the spec): service `177b0001-6f32-4ea3-b878-866e7628de1f`, ingress `177b0002-…de1f`, decision `177b0003-…de1f`. BLE device name: `claude-cyd`. Bridge port default: `8787`.
- Edit files only with proper file tools — never PowerShell `Get-Content`/`Set-Content` one-liners (they mangle encodings; this repo has been burned twice).
- `curl` to localhost/device MUST use `--noproxy "*"` (the system proxy fakes 502s).
- Hooks in `~/.claude/settings.json` point at the repo's `tools/buddy_hook.py` **live** — hook edits take effect on the next event. Write hook file changes in a single atomic Write.

---

### Task 1: Bridge core logic + unit tests

**Files:**
- Create: `tools/buddy_bridge.py` (logic classes only; HTTP/BLE come in Tasks 2–3)
- Test: `tools/test_buddy_bridge.py`

**Interfaces:**
- Produces: `make_envelope(kind: str, token: str, body: dict) -> bytes`; class `LatestSlot` (`.put(item)`, `.take() -> item|None`, `.attach(loop)`, `.event: asyncio.Event|None`); class `DecisionStore` (`.clear()`, `.set_from_notify(data: bytes)`, `.get() -> str`); class `Link` (`.slot`, `.decisions`, `.connected: bool`, `.loop`, `.worker`, `.last_request: float`, `.touch()`); constants `SERVICE_UUID/INGRESS_UUID/DECISION_UUID/DEVICE_NAME/IDLE_EXIT_S/SCAN_WINDOW_S/DORMANT_SCAN_S/DORMANT_GAP_S`.

- [ ] **Step 1: Write the failing tests**

```python
# tools/test_buddy_bridge.py
"""Unit tests for the bridge's pure logic (no BLE, no HTTP, no device).
Run: cd tools && python -m unittest test_buddy_bridge -v"""
import json
import unittest

import buddy_bridge as bb


class TestEnvelope(unittest.TestCase):
    def test_event_envelope_roundtrip(self):
        raw = bb.make_envelope("event", "sekrit", {"running": 1, "msg": "hi"})
        self.assertIsInstance(raw, bytes)
        o = json.loads(raw.decode("utf-8"))
        self.assertEqual(o, {"k": "event", "tok": "sekrit",
                             "d": {"running": 1, "msg": "hi"}})

    def test_ask_envelope_kind(self):
        o = json.loads(bb.make_envelope("ask", "t", {"tool": "Bash"}))
        self.assertEqual(o["k"], "ask")
        self.assertEqual(o["d"], {"tool": "Bash"})


class TestLatestSlot(unittest.TestCase):
    def test_take_empty_is_none(self):
        self.assertIsNone(bb.LatestSlot().take())

    def test_latest_wins(self):
        s = bb.LatestSlot()
        s.put(b"old")
        s.put(b"new")
        self.assertEqual(s.take(), b"new")

    def test_take_clears(self):
        s = bb.LatestSlot()
        s.put(b"x")
        s.take()
        self.assertIsNone(s.take())


class TestDecisionStore(unittest.TestCase):
    def test_starts_empty(self):
        self.assertEqual(bb.DecisionStore().get(), "")

    def test_set_from_notify_and_clear(self):
        d = bb.DecisionStore()
        d.set_from_notify(b'{"askId":3,"decision":"allow"}')
        self.assertEqual(d.get(), "allow")
        d.clear()
        self.assertEqual(d.get(), "")

    def test_junk_notify_ignored(self):
        d = bb.DecisionStore()
        d.set_from_notify(b"not json")
        d.set_from_notify(b'{"decision":"maybe"}')  # not allow/deny
        self.assertEqual(d.get(), "")


if __name__ == "__main__":
    unittest.main()
```

(No `tools/__init__.py` — plain same-directory module import, always run from `tools/`.)

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd tools && python -m unittest test_buddy_bridge -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'buddy_bridge'`

- [ ] **Step 3: Write the logic module**

```python
#!/usr/bin/env python3
# tools/buddy_bridge.py
"""On-demand BLE bridge: Claude Code hooks -> CYD buddy.

buddy_hook.py POSTs the same HTTP surface the device used to serve over WiFi
(POST /event, POST /ask, GET /decision, GET /) — but to 127.0.0.1, where this
bridge relays it over Bluetooth LE. It is spawned by the hook on demand
(connection refused -> spawn), uses its listening port as the single-instance
lock, and exits on its own after IDLE_EXIT_S without any HTTP request — so
there is no autostart entry and no standing drain on the laptop.
Design: docs/superpowers/specs/2026-07-16-ble-migration-design.md
"""
import json
import threading
import time

SERVICE_UUID = "177b0001-6f32-4ea3-b878-866e7628de1f"
INGRESS_UUID = "177b0002-6f32-4ea3-b878-866e7628de1f"
DECISION_UUID = "177b0003-6f32-4ea3-b878-866e7628de1f"
DEVICE_NAME = "claude-cyd"

IDLE_EXIT_S = 600     # no HTTP request this long -> exit (Claude idle)
SCAN_WINDOW_S = 90    # scanning budget after start/disconnect, then dormant
DORMANT_SCAN_S = 10   # short rescan length while dormant
DORMANT_GAP_S = 300   # minimum gap between dormant rescans


def make_envelope(kind, token, body):
    """GATT ingress payload: {"k","tok","d"} as compact UTF-8 JSON bytes.
    The device validates "tok" and dispatches on "k" ("event" | "ask")."""
    return json.dumps({"k": kind, "tok": token, "d": body},
                      separators=(",", ":")).encode("utf-8")


class LatestSlot:
    """Thread-safe latest-wins mailbox. Events are full snapshots, so a
    backlog collapses to the newest — a stale event has zero value."""

    def __init__(self):
        self._lock = threading.Lock()
        self._item = None
        self._loop = None
        self.event = None  # asyncio.Event, created by attach()

    def attach(self, loop):
        """Called by the BLE worker once its asyncio loop exists, so put()
        (HTTP thread) can wake the worker across threads."""
        import asyncio
        self._loop = loop
        self.event = asyncio.Event()

    def put(self, item):
        with self._lock:
            self._item = item
        if self._loop is not None:
            self._loop.call_soon_threadsafe(self.event.set)

    def take(self):
        with self._lock:
            item, self._item = self._item, None
        return item


class DecisionStore:
    """Latest allow/deny pushed by the device; cleared on each new /ask."""

    def __init__(self):
        self._lock = threading.Lock()
        self._decision = ""

    def clear(self):
        with self._lock:
            self._decision = ""

    def set_from_notify(self, data):
        try:
            d = json.loads(bytes(data).decode("utf-8")).get("decision", "")
        except Exception:
            return
        if d in ("allow", "deny"):
            with self._lock:
                self._decision = d

    def get(self):
        with self._lock:
            return self._decision


class Link:
    """State shared between the HTTP threads and the BLE worker."""

    def __init__(self):
        self.slot = LatestSlot()
        self.decisions = DecisionStore()
        self.connected = False
        self.loop = None    # the BLE worker's asyncio loop
        self.worker = None  # BleWorker, set in main()
        self.last_request = time.monotonic()

    def touch(self):
        self.last_request = time.monotonic()
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd tools && python -m unittest test_buddy_bridge -v`
Expected: `OK` — 8 tests pass.

- [ ] **Step 5: Commit**

```bash
git add tools/buddy_bridge.py tools/test_buddy_bridge.py
git commit -m "feat: bridge core logic (envelope, latest-wins slot, decision store)"
```

---

### Task 2: Bridge HTTP surface, singleton lock, idle-exit

**Files:**
- Modify: `tools/buddy_bridge.py` (append HTTP handler + `main()`)
- Test: manual localhost checks (below) — no device, no bleak needed.

**Interfaces:**
- Consumes: Task 1's `Link`, `make_envelope`.
- Produces: HTTP endpoints `POST /event` (always 202), `POST /ask` (200 delivered / 502 not connected), `GET /decision` (`{"decision":""|"allow"|"deny"}`), `GET /` (health incl. `"connected"`); CLI flags `--port`, `--listen`, `--idle-exit`, `--no-ble`.

- [ ] **Step 1: Append the HTTP layer and main() to `tools/buddy_bridge.py`**

```python
# ---- HTTP surface (mirrors the device's old WiFi endpoints) -----------------
import argparse
import asyncio
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CFG = os.path.join(os.path.expanduser("~"), ".claude", "buddy.json")


def _cfg_port():
    try:
        with open(CFG, "r", encoding="utf-8") as f:
            return int(json.load(f).get("port", 8787) or 8787)
    except Exception:
        return 8787


class Handler(BaseHTTPRequestHandler):
    link = None  # class attr, set in main()

    def log_message(self, *a):  # stay silent (spawned headless by the hook)
        pass

    def _body(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        return self.rfile.read(n) if n else b""

    def _send(self, code, obj):
        raw = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        self.link.touch()
        if self.path == "/decision":
            self._send(200, {"decision": self.link.decisions.get()})
        else:
            self._send(200, {"ok": True, "connected": self.link.connected})

    def do_POST(self):
        self.link.touch()
        tok = self.headers.get("X-Buddy-Token", "")
        try:
            body = json.loads(self._body().decode("utf-8"))
        except Exception:
            self._send(400, {"ok": False, "error": "bad json"})
            return
        if self.path == "/event":
            # Always accepted: the BLE side delivers the newest snapshot when
            # it can; a dropped event is healed by the next one.
            self.link.slot.put(make_envelope("event", tok, body))
            self._send(202, {"ok": True})
        elif self.path == "/ask":
            # Synchronous-ish: the hook is blocking on this, so confirm the
            # GATT write actually landed (or fail fast so the hook fails open).
            self.link.decisions.clear()
            if not (self.link.connected and self.link.loop):
                self._send(502, {"ok": False, "error": "device not connected"})
                return
            try:
                fut = asyncio.run_coroutine_threadsafe(
                    self.link.worker.send_ask(make_envelope("ask", tok, body)),
                    self.link.loop)
                fut.result(timeout=4)
                self._send(200, {"ok": True})
            except Exception:
                self._send(502, {"ok": False, "error": "ble write failed"})
        else:
            self._send(404, {"ok": False})


def main(argv=None):
    ap = argparse.ArgumentParser(description="CYD buddy BLE bridge")
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--listen", default="127.0.0.1",
                    help="bind address; 0.0.0.0 lets other machines drive the "
                         "buddy through this bridge")
    ap.add_argument("--idle-exit", type=float, default=IDLE_EXIT_S)
    ap.add_argument("--no-ble", action="store_true",
                    help="HTTP surface only (tests)")
    args = ap.parse_args(argv)

    link = Link()
    # The bind IS the single-instance lock: with SO_REUSEADDR off, a second
    # bridge's bind fails and it exits silently (the hook spawns eagerly).
    ThreadingHTTPServer.allow_reuse_address = False
    try:
        httpd = ThreadingHTTPServer((args.listen, args.port or _cfg_port()),
                                    Handler)
    except OSError:
        return 0  # another bridge is already serving
    Handler.link = link
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    if not args.no_ble:
        worker = BleWorker(link)
        link.worker = worker
        threading.Thread(target=lambda: asyncio.run(worker.run()),
                         daemon=True).start()

    # Sole exit condition: Claude has gone quiet. (Device-absent costs ~zero
    # while events still flow — the worker sits dormant, radio silent.)
    while time.monotonic() - link.last_request < args.idle_exit:
        time.sleep(5)
    httpd.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

NOTE: `BleWorker` doesn't exist yet — that's Task 3. For this task to be runnable, add a placeholder **stub in this same step** directly above `main()`:

```python
class BleWorker:
    """BLE side — implemented in Task 3. The stub keeps --no-ble runs and the
    unit tests importable before bleak enters the picture."""

    def __init__(self, link):
        self.link = link

    async def run(self):
        return

    async def send_ask(self, envelope):
        raise RuntimeError("BLE not implemented yet")
```

(Task 3 **replaces** this stub wholesale.)

- [ ] **Step 2: Unit tests still pass**

Run: `cd tools && python -m unittest test_buddy_bridge -v`
Expected: `OK`

- [ ] **Step 3: Behavior test — endpoints, singleton, idle-exit (no device needed)**

Run (Git Bash, from repo root):

```bash
python tools/buddy_bridge.py --no-ble --port 18787 --idle-exit 8 &
sleep 1
curl -s --noproxy "*" http://127.0.0.1:18787/                      # health
curl -s --noproxy "*" -X POST -H "X-Buddy-Token: t" -d '{"running":1}' http://127.0.0.1:18787/event
curl -s --noproxy "*" -X POST -H "X-Buddy-Token: t" -d '{"tool":"Bash"}' http://127.0.0.1:18787/ask
curl -s --noproxy "*" http://127.0.0.1:18787/decision
python tools/buddy_bridge.py --no-ble --port 18787 --idle-exit 8   # 2nd instance
echo "second instance exited: $?"
sleep 15
curl -s --noproxy "*" -m 2 http://127.0.0.1:18787/ || echo "IDLE-EXITED OK"
```

Expected, in order:
- `{"ok": true, "connected": false}`
- `{"ok": true}` (202 accepted event)
- `{"ok": false, "error": "device not connected"}` (502)
- `{"decision": ""}`
- second instance returns **immediately**, exit code 0
- after the idle window: connection refused → `IDLE-EXITED OK`

- [ ] **Step 4: Commit**

```bash
git add tools/buddy_bridge.py
git commit -m "feat: bridge HTTP surface, port-lock singleton, idle self-exit"
```

---

### Task 3: Bridge BLE worker (bleak)

**Files:**
- Modify: `tools/buddy_bridge.py` (replace the `BleWorker` stub)

**Interfaces:**
- Consumes: `Link`, UUID constants, `LatestSlot.attach/take/event`.
- Produces: `BleWorker.run()` (scan → connected → dormant states), `BleWorker.send_ask(envelope)`.

- [ ] **Step 1: Install bleak**

Run: `python -m pip install bleak`
Expected: `Successfully installed bleak-...` (WinRT backend comes with it).

- [ ] **Step 2: Replace the BleWorker stub with the real worker**

```python
class BleWorker:
    """Owns the BLE side on its own asyncio loop.

    States: SCANNING (budgeted burst after start/disconnect) -> CONNECTED
    (no scanning; connection upkeep is far cheaper than scanning) -> DORMANT
    (scan gave up: radio quiet, short rescan every DORMANT_GAP_S while events
    keep arriving). bleak is imported lazily so --no-ble runs and the unit
    tests never need it."""

    def __init__(self, link):
        self.link = link
        self._client = None

    async def send_ask(self, envelope):
        c = self._client
        if not (c and c.is_connected):
            raise RuntimeError("not connected")
        await c.write_gatt_char(INGRESS_UUID, envelope, response=True)

    async def _pump(self, client, disconnected):
        """Forward the newest snapshot whenever one is waiting."""
        ev = self.link.slot.event
        while not disconnected.is_set():
            env = self.link.slot.take()
            if env is not None:
                await client.write_gatt_char(INGRESS_UUID, env, response=True)
                continue
            try:
                await asyncio.wait_for(ev.wait(), timeout=2)
            except asyncio.TimeoutError:
                pass
            ev.clear()

    async def run(self):
        from bleak import BleakClient, BleakScanner
        link = self.link
        link.loop = asyncio.get_running_loop()
        link.slot.attach(link.loop)
        scan_until = time.monotonic() + SCAN_WINDOW_S
        last_scan = 0.0
        while True:
            dormant = time.monotonic() > scan_until
            if dormant and time.monotonic() - last_scan < DORMANT_GAP_S:
                await asyncio.sleep(15)
                continue
            last_scan = time.monotonic()
            try:
                dev = await BleakScanner.find_device_by_name(
                    DEVICE_NAME, timeout=DORMANT_SCAN_S if dormant else 15.0)
            except Exception:
                await asyncio.sleep(5)  # BT stack hiccup (sleep/resume) — retry
                continue
            if dev is None:
                continue
            disconnected = asyncio.Event()

            def _on_dc(_c):
                link.loop.call_soon_threadsafe(disconnected.set)

            try:
                async with BleakClient(dev,
                                       disconnected_callback=_on_dc) as client:
                    self._client = client
                    await client.start_notify(
                        DECISION_UUID,
                        lambda _h, data: link.decisions.set_from_notify(data))
                    link.connected = True
                    await self._pump(client, disconnected)
            except Exception:
                await asyncio.sleep(2)
            finally:
                self._client = None
                link.connected = False
                # fresh scanning budget after losing the device
                scan_until = time.monotonic() + SCAN_WINDOW_S
```

- [ ] **Step 3: Unit tests still pass (bleak stays un-imported)**

Run: `cd tools && python -m unittest test_buddy_bridge -v`
Expected: `OK`

- [ ] **Step 4: Smoke — real worker, no device present**

Run: `python tools/buddy_bridge.py --port 18787 --idle-exit 30 & sleep 20 && curl -s --noproxy "*" http://127.0.0.1:18787/`
Expected: `{"ok": true, "connected": false}` — the worker scanned, found nothing, and is heading dormant without crashing. Process exits by itself ~30 s after the last curl.

- [ ] **Step 5: Commit**

```bash
git add tools/buddy_bridge.py
git commit -m "feat: bridge BLE worker (scan/connected/dormant, decision notify)"
```

---

### Task 4: Firmware — partitions, config, and the NimBLE module

**Files:**
- Modify: `partitions.csv`, `platformio.ini`
- Create: `src/net/ble.h`, `src/net/ble.cpp`

**Interfaces:**
- Consumes: existing `net::AppState` semantics (from `src/net/server.h`) — field-for-field, minus `wifiUp`/`ip`, plus `linkUp`.
- Produces: `net::Ble` with `setToken(const String&)`, `begin()`, `loop()`, `state() -> AppState&`; global `net::ble`. Task 5's rewire depends on exactly these names.

- [ ] **Step 1: Replace `partitions.csv`** (nvs and littlefs offsets MUST NOT move — battery/touch/stats/token live in nvs, the GIF pack in littlefs):

```
# Factory-only layout for 4MB flash (BLE build: espota + the 2nd app slot are
# gone -- firmware updates are USB-only now). nvs and littlefs keep their exact
# offsets so the token, touch + battery calibration, stats ring and GIF pack
# all survive the migration flash. The old otadata region (0xE000) is a dead
# gap. 0x10000 + 0x2A0000 = 0x2B0000; 0x2B0000 + 0x150000 = 4MB exactly.
# Name,     Type, SubType, Offset,   Size
nvs,        data, nvs,     0x9000,   0x5000
app0,       app,  factory, 0x10000,  0x2A0000
littlefs,   data, spiffs,  0x2B0000, 0x150000
```

- [ ] **Step 2: `platformio.ini`** — in `[env:cyd]` `lib_deps`, add NimBLE and (this task only — removed in Task 5) keep WiFiManager:

```ini
    h2zero/NimBLE-Arduino@^1.4.3
```

and append to `build_flags` (flash trim: this device is peripheral-only):

```ini
    -DCONFIG_BT_NIMBLE_ROLE_CENTRAL_DISABLED
    -DCONFIG_BT_NIMBLE_ROLE_OBSERVER_DISABLED
    -DCONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
```

- [ ] **Step 3: Create `src/net/ble.h`**

```cpp
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
```

- [ ] **Step 4: Create `src/net/ble.cpp`** — the `applyEvent` body is a verbatim port of the old `handleEvent` field logic (`src/net/server.cpp:44-86`); keep those comments:

```cpp
#include "ble.h"
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace net {

Ble ble;
static AppState g_state;
static String g_token;

static const char *SVC_UUID = "177b0001-6f32-4ea3-b878-866e7628de1f";
static const char *ING_UUID = "177b0002-6f32-4ea3-b878-866e7628de1f";
static const char *DEC_UUID = "177b0003-6f32-4ea3-b878-866e7628de1f";

// NimBLE callbacks run on the NimBLE host task; the renderer owns AppState on
// the Arduino loop task. Raw payloads cross over through this queue as heap
// copies and are parsed + applied in loop() — same "single-threaded AppState"
// contract the WebServer version had.
#define Q_DEPTH 8
#define PAYLOAD_MAX 2048
static QueueHandle_t g_q = nullptr;
static volatile bool g_connected = false;
static NimBLECharacteristic *g_decision = nullptr;

class IngressCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    NimBLEAttValue v = c->getValue();
    if (v.length() == 0 || v.length() > PAYLOAD_MAX)
      return;
    char *copy = (char *)malloc(v.length() + 1);
    if (!copy)
      return;
    memcpy(copy, v.data(), v.length());
    copy[v.length()] = 0;
    if (xQueueSend(g_q, &copy, 0) != pdTRUE)
      free(copy); // queue full: drop (snapshot semantics; the next event heals)
  }
};

class SrvCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *) override { g_connected = true; }
  void onDisconnect(NimBLEServer *) override { g_connected = false; }
};

static IngressCb g_ingressCb;
static SrvCb g_srvCb;

static void applyEvent(JsonVariant d) {
  AppState &s = g_state;
  // Drop out-of-order arrivals (async hooks can reorder): an event stamped older
  // than the last one we applied must not clobber newer state -- this is what
  // kept "running" pinned after a turn ended (a late PostToolUse landing after
  // Stop). Events without a ts (older hooks) are always applied.
  long long ts = d["ts"] | (long long)0;
  if (ts != 0 && ts < s.lastTs)
    return;
  if (ts > s.lastTs)
    s.lastTs = ts;
  s.total = d["total"] | s.total;
  s.running = d["running"] | 0;
  if (d["msg"].is<const char *>())
    s.msg = (const char *)d["msg"];
  if (d["project"].is<const char *>())
    s.project = (const char *)d["project"];
  if (d["date"].is<const char *>())
    s.date = (const char *)d["date"];
  s.tokens = d["tokens"] | s.tokens;
  s.tokensAll = d["tokensAll"] | (long long)s.tokensAll;
  s.tools = d["tools"] | s.tools;
  s.turns = d["turns"] | s.turns;
  s.sessions = d["sessions"] | s.sessions;
  // sticky per-activity clip name (typing/building/...) for the running state.
  if (d["act"].is<const char *>())
    s.act = (const char *)d["act"];
  // optional transient effect (attention/celebrate/heart): bump fxId so the
  // renderer fires it exactly once.
  if (d["fx"].is<const char *>()) {
    s.fx = (const char *)d["fx"];
    if (s.fx.length())
      s.fxId++;
  }
  // "Claude is waiting on you" sticky + intensity inputs. Every event sends
  // these explicitly (default 0/false), so e.g. a tool starting clears waiting.
  bool wasWaiting = s.waiting;
  s.waiting = d["waiting"] | false;
  if (s.waiting && !wasWaiting)
    s.waitId++; // a fresh wait began -> restart the escalating nudge timer
  s.burst = d["burst"] | 0;
  s.agents = d["agents"] | 0;
  if (d["budget"].is<long>() || d["budget"].is<int>())
    s.budget = d["budget"] | s.budget; // sticky once provided
  s.actSeq++; // every event ticks this -> renderer switches clip in lock-step
  s.dirty = true;
}

// "ask" envelope -> show the Allow/Deny prompt; the bridge relays our decision
// notify back to the (polling) hook.
static void applyAsk(JsonVariant d) {
  g_state.askTool = (const char *)(d["tool"] | "this tool");
  g_state.askId++;
  g_state.decision = ""; // reset; undecided until a tap
  g_state.dirty = true;
}

void Ble::begin() {
  g_q = xQueueCreate(Q_DEPTH, sizeof(char *));
  NimBLEDevice::init("claude-cyd");
  NimBLEDevice::setMTU(517); // event envelopes (~400B) fit one write
  NimBLEServer *srv = NimBLEDevice::createServer();
  srv->setCallbacks(&g_srvCb);
  srv->advertiseOnDisconnect(true);
  NimBLEService *svc = srv->createService(SVC_UUID);
  NimBLECharacteristic *ing =
      svc->createCharacteristic(ING_UUID, NIMBLE_PROPERTY::WRITE);
  ing->setCallbacks(&g_ingressCb);
  g_decision = svc->createCharacteristic(DEC_UUID, NIMBLE_PROPERTY::READ |
                                                       NIMBLE_PROPERTY::NOTIFY);
  g_decision->setValue("{\"decision\":\"\"}");
  svc->start();
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->start();
  Serial.println("[ble] advertising as claude-cyd");
  g_state.dirty = true;
}

void Ble::loop() {
  // link dot follows the central's connection
  if (g_state.linkUp != g_connected) {
    g_state.linkUp = g_connected;
    g_state.dirty = true;
  }
  // drain queued envelopes (parsed HERE, on the loop task)
  char *raw = nullptr;
  while (g_q && xQueueReceive(g_q, &raw, 0) == pdTRUE) {
    JsonDocument env;
    bool ok = deserializeJson(env, raw) == DeserializationError::Ok;
    free(raw);
    if (!ok)
      continue;
    if (g_token.length() &&
        String((const char *)(env["tok"] | "")) != g_token)
      continue; // bad/missing token: drop silently
    const char *kind = env["k"] | "";
    if (!strcmp(kind, "event"))
      applyEvent(env["d"]);
    else if (!strcmp(kind, "ask"))
      applyAsk(env["d"]);
  }
  // push a fresh Allow/Deny to the bridge (notify; read is the poll fallback)
  static uint32_t lastPushed = 0;
  if (g_decision && g_state.decidedId == g_state.askId &&
      g_state.decidedId != lastPushed && g_state.decision.length()) {
    lastPushed = g_state.decidedId;
    String j = String("{\"askId\":") + g_state.decidedId + ",\"decision\":\"" +
               g_state.decision + "\"}";
    g_decision->setValue(j.c_str());
    if (g_connected)
      g_decision->notify();
  }
}

AppState &Ble::state() { return g_state; }

void Ble::setToken(const String &t) {
  g_token = t;
  g_state.token = t;
}

} // namespace net
```

- [ ] **Step 5: Build**

Run: `python -m platformio run -e cyd`
Expected: SUCCESS (both stacks temporarily linked; the app partition is now 2.625 MB so size is not a concern). **Contingency:** if the transitional link fails on DRAM/IRAM overflow (WiFi + NimBLE statics colliding), do NOT shrink anything — proceed straight into Task 5 (which deletes the WiFi stack) and treat Task 5's build as the gate for both tasks, committing them together.

- [ ] **Step 6: Commit**

```bash
git add partitions.csv platformio.ini src/net/ble.h src/net/ble.cpp
git commit -m "feat: NimBLE GATT server + factory-only partition layout (nvs/littlefs pinned)"
```

---

### Task 5: Firmware — rewire main/screens, delete the WiFi stack

**Files:**
- Modify: `src/main.cpp`, `src/app/store.cpp`, `src/screens/ask.cpp`, `src/screens/home.cpp`, `src/screens/stats_panel.cpp`, `src/screens/settings.cpp`, `src/screens/layout.h`, `src/screens/layout.cpp`, `platformio.ini`
- Delete: `src/net/server.h`, `src/net/server.cpp`, `src/screens/wifi_confirm.h`, `src/screens/wifi_confirm.cpp`

**Interfaces:**
- Consumes: Task 4's `net::ble` (`begin()/loop()/state()/setToken()`), `AppState.linkUp`.

- [ ] **Step 1: Mechanical rename across `src/`** — in `src/main.cpp`, `src/app/store.cpp`, `src/screens/ask.cpp` (the files that include the old header):
  - `#include "net/server.h"` → `#include "net/ble.h"`
  - every `net::server.` → `net::ble.`
  (home.cpp/stats_panel.cpp receive `AppState&` as a parameter — check their headers; only fix includes if they name server.h directly.)

- [ ] **Step 2: `src/main.cpp` — remove OTA + WiFi flow** (line numbers from the pre-edit file):
  - Delete `#include <ArduinoOTA.h>` (line 2) and `#include "screens/wifi_confirm.h"` (line 23).
  - Delete the whole `otaSetup()` function (lines 129–181) and the OTA arming block in `loop()` (lines 409–417, `static bool otaUp ... ArduinoOTA.handle();`).
  - Delete `static bool wifiConfirmOpen = false;` (78) and every `wifiConfirmOpen` reference: the settings-branch (222–225), the idle-timeout check (578–580 → keep the check for `settingsOpen || statsOpen` only), the ask-open reset (452), `pollBattery`'s guard (327), `pressOnHome` (573), and the whole `if (wifiConfirmOpen) {...}` mode block (644–660).
  - `handleSettingsTap`: loop bound `7` → `6`; delete the `i == 5` WiFi branch; the close branch comment becomes `// close (i == 5)`.
  - Delete both `net::server.nudgeReconnect();` calls (BOOT-key wake ~311, screen wake ~539) — BLE advertising needs no kick.
  - `stateName()`: `if (!s.wifiUp)` → `if (!s.linkUp)` — the buddy now sleeps whenever no bridge is attached, which is exactly "Claude isn't in use".
  - `setup()`: banner → `"\n[CYD Buddy] BLE + official Clawd character"`; replace the whole "Connecting WiFi..." draw + `net::server.begin([](const String &ap) {...});` block (lines 370–388) with just `net::ble.begin();`; the boot printf → `Serial.printf("char=%d\n", haveChar);`.
  - `loop()` head: `net::server.loop();` → `net::ble.loop();`.
  - Screen-off comment (~750): `// still tracking over WiFi, but at the WiFi-safe` → `// BLE stays connectable, and 80 MHz is the radio-safe`.

- [ ] **Step 3: screens**
  - `src/screens/layout.h`: `extern ui::Rect setBtns[7];` → `[6]`; comment line 12 → `// Settings: Power off / Stats / Quiet / Brightness / Recalibrate / Close`.
  - `src/screens/layout.cpp`: `ui::Rect setBtns[7];` → `[6]`; loop `i < 7` → `i < 6`; comment → `// 6 rows fit 240x320 (46 + 6*38 = 274)`.
  - `src/screens/settings.cpp`: `labels[7]` → `labels[6]` dropping `"WiFi setup"`; loop `i < 7` → `i < 6`.
  - `src/screens/home.cpp`: both `s.wifiUp` → `s.linkUp` (lines 234, 246); comment 225 `WiFi dot` → `link dot`.
  - `src/screens/stats_panel.cpp`: `row("IP", s.ip);` → `row("Link", s.linkUp ? String("BLE connected") : String("advertising"));`
  - Delete `src/screens/wifi_confirm.h` and `src/screens/wifi_confirm.cpp`.

- [ ] **Step 4: Delete `src/net/server.h` + `src/net/server.cpp`.** `platformio.ini`: remove the `https://github.com/tzapu/WiFiManager.git` lib_deps line and the whole `[env:cyd-ota]` section including its "WiFi reflash" comment block.

- [ ] **Step 5: Build**

Run: `python -m platformio run -e cyd`
Expected: SUCCESS. Note the flash/RAM numbers in the commit message (expect roughly 1.0–1.3 MB of the 2.625 MB slot).

- [ ] **Step 6: Commit**

```bash
git add -A src platformio.ini
git commit -m "refactor: WiFi stack out, BLE transport in (settings 7->6 rows, link dot)"
```

---

### Task 6: Battery constant + docs

**Files:**
- Modify: `src/app/battery.cpp`, `README.md`, `tools/HOOKS.md`, `docs/DESIGN.md`, `docs/battery-gauge-spec.md`

- [ ] **Step 1: `src/app/battery.cpp` line 13:**

```cpp
static const float MA_BASE = 90.0f;  // board + ESP32 + BLE, screen dark. First
                                     // guess (the WiFi build measured ~143);
                                     // the death-anchored capacity calibration
                                     // absorbs the error over cycles.
```

- [ ] **Step 2: `README.md`** — replace the WiFi provisioning / OTA / mDNS sections with: setup = flash over USB (`-e cyd`, COM5), `pip install bleak`, hooks + `~/.claude/buddy.json` = `{"token": "YOUR_DEVICE_TOKEN"}` (optional `"port"`, `"budget"`); "How it connects" = the bridge paragraph (hook spawns `tools/buddy_bridge.py` on demand; it binds 127.0.0.1:8787, relays over BLE, exits after 10 idle minutes; no autostart). Troubleshooting: replace the espota "No response" entry with (a) "buddy stuck asleep / dot dark → is a Claude session running? The bridge only lives while events flow", (b) "bridge never connects → toggle Windows Bluetooth off/on (the stack wedges after sleep/resume), then run any Claude turn to respawn the bridge". Keep placeholders only.

- [ ] **Step 3: `tools/HOOKS.md`** — config snippet: `{"ip": ...}` → `{"token": "...."}`; multi-machine section: other machines POST to **this PC's bridge** (`python tools/buddy_bridge.py --listen 0.0.0.0` + reach the laptop over LAN/Tailscale); the ESP32 itself is no longer on any network.

- [ ] **Step 4: `docs/DESIGN.md` + `docs/battery-gauge-spec.md`** — read both; update transport mentions (WiFi/HTTP/WebServer/OTA → BLE bridge, one paragraph each, reuse the spec's architecture diagram) and the 143 mA base-current mention (now 90 mA initial guess, BLE). No structural rewrites.

- [ ] **Step 5: Build still clean + commit**

```bash
python -m platformio run -e cyd
git add src/app/battery.cpp README.md tools/HOOKS.md docs/DESIGN.md docs/battery-gauge-spec.md
git commit -m "docs: BLE-era setup/troubleshooting; battery base current 143->90mA first guess"
```

---

### Task 7: Hook cutover (retarget + spawn-on-refused)

**Files:**
- Modify: `tools/buddy_hook.py`
- User config (not committed): `~/.claude/buddy.json`

**Interfaces:**
- Consumes: bridge HTTP surface (Task 2), `tools/buddy_bridge.py` path (sibling file).
- Produces: `_cfg() -> (host, token)` where host is `"127.0.0.1:<port>"`.

⚠️ This session's own hooks run this file live — make each file change in ONE atomic Write/Edit, and expect a stray `buddy_bridge.py` to appear (spawned by our own hooks) during testing; that's the feature working.

- [ ] **Step 1: Edit `tools/buddy_hook.py`:**
  - Docstring line 12: `Config: ~/.claude/buddy.json  ->  {"token": "....", "port": 8787 (optional)}` and add a line: `Transport: POSTs to the local buddy_bridge.py (spawned on demand), which relays over BLE.`
  - Add `import urllib.error` and `import subprocess` next to the existing imports.
  - Replace `_cfg`:

```python
def _cfg():
    """Bridge endpoint + device token. The device no longer has an IP — the
    on-demand local bridge (buddy_bridge.py) relays everything over BLE."""
    with open(CFG, "r", encoding="utf-8") as f:
        c = json.load(f)
    return "127.0.0.1:%d" % int(c.get("port", 8787) or 8787), c["token"]
```

  - Add after `_cfg`:

```python
def _spawn_bridge():
    """Fire-and-forget: start the bridge headless. The current event is
    dropped (snapshot semantics — the next one heals the display); the
    bridge's port-bind makes concurrent spawns collapse to one instance."""
    bridge = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "buddy_bridge.py")
    kw = {"stdin": subprocess.DEVNULL, "stdout": subprocess.DEVNULL,
          "stderr": subprocess.DEVNULL, "close_fds": True}
    if sys.platform == "win32":
        # DETACHED_PROCESS | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP
        kw["creationflags"] = 0x00000008 | 0x08000000 | 0x00000200
    try:
        subprocess.Popen([sys.executable, bridge], **kw)
    except Exception:
        pass


def _refused(exc):
    return isinstance(getattr(exc, "reason", None), ConnectionRefusedError)
```

  - `_post_event` (rename param `ip` → `host`, spawn on refused):

```python
def _post_event(host, tok, payload):
    req = urllib.request.Request(
        "http://%s/event" % host,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "X-Buddy-Token": tok},
        method="POST",
    )
    try:
        _opener.open(req, timeout=5).read()
    except urllib.error.URLError as e:
        if _refused(e):
            _spawn_bridge()  # bridge wasn't running; this event is dropped
        raise
```

  - `_ask_decision` (rename `ip` → `host`; the initial POST gets one spawn-and-retry; the poll loop is unchanged apart from the variable name):

```python
def _ask_decision(host, tok, tool, timeout=26):
    """Show an Allow/Deny prompt on the device, then poll the bridge for the
    tap. Returns "allow"/"deny", or "" on timeout/unreachable so the caller
    FAILS OPEN to Claude's normal permission prompt."""
    req = urllib.request.Request(
        "http://%s/ask" % host,
        data=json.dumps({"tool": tool}).encode("utf-8"),
        headers={"Content-Type": "application/json", "X-Buddy-Token": tok},
        method="POST",
    )
    for attempt in (0, 1):
        try:
            _opener.open(req, timeout=4).read()
            break
        except urllib.error.URLError as e:
            if attempt == 0 and _refused(e):
                _spawn_bridge()
                time.sleep(1.5)  # bridge boots fast; BLE connect races the poll
                continue
            return ""  # not connected / no bridge -> normal prompt
        except Exception:
            return ""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            req = urllib.request.Request("http://%s/decision" % host,
                                         headers={"X-Buddy-Token": tok})
            r = json.loads(_opener.open(req, timeout=3).read().decode("utf-8"))
            if r.get("decision") in ("allow", "deny"):
                return r["decision"]
        except Exception:
            pass
        time.sleep(0.4)
    return ""
```

  - In `main()`: `ip, tok = _cfg()` → `host, tok = _cfg()`, and the two call sites `_ask_decision(ip, ...)`/`_post_event(ip, ...)` → `host`.

- [ ] **Step 2: Test on the laptop (no device needed)**

```bash
curl -s --noproxy "*" -m 2 http://127.0.0.1:8787/ || echo "no bridge yet (good)"
echo '{"hook_event_name":"Stop"}' | python tools/buddy_hook.py; echo "hook exit: $?"
sleep 2
curl -s --noproxy "*" http://127.0.0.1:8787/
```

Expected: "no bridge yet (good)"; hook exit 0 (fast, non-blocking); then `{"ok": true, "connected": false}` — the hook spawned a live bridge. Leave it; it self-exits in 10 min.

- [ ] **Step 3: Update `~/.claude/buddy.json`** (user machine, NOT committed): keep `"token"` (and `"budget"` if present), delete `"ip"`.

- [ ] **Step 4: Commit**

```bash
git add tools/buddy_hook.py
git commit -m "feat: hooks target the local BLE bridge; spawn it on demand"
```

---

### Task 8: Migration flash + end-to-end verification (device + user present)

**Files:** none (hardware/system steps). The device runs the old WiFi firmware until this task; from Task 7 on, the hook already talks to the bridge, so the buddy is dark for the interim — keep Tasks 7→8 back-to-back.

- [ ] **Step 1: Flash over USB** (cable to COM5):
`python -m platformio run -e cyd -t upload`
Expected: new partition table + firmware written; boot log shows `[ble] advertising as claude-cyd`; "No core dump partition found" is benign.

- [ ] **Step 2: Survival checks on the device:** GIFs animate (littlefs untouched — no uploadfs was run); long-press → Settings shows 6 rows; Stats panel shows the old token-day stats + battery % (nvs untouched); touch works without recalibration; trends card still has its 14-day history.

- [ ] **Step 3: Live end-to-end:** run any Claude Code turn on the laptop. Within ~20 s: bridge spawns, connects (top-bar dot green, Stats panel row "Link: BLE connected"), Clawd animates with the session. Verify: `curl -s --noproxy "*" http://127.0.0.1:8787/` → `"connected": true`.

- [ ] **Step 4: Ask round-trip** (PermissionRequest hook is opt-in — only if enabled in `~/.claude/settings.json`): trigger a permission prompt; tap Allow on the device; Claude proceeds with "Approved on the Claude Buddy device".

- [ ] **Step 5: Lifecycle:** close all Claude sessions; after ~10 min `tasklist | findstr python` shows no bridge, and the buddy's dot goes dark / state falls to sleep. Start a new session → everything comes back by itself.

- [ ] **Step 6: Deep-sleep path:** let the device auto-sleep (or Settings → Power off), touch to wake → advertising resumes → bridge reconnects on the next event within a scan window.

- [ ] **Step 7: Push** (account `qiankangwang`): `git push origin main`.

Battery-model note: the 90 mA guess self-corrects over the next death-anchored charge cycles — nothing to do now.

---

## Self-review notes (already applied)

- Ordering: hook cutover (Task 7) sits immediately before the flash (Task 8) because `~/.claude/settings.json` runs the repo's hook file live — minimizes the dark window where the device still speaks WiFi but hooks speak bridge.
- Spec coverage: lifecycle §1 → Tasks 2/7; GATT §2 → Task 4; firmware §3 → Tasks 4/5; partitions §4 → Task 4 + 8; PC side §5 → Tasks 1–3/7; error table → Tasks 2/3/7; success criteria → Task 8.
- The `wifiUp→linkUp` sleep-state semantic change (buddy sleeps whenever no bridge is attached) is intended and called out in Task 5 Step 2.
