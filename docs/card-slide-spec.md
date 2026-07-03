# Bottom-card slide transition — design spec

Status: approved. Date: 2026-07-03.

## Problem

The bottom card's two pages (0 = stats, 1 = trends) currently toggle on any
mostly-horizontal swipe (`setCard(card() ^ 1)` in main.cpp) with an instant
full repaint. The user wants (a) real slide feel, and (b) fixed spatial
semantics instead of either-direction toggling.

## Behavior

- Pages have fixed positions: **stats on the left, trends on the right**.
- On stats, swipe left (finger travels right→left, dx < 0) slides to trends.
  On trends, swipe right (dx > 0) slides back to stats.
- Wrong-direction swipe = **rubber-band bounce**: the card follows a small
  nudge (~24 px) in the swiped direction and eases back, signaling "end of
  row". Reuses the same animation path with a short amplitude.
- Existing rules unchanged: swipe thresholds (SWIPE_MIN_DX/MAX_DY/MAX_MS),
  screen-off returns to page 0, attention/needs-you screen has no card.

## Animation

- Release-triggered, ~250 ms ease-out (house style: display glide, state
  bounce-in). No finger-tracking drag — resistive-panel jitter makes
  continuous tracking look rough.
- Render both pages into 4-bit palette sprites of the card region
  (240 × card-height @ 4 bpp ≈ 17 KB each; a full 16 bpp card sprite would
  blow the ~47 KB max alloc). The UI uses well under 16 colors; the palette
  is built from theme.h + chart colors.
- Each frame pushes both sprites at x and x±W with eased offset. Character
  GIF above the card pauses for the ~250 ms (imperceptible during a gesture).
- Requires card renderers (stats grid, trends chart, headline) to draw onto a
  passed `TFT_eSPI&` canvas instead of the global tft — mechanical refactor,
  no visual change when the canvas is the screen itself.

## Testing

1. On-device: swipe left/right on both pages — correct pages slide in from
   the correct side; wrong direction bounces; thresholds still reject slow or
   diagonal drags; pet/tap gestures unaffected.
2. Heap check: `[character] freeHeap` stays comfortably positive after two
   sprite allocations (allocate → animate → free, never held across frames).
3. Screen-off → wake returns to stats page with no animation.
