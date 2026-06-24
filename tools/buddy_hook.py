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
RT_STATE = os.path.join(os.path.expanduser("~"), ".claude", "buddy_rt.json")

# Bypass any system/env HTTP proxy — the buddy is on the LAN.
_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _cfg():
    with open(CFG, "r", encoding="utf-8") as f:
        c = json.load(f)
    return c["ip"], c["token"]


def _budget():
    """Optional daily token budget from buddy.json ("budget": 2000000) for the
    on-device budget gauge. 0/absent -> no gauge."""
    try:
        with open(CFG, "r", encoding="utf-8") as f:
            return int(json.load(f).get("budget", 0) or 0)
    except Exception:
        return 0


def _intensity(evt, tool):
    """Rolling-window session intensity: tool calls in the last 60s (burst) and
    distinct subagent spawns in the last 120s (agents). Persisted so the values
    decay between events instead of only reflecting this one call."""
    now = time.time()
    try:
        with open(RT_STATE, "r", encoding="utf-8") as f:
            st = json.load(f)
    except Exception:
        st = {}
    calls = [t for t in st.get("calls", []) if isinstance(t, (int, float))
             and now - t < 60]
    tasks = [t for t in st.get("tasks", []) if isinstance(t, (int, float))
             and now - t < 120]
    if evt == "PreToolUse":
        calls.append(now)
        if tool == "Task":
            tasks.append(now)
    try:
        with open(RT_STATE, "w", encoding="utf-8") as f:
            json.dump({"calls": calls[-200:], "tasks": tasks[-50:]}, f)
    except Exception:
        pass
    return len(calls), len(tasks)


def _post_event(ip, tok, payload):
    req = urllib.request.Request(
        "http://%s/event" % ip,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "X-Buddy-Token": tok},
        method="POST",
    )
    _opener.open(req, timeout=5).read()


def _ask_decision(ip, tok, tool, timeout=26):
    """Show an Allow/Deny prompt on the device for a pending tool call, then poll
    for the tap. Returns "allow"/"deny", or "" on timeout/unreachable so the
    caller FAILS OPEN to Claude's normal permission prompt."""
    try:
        req = urllib.request.Request(
            "http://%s/ask" % ip,
            data=json.dumps({"tool": tool}).encode("utf-8"),
            headers={"Content-Type": "application/json", "X-Buddy-Token": tok},
            method="POST",
        )
        _opener.open(req, timeout=4).read()
    except Exception:
        return ""  # device unreachable -> normal prompt
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            req = urllib.request.Request("http://%s/decision" % ip,
                                         headers={"X-Buddy-Token": tok})
            r = json.loads(_opener.open(req, timeout=3).read().decode("utf-8"))
            if r.get("decision") in ("allow", "deny"):
                return r["decision"]
        except Exception:
            pass
        time.sleep(0.4)
    return ""


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

    # PermissionRequest: synchronous on-device approval. Output the user's tap as
    # a permission decision; fail OPEN (no output -> normal prompt) on timeout or
    # an unreachable device. Skips the stats/animation path below.
    if evt == "PermissionRequest":
        d = _ask_decision(ip, tok, data.get("tool_name", "this tool"))
        if d:
            # PermissionRequest contract: decision.behavior is "allow"|"block".
            print(json.dumps({"hookSpecificOutput": {
                "hookEventName": "PermissionRequest",
                "decision": {
                    "behavior": "allow" if d == "allow" else "block",
                    "message": ("Approved" if d == "allow" else "Denied") +
                               " on the Claude Buddy device",
                },
            }}))
        return 0

    extra = {"project": _project(data)}
    stats = _today_stats(data)
    if stats:
        extra.update(stats)

    # Map the live tool to an activity clip the device shows while running. The
    # device animates `act` (typing/building/thinking/juggling); empty -> its own
    # random busy carousel. `fx` is a one-shot reaction (error/notification/...).
    # NOTE: the device resets running=0 on any event missing the field, so EVERY
    # event sends running explicitly.
    TOOL_ACT = {
        "Edit": "typing", "Write": "typing", "MultiEdit": "typing",
        "NotebookEdit": "typing", "Bash": "building", "BashOutput": "building",
        "KillShell": "building", "Read": "reading", "Grep": "reading",
        "Glob": "reading", "WebFetch": "thinking", "WebSearch": "thinking",
        "Task": "juggling",
    }
    act = fx = ""
    if evt in ("PreToolUse", "PostToolUse"):
        running, total = 1, 1
        act = TOOL_ACT.get(data.get("tool_name", ""), "")
        msg = act or "working"
        if evt == "PostToolUse":
            tr = data.get("tool_response")
            if isinstance(tr, dict) and (tr.get("is_error") or tr.get("error")):
                fx = "error"  # a tool failed -> brief wince (running stays 1)
    elif evt == "UserPromptSubmit":
        running, total, act, msg = 1, 1, "thinking", "thinking"
    elif evt == "SessionStart":
        running, total, msg, fx = 0, 1, "session started", "heart"
    elif evt == "Stop":
        running, total, msg, fx = 0, 1, "done", "celebrate"
    elif evt == "PreCompact":
        running, total, msg, fx = 1, 1, "compacting", "sweeping"
    elif evt == "Notification":
        running, total, msg, fx = 0, 1, str(data.get("notification", "notice")), "notification"
    elif evt == "SessionEnd":
        running, total, msg = 0, 0, "bye"
    else:
        running, total, msg = 0, 1, evt

    # waiting = Claude has handed the turn back to you (finished, or asking) and
    # nothing is running -> the device escalates a "your turn" nudge over time.
    waiting = evt in ("Stop", "Notification")
    burst, agents = _intensity(evt, data.get("tool_name", ""))

    try:
        payload = dict(extra, total=total, running=running, msg=msg[:24],
                       waiting=waiting, burst=burst, agents=agents)
        bud = _budget()
        if bud:
            payload["budget"] = bud
        if act:
            payload["act"] = act
        if fx:
            payload["fx"] = fx
        _post_event(ip, tok, payload)
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
