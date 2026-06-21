#!/usr/bin/env python3
"""Claude Code hook -> CYD buddy bridge.

Reads the hook event JSON on stdin, talks to the CYD over the LAN (HTTP), and:
  * PreToolUse: shows the tool + asks for approval ON THE DEVICE, blocks until
    you tap Approve/Deny, then returns the permissionDecision to Claude Code.
  * other events (SessionStart/Stop/Notification/UserPromptSubmit/...): pushes a
    status snapshot so the buddy reflects what Claude is doing.

Config: ~/.claude/buddy.json  ->  {"ip": "192.168.x.x", "token": "...."}
Fail-open: any device/network error -> defer to Claude's normal flow (never
blocks or breaks the session).
"""
import json
import os
import sys
import time
import urllib.request

CFG = os.path.join(os.path.expanduser("~"), ".claude", "buddy.json")
POLL_TIMEOUT = 300  # seconds to wait for a tap before giving up

# Bypass any system/env HTTP proxy — the buddy is on the LAN.
_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _cfg():
    with open(CFG, "r", encoding="utf-8") as f:
        c = json.load(f)
    return c["ip"], c["token"]


def _post_event(ip, tok, payload):
    req = urllib.request.Request(
        "http://%s/event" % ip,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "X-Buddy-Token": tok},
        method="POST",
    )
    _opener.open(req, timeout=5).read()


def _get_decision(ip, tok):
    req = urllib.request.Request(
        "http://%s/decision" % ip, headers={"X-Buddy-Token": tok}
    )
    return json.loads(_opener.open(req, timeout=5).read().decode("utf-8"))["decision"]


def _hint(tool, ti):
    for k in ("command", "file_path", "path", "url", "pattern"):
        if isinstance(ti, dict) and ti.get(k):
            return str(ti[k])
    return json.dumps(ti)[:80] if ti else tool


def main():
    try:
        data = json.load(sys.stdin)
    except Exception:
        return 0
    evt = data.get("hook_event_name", "")
    try:
        ip, tok = _cfg()
    except Exception:
        return 0  # not configured -> do nothing

    if evt == "PreToolUse":
        tool = data.get("tool_name", "tool")
        hint = _hint(tool, data.get("tool_input", {}))
        try:
            _post_event(ip, tok, {
                "running": 1, "waiting": 1,
                "prompt": {"id": str(int(time.time() * 1000)),
                           "tool": tool, "hint": hint[:120]},
            })
        except Exception:
            return 0  # can't even reach the device -> defer to normal flow
        # Poll for the tap. A transient error on a single poll must NOT discard a
        # decision the user already made — keep polling until the window closes.
        deadline = time.time() + POLL_TIMEOUT
        dec = "pending"
        while time.time() < deadline:
            try:
                dec = _get_decision(ip, tok)
            except Exception:
                dec = "pending"
            if dec in ("allow", "deny"):
                break
            time.sleep(0.4)
        if dec in ("allow", "deny"):
            print(json.dumps({"hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": dec,
                "permissionDecisionReason": "Decided on CYD buddy",
            }}))
        return 0  # no tap in time -> defer to Claude's normal permission flow

    # Non-blocking status events.
    msg = {
        "SessionStart": "session started",
        "UserPromptSubmit": "thinking...",
        "Stop": "done",
        "SessionEnd": "bye",
        "Notification": str(data.get("notification", "attention")),
    }.get(evt, evt)
    running = 1 if evt in ("UserPromptSubmit", "PostToolUse") else 0
    total = 0 if evt in ("SessionEnd",) else 1
    try:
        _post_event(ip, tok, {"total": total, "running": running, "msg": msg})
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
