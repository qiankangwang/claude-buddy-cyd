# Spec — new "busy" animation clips for the Clawd pack

Hand this to a pixel-art AI image/video tool **or** a human pixel artist to
produce additional `busy`-state clips for the CYD Claude Buddy. The goal is new
*motions of the Clawd character itself* (Clawd actively "working"), matching the
existing pack so they drop in seamlessly.

## Reference (match this exactly)

The canonical style reference is the existing pack in `data/clawd/` — open
`busy_0.gif`, `busy_1.gif`, `busy_2.gif` and match their character design,
proportions, palette, line weight, and "feel". **Clawd** is an orange pixel-art
crab mascot (Claude + claw). Do not redesign the character; reuse its look.

## Hard technical requirements (the device renderer depends on these)

| Property | Value |
|---|---|
| Format | Animated **GIF** |
| Canvas width | **120 px** (match the pack; height may vary ~80–160 px) |
| Background | **solid black `#000000`**, opaque, edge-to-edge |
| Frames | **full-frame every frame** — NO partial/diff frames, disposal = "restore to background" / replace |
| Interlacing | **off** |
| Palette | small, **single global palette**; no smooth gradients/anti-alias smear (crisp pixels — nearest-neighbour friendly) |
| Frame count | ~34–48 frames |
| Frame delay | ~60–70 ms/frame (≈ a 2.5–3 s loop) |
| Loop | infinite (`loop=0`) |
| Seamless | last frame must flow back into the first (clean loop) |

Why these matter: the device decodes one scanline at a time with no
framebuffer, so each frame must be complete on a black background; it scales the
120 px art up to fill a ~224 px region, so keep art crisp and centered with a
little headroom for motion.

## Palette

- Body / accent: **`#D97757`** (Anthropic coral — Clawd's body)
- Background: **`#000000`**
- Highlights / eyes: **`#FFFFFF`**
- Dim details / shadow: **`~#808080`**
- A few extra shades of the coral for shading are fine; keep the total color
  count low (≤ ~32).

## Motion ideas (pick a few; "busy / working" mood, looping)

Each clip = Clawd doing one looping action. Good options:

- **Typing** on a tiny laptop/keyboard (claws tapping, screen flicker).
- **Hammering / building** something small.
- **Stirring a pot** ("cooking up" a result), little steam puffs.
- **Reading** a scroll/tablet, eyes scanning, occasional page flip.
- **Juggling / spinning** a small tool or gear between claws.
- **Pacing + finger-tap** thinking, foot tapping.
- **Cranking a lever / winding a key** on its own back.

Keep the whole character on-frame, centered, bobbing naturally; avoid text the
device can't resolve at small size.

## Deliverable & integration

- Provide each clip as a GIF named `busy_5.gif`, `busy_6.gif`, … (continuing
  from the current `busy_0..4`).
- Drop them in `data/clawd/` and add the filenames to the `busy` array in
  `data/clawd/manifest.json`:
  ```json
  "busy": ["busy_0.gif","busy_1.gif","busy_2.gif","busy_3.gif","busy_4.gif","busy_5.gif"]
  ```
- Total pack must stay **≤ ~2.0 MB** (LittleFS partition). Current pack ≈ 1.7 MB,
  so budget ~100–130 KB per new clip (a 120 px, ~45-frame, low-palette GIF lands
  there comfortably).

## Ready-to-paste prompt for an AI image/pixel tool

> Pixel-art animation, 120×118 px, transparent-style on a solid black
> background. Character: an orange (#D97757) crab mascot named "Clawd" — match
> the attached reference frames exactly (same proportions, eyes, palette, crisp
> pixel style, no anti-aliasing). Animate a smooth, seamless ~45-frame loop of
> Clawd **[typing on a tiny laptop]**, centered, gentle bob, ~70 ms per frame.
> Limited palette (coral body, black bg, white highlights, grey shadow). Output
> an animated GIF, non-interlaced, full frames, loop forever.

Attach `busy_0.gif` (and `idle.gif`) as the style/character reference for
img2img or a reference-image workflow — that anchors the look far better than a
text description alone.

## For a human artist (Aseprite)

- New sprite 120 px wide, black background layer, indexed palette seeded from
  `busy_0.gif`. Animate ~45 frames at 70 ms. Export: File → Export Animation →
  GIF, "no dithering", whole-frame, loop forever. Send the `.gif`(s) back and
  they drop straight into `data/clawd/`.

Once you have the GIF(s), hand them back and they'll be added to the pack,
flashed to LittleFS, and they'll join the random busy-clip rotation.
