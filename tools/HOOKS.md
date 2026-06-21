# CYD Buddy — Claude Code hooks setup

The buddy is driven entirely by Claude Code **hooks** (no official Hardware
Buddy / BLE feature needed). A tiny helper (`buddy_hook.py`) bridges hook events
to the CYD over the LAN (HTTP + `X-Buddy-Token`).

## 1. Config (holds the device address + secret; not committed)

`~/.claude/buddy.json`:
```json
{ "ip": "192.168.x.x", "token": "<your-token>" }
```
Update `ip` if the device's DHCP address changes (shown on the CYD screen).

## 2. Hooks (`settings.json`)

Two groups:

- **Status** (non-blocking): buddy reflects what Claude is doing. Safe to enable
  globally in `~/.claude/settings.json`.
- **Approve-from-device** (blocking): `PreToolUse` shows the tool on the CYD and
  waits for a tap, returning allow/deny. Scope it with a `matcher` so only the
  tools you choose route through the device. NOTE: whatever scope you enable also
  affects the Claude Code session you're currently in.

```json
{
  "hooks": {
    "SessionStart":     [{ "hooks": [{ "type": "command", "command": "python \"<HOME>/projects/claude-buddy-cyd/tools/buddy_hook.py\"" }] }],
    "UserPromptSubmit": [{ "hooks": [{ "type": "command", "command": "python \"<HOME>/projects/claude-buddy-cyd/tools/buddy_hook.py\"" }] }],
    "Stop":             [{ "hooks": [{ "type": "command", "command": "python \"<HOME>/projects/claude-buddy-cyd/tools/buddy_hook.py\"" }] }],
    "SessionEnd":       [{ "hooks": [{ "type": "command", "command": "python \"<HOME>/projects/claude-buddy-cyd/tools/buddy_hook.py\"" }] }],
    "Notification":     [{ "hooks": [{ "type": "command", "command": "python \"<HOME>/projects/claude-buddy-cyd/tools/buddy_hook.py\"" }] }],
    "PreToolUse":       [{ "matcher": "Bash", "hooks": [{ "type": "command", "command": "python \"<HOME>/projects/claude-buddy-cyd/tools/buddy_hook.py\"", "timeout": 310 }] }]
  }
}
```

- `matcher: "Bash"` gates only Bash; widen to `"Bash|Edit|Write"` etc. as desired.
- `timeout: 310` gives you ~5 min to tap before the hook gives up (it then defers
  to Claude's normal permission flow — fail-open).

## 3. Behaviour / safety
- Device unreachable or no tap in time → hook exits 0 with no decision → Claude
  Code falls back to its normal permission prompt. The buddy never blocks or
  breaks a session.
- The helper bypasses the system HTTP proxy (the buddy is on the LAN).
