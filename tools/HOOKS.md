# CYD Buddy — Claude Code hooks setup

The buddy is driven entirely by Claude Code **hooks** (no official Hardware
Buddy / BLE feature needed). A tiny helper (`buddy_hook.py`) bridges hook events
to the CYD over the LAN (HTTP + `X-Buddy-Token`). The device is a **passive
stats dashboard** — it shows what Claude is doing and today's usage; there is no
on-device approval, so every hook is non-blocking and never affects a session.

## 1. Config (device address + secret; NOT committed)

`~/.claude/buddy.json`:
```json
{ "ip": "192.168.x.x", "token": "<the token shown on the device>" }
```
The CYD shows its current `IP` and `token` (long-press → **Settings → Stats**).
Update `ip` if the device's DHCP address changes.

## 2. Hooks (`~/.claude/settings.json`)

All events are non-blocking (`async: true`), so they never slow Claude. Add the
buddy helper to the events you want the device to react to:

```json
{
  "hooks": {
    "SessionStart":     [{ "hooks": [{ "type": "command", "command": "python \"<repo>/tools/buddy_hook.py\"", "async": true, "timeout": 10 }] }],
    "UserPromptSubmit": [{ "hooks": [{ "type": "command", "command": "python \"<repo>/tools/buddy_hook.py\"", "async": true, "timeout": 10 }] }],
    "PreToolUse":       [{ "hooks": [{ "type": "command", "command": "python \"<repo>/tools/buddy_hook.py\"", "async": true, "timeout": 10 }] }],
    "PostToolUse":      [{ "hooks": [{ "type": "command", "command": "python \"<repo>/tools/buddy_hook.py\"", "async": true, "timeout": 10 }] }],
    "Stop":             [{ "hooks": [{ "type": "command", "command": "python \"<repo>/tools/buddy_hook.py\"", "async": true, "timeout": 10 }] }],
    "SessionEnd":       [{ "hooks": [{ "type": "command", "command": "python \"<repo>/tools/buddy_hook.py\"", "async": true, "timeout": 10 }] }],
    "Notification":     [{ "hooks": [{ "type": "command", "command": "python \"<repo>/tools/buddy_hook.py\"", "async": true, "timeout": 10 }] }]
  }
}
```
Replace `<repo>` with the absolute path to this checkout — **or**, for a setup
that doesn't depend on the repo, copy the single file `buddy_hook.py` to
`~/.claude/buddy_hook.py` and point the command there (see §4). `PreToolUse`/
`PostToolUse` make the activity + tool counter update live on every tool call.

## 3. What it sends

Per event the helper pushes the current activity (a rotating whimsical verb
while busy) plus today's usage rollup read from the session transcript: tokens
(today + all-time), tool calls, assistant turns, and session count. Today's
counts persist in `~/.claude/buddy_tokens.json` and reset at local midnight.

## 4. Use it from another computer (no repo needed)

The flashed device is **fully standalone** — firmware and the animation pack live
in its own flash, so it needs no PC, no repo, and no cloud; it just boots, joins
your WiFi, and waits for events. To drive it from a machine that **doesn't have
this repository**, the only thing that machine needs is the **single** helper
file: `buddy_hook.py` depends on nothing but the Python 3 standard library and
reads only `~/.claude/buddy.json` and `~/.claude/buddy_tokens.json` (never the
repo). So it's just:

1. **Copy one file** — put `buddy_hook.py` on that machine; a good
   repo-independent home is `~/.claude/buddy_hook.py`.
2. **Config** — create `~/.claude/buddy.json` with the device `ip` + `token`
   (shown on the device under Settings → Stats).
3. **Hooks** — add the block from §2 to that machine's `~/.claude/settings.json`,
   with the command pointing where you put the file, e.g.
   `python "~/.claude/buddy_hook.py"` (use an absolute path; on Windows
   `%USERPROFILE%\.claude\buddy_hook.py`).

Requirements: **Python 3 on `PATH`**, and the machine must be able to **reach the
device** — same WiFi is simplest; off-network works via a mesh VPN (e.g. Tailscale
with a subnet router advertising the device's LAN subnet), with `buddy.json`
pointed at whatever address that machine can reach. `buddy_tokens.json` is created
automatically on first run.

Each machine keeps its **own** `buddy_tokens.json`, so today/all-time counts are
per-machine, not merged. If two machines push at once, the device shows whichever
pushed last.

## 5. Behaviour / safety

- Device unreachable or slow → the hook swallows the error and returns
  immediately; it never blocks or breaks a session.
- The helper bypasses the system HTTP proxy (the buddy is on the LAN).
- The token is a shared secret; treat `buddy.json` as private (it is not part of
  this repo).
