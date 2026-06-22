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
Replace `<repo>` with the absolute path to this checkout. `PreToolUse`/
`PostToolUse` make the activity + tool counter update live on every tool call.

## 3. What it sends

Per event the helper pushes the current activity (a rotating whimsical verb
while busy) plus today's usage rollup read from the session transcript: tokens
(today + all-time), tool calls, assistant turns, and session count. Today's
counts persist in `~/.claude/buddy_tokens.json` and reset at local midnight.

## 4. Using it from another computer

Any machine running Claude Code can drive the buddy, as long as:

1. **It can reach the device over the network.** The device serves HTTP on the
   LAN, so the easiest case is "same WiFi". To drive it from a machine that is
   off the home network, expose the device's address over a mesh VPN
   (e.g. Tailscale with a subnet router advertising the device's LAN subnet) and
   point `buddy.json` at whatever address that machine can reach.
2. **The helper + config are installed there:** copy `tools/buddy_hook.py`
   (or clone this repo), create `~/.claude/buddy.json`, and add the hooks above
   to that machine's `~/.claude/settings.json`. Python 3 must be on `PATH`.

Note: each machine keeps its **own** `buddy_tokens.json`, so today/all-time
counts are per-machine, not merged. If two machines push at once, the device
shows whichever pushed last.

## 5. Behaviour / safety

- Device unreachable or slow → the hook swallows the error and returns
  immediately; it never blocks or breaks a session.
- The helper bypasses the system HTTP proxy (the buddy is on the LAN).
- The token is a shared secret; treat `buddy.json` as private (it is not part of
  this repo).
