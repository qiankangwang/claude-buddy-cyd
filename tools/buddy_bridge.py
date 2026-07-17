#!/usr/bin/env python3
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
