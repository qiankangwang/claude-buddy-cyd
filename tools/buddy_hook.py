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
    """One pass over a session log -> {tok, tools, turns}.

    Two correctness points learned from real transcripts:
    * The same assistant message id is re-logged several times (streaming /
      updates). Counting every line double-counts tokens, turns and tools (~2x),
      so we dedupe by message id and keep the last occurrence.
    * tokens = input + output + cache_creation. We deliberately EXCLUDE
      cache_read_input_tokens: that's the cached context re-read on every turn
      and on a long session it's >95% of the raw total, which balloons the count
      without reflecting real usage."""
    seen = {}  # message id -> {tok, tools} (last occurrence wins)
    extra_tok = extra_tools = extra_turns = 0  # assistant msgs lacking an id
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
                if o.get("type") != "assistant":
                    continue
                msg = o.get("message") or {}
                if not isinstance(msg, dict):
                    msg = {}
                u = msg.get("usage")
                tok = 0
                if isinstance(u, dict):
                    tok = (int(u.get("input_tokens", 0) or 0)
                           + int(u.get("output_tokens", 0) or 0)
                           + int(u.get("cache_creation_input_tokens", 0) or 0))
                tools = 0
                content = msg.get("content")
                if isinstance(content, list):
                    for b in content:
                        if isinstance(b, dict) and b.get("type") == "tool_use":
                            tools += 1
                mid = msg.get("id")
                if mid:
                    seen[mid] = {"tok": tok, "tools": tools}
                else:
                    extra_tok += tok
                    extra_tools += tools
                    extra_turns += 1
    except Exception:
        return None
    return {
        "tok": sum(r["tok"] for r in seen.values()) + extra_tok,
        "tools": sum(r["tools"] for r in seen.values()) + extra_tools,
        "turns": len(seen) + extra_turns,
    }


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
    carry = st.get("carry")
    if not isinstance(carry, dict):
        carry = {}
    if st.get("date") != today or not isinstance(sessions, dict):
        # New local day (or first run / legacy format): roll the prior day's
        # tokens into the all-time base, and remember each session's rolled
        # count in `carry` so a session that continues PAST midnight doesn't get
        # its pre-midnight tokens counted again today (which double-counted them
        # in tokensAll, since base already holds them).
        new_carry = {}
        if isinstance(sessions, dict):
            for k, v in sessions.items():
                if isinstance(v, dict):
                    t = int(v.get("tok", 0) or 0)
                    base += t
                    new_carry[k] = t
        sessions = {}
        carry = new_carry
    sessions[sid] = sess
    st = {"date": today, "sessions": sessions, "allTokBase": base,
          "carry": carry}
    try:
        with open(TOK_STATE, "w", encoding="utf-8") as f:
            json.dump(st, f)
    except Exception:
        pass

    # today's tokens = each session's lifetime tokens minus whatever already
    # rolled into base at the last midnight (0 for sessions that started today).
    tok = 0
    for k, v in sessions.items():
        if isinstance(v, dict):
            tok += max(0, int(v.get("tok", 0) or 0) - int(carry.get(k, 0) or 0))

    def _sum(k):
        return sum(int(v.get(k, 0) or 0)
                   for v in sessions.values() if isinstance(v, dict))

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

    # Transient device animation cue (short, non-sticky): Claude wants you
    # (Notification), finished a turn (Stop), or a session just began (hello).
    fx = {"Notification": "attention", "Stop": "celebrate",
          "SessionStart": "heart"}.get(evt, "")

    try:
        payload = dict(extra, total=total, running=running, msg=msg[:24])
        if fx:
            payload["fx"] = fx
        _post_event(ip, tok, payload)
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
