# Spec — new "busy" animation clips for the Clawd pack

Hand this to a pixel-art AI image/video tool **or** a human pixel artist to
produce additional `busy`-state clips for the CYD Claude Buddy. The goal is new
*motions of the Clawd character itself* (Clawd actively "working"), matching the
existing pack so they drop in seamlessly.

## Reference (match this exactly)

The canonical style reference is the existing pack in `data/clawd/` — open
`idle.gif` and `busy_0.gif` and match them. **Clawd is NOT a crab** — it is a
simple, blocky coral creature.

### Clawd — exact build (from the real frames)

On a 120 px-wide black canvas, Clawd sits low and centered:

- **Body:** one solid coral (`#D97757`) **rounded rectangle, wide and flat**,
  about **78 px wide × 40 px tall** (≈ 2:1), corners softened ~3 px. **Flat fill,
  no gradient/shading**; an optional 1 px lighter top/left edge.
- **Eyes:** two identical black eyes in the upper third, at ~**38%** and ~**62%**
  of the body width. **Resting** = tall thin **vertical bars** (~6×16 px, `▮ ▮`).
  **Busy/concentrating** = short **horizontal dashes** (~10×3 px, `- -`).
- **Arms:** one short coral **stub poking straight out each side** at mid-height
  (~8–12 px). They pose/move per action.
- **Legs:** **four** short coral stubs (~6 px wide × ~12 px tall) hanging from
  the bottom edge, evenly spaced.
- **Absolutely NOT:** no pincers/claws, no eyestalks, no antennae, no spider
  legs, no mouth/nose. It is a wide coral block with slot/dash eyes, two side-arm
  stubs, and four little legs.
- **Thought bubble (on-model):** the official "thinking" clip puts a white
  pixel **thought-bubble above the head with 3 small blue dots** that cycle.

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

## Ready-to-paste prompt (character + one action)

> Crisp pixel art, 120×118 px, hard edges, no anti-aliasing, on a solid black
> background. **Character "Clawd"** — a simple blocky coral creature, NOT a crab:
> a single solid coral `#D97757` **wide flat rounded rectangle** body (~78×40 px,
> ~2:1), flat fill no shading. **Two eyes** in the upper third at ~38% and ~62%
> width — short black **horizontal dashes** when working. **One short coral
> stub-arm out each side**; **four short coral stub-legs** hanging from the
> bottom. No pincers, no eyestalks, no antennae, no mouth. Centered, sitting low.
> Animate a smooth, seamless **~45-frame** loop @ ~70 ms of Clawd **[ACTION]**,
> gentle body bob. Palette: coral body, black bg+eyes, white highlights, one
> muted blue. Output an **animated GIF**, non-interlaced, full frames, loop
> forever.

Replace `[ACTION]` with one of the actions below. **Also attach `idle.gif` +
`busy_0.gif` as reference images** (img2img / reference workflow) — text plus the
real frames anchors the look far better than text alone.

## Per-action motion (each its own clip)

- **Typing** — a small dark laptop (`#1E1E2E` screen w/ flickering green code,
  teal keys) in front; the two side stub-arms alternate tapping down on the keys;
  eyes are horizontal dashes.
- **Thinking** (the canonical busy look) — a white pixel **thought-bubble above
  the head with 3 muted-blue dots** that cycle/pulse; arms slightly raised; calm
  bob.
- **Stirring a pot** — a small dark pot in front; one stub-arm holds a spoon and
  rotates it in a circle; a few steam pixels rise; body leans with the motion.
- **Hammering** — one stub-arm raised holding a small mallet, swings down onto a
  peg and the body recoils a little on each hit; the other arm braces.

Key correction for any tool that keeps drawing it wrong: **Clawd is a wide flat
coral BLOCK with slot/dash eyes, two side-arm stubs and four legs — not a crab,
not a tall blob, not a bug.** Arms and legs must visibly move for the action.

## For a human artist (Aseprite)

- New sprite 120 px wide, black background layer, indexed palette seeded from
  `busy_0.gif`. Animate ~45 frames at 70 ms. Export: File → Export Animation →
  GIF, "no dithering", whole-frame, loop forever. Send the `.gif`(s) back and
  they drop straight into `data/clawd/`.

Once you have the GIF(s), hand them back and they'll be added to the pack,
flashed to LittleFS, and they'll join the random busy-clip rotation.
