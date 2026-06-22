#!/usr/bin/env python3
"""Claude Code hook -> CYD buddy bridge.

Reads the hook event JSON on stdin and pushes a status + usage snapshot to the
CYD over the LAN (HTTP). The device is a passive stats dashboard (the orange
Clawd mascot reacts to what Claude is doing); there is no on-device approval, so
every event is non-blocking and never affects Claude's own permission flow.

Per event it sends: current activity, project name, and today's usage rollup
(tokens, all-time tokens, tool calls, assistant turns, session count).

Config: ~/.claude/buddy.json  ->  {"ip": "192.168.x.x", "token": "...."}
Fail-open: any device/network error is swallowed (never blocks the session).
"""
import json
import os
import sys
import time
import urllib.request

CFG = os.path.join(os.path.expanduser("~"), ".claude", "buddy.json")
TOK_STATE = os.path.join(os.path.expanduser("~"), ".claude", "buddy_tokens.json")

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


def _project(data):
    cwd = data.get("cwd") or os.getcwd()
    return os.path.basename(os.path.normpath(cwd))[:24]


def _scan_transcript(path):
    """One pass over a session log: sum tokens, count tool_use blocks + turns."""
    tok = tools = turns = 0
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    o = json.loads(line)
                except Exception:
                    continue
                msg = o.get("message") or {}
                if not isinstance(msg, dict):
                    msg = {}
                u = msg.get("usage")
                if isinstance(u, dict):
                    tok += int(u.get("input_tokens", 0) or 0)
                    tok += int(u.get("output_tokens", 0) or 0)
                if o.get("type") == "assistant":
                    turns += 1
                    content = msg.get("content")
                    if isinstance(content, list):
                        for b in content:
                            if isinstance(b, dict) and b.get("type") == "tool_use":
                                tools += 1
    except Exception:
        return None
    return {"tok": tok, "tools": tools, "turns": turns}


def _today_stats(data):
    """Today's tokens/tools/turns/session-count (persisted, resets at local
    midnight) plus an all-time token counter. Returns a dict or None."""
    tp, sid = data.get("transcript_path"), data.get("session_id")
    if not tp or not sid:
        return None
    sess = _scan_transcript(tp)
    if sess is None:
        return None
    today = time.strftime("%Y-%m-%d", time.localtime())
    try:
        with open(TOK_STATE, "r", encoding="utf-8") as f:
            st = json.load(f)
    except Exception:
        st = {}
    if not isinstance(st, dict):
        st = {}
    base = int(st.get("allTokBase", 0) or 0)
    sessions = st.get("sessions")
    if st.get("date") != today or not isinstance(sessions, dict):
        # new day (or first run / legacy format): roll the prior day into base
        if isinstance(sessions, dict):
            for v in sessions.values():
                if isinstance(v, dict):
                    base += int(v.get("tok", 0) or 0)
        sessions = {}
    sessions[sid] = sess
    st = {"date": today, "sessions": sessions, "allTokBase": base}
    try:
        with open(TOK_STATE, "w", encoding="utf-8") as f:
            json.dump(st, f)
    except Exception:
        pass

    def _sum(k):
        return sum(int(v.get(k, 0) or 0)
                   for v in sessions.values() if isinstance(v, dict))

    tok = _sum("tok")
    return {
        "tokens": tok,
        "tokensAll": base + tok,
        "tools": _sum("tools"),
        "turns": _sum("turns"),
        "sessions": len(sessions),
    }


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

    extra = {"project": _project(data)}
    stats = _today_stats(data)
    if stats:
        extra.update(stats)

    # All events are non-blocking; map each to a (running, total, activity) tuple.
    # While busy the DEVICE shows its own whimsical verb (synced to the
    # animation), so the activity text here is only used for idle/end states.
    if evt in ("PreToolUse", "PostToolUse", "UserPromptSubmit"):
        running, total, msg = 1, 1, "working"
    elif evt == "SessionStart":
        running, total, msg = 0, 1, "session started"
    elif evt == "Stop":
        running, total, msg = 0, 1, "done"
    elif evt == "SessionEnd":
        running, total, msg = 0, 0, "bye"
    elif evt == "Notification":
        running, total, msg = 0, 1, str(data.get("notification", "notice"))
    else:
        running, total, msg = 0, 1, evt

    try:
        _post_event(ip, tok, dict(extra, total=total, running=running,
                                  msg=msg[:24]))
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
