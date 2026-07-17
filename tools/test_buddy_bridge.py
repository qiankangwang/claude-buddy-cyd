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
