# UI spec — sourced from Minecraft Java 26.3

Every number here comes from the user's own client JAR. Anything not measured is marked `TODO(source)`.
**Rule:** if a value is not in this file and not read from the JAR at runtime, it must not be hardcoded.

Source JAR: `%APPDATA%\Migurinth\meta\versions\26.3\26.3.jar`

## 1. GUI sprite system (the thing a clone skips)

- 26.3 ships **469 GUI sprites** under `assets/minecraft/textures/gui/sprites/**`.
- Each sprite is a PNG plus an optional `.mcmeta` that declares its nine-slice geometry:

```json
{ "gui": { "scaling": { "type": "nine_slice", "width": 200, "height": 20, "border": 3 } } }
```

- `border` is either one integer (all sides) or `{left, top, right, bottom}`.
- Renderer contract: sample the sprite, stretch the centre, keep the borders at their declared pixel size,
  scaled by the GUI scale factor. No hand-drawn approximations.

## 2. Measured widget geometry (26.3)

| Sprite | PNG size | Nine-slice |
|---|---|---|
| `widget/button` | 200 × 20 | border 3 |
| `widget/button_highlighted` | 200 × 20 | border 3 |
| `widget/button_disabled` | 200 × 20 | border 3 |
| `widget/slider` | 200 × 20 | (see its `.mcmeta`) |
| `widget/slider_handle` | 8 × 20 | sprite |
| `widget/text_field` | 200 × 20 | border 1 |
| `widget/text_field_highlighted` | 200 × 20 | border 1 |
| `widget/tab` | 130 × 24 | (see `.mcmeta`) |
| `widget/tab_highlighted` | 130 × 24 | (see `.mcmeta`) |
| `widget/tab_selected` | 130 × 24 | border l2 t2 r2 b0 |
| `widget/checkbox` | 20 × 20 | sprite |
| `widget/checkbox_selected` | 20 × 20 | sprite |
| `widget/cross_button` | 14 × 14 | sprite |

Consequences for us:

- Buttons are **200 × 20 logical px** (not arbitrary); rows are spaced by vanilla's layout, not by eye.
- Tabs are **130 × 24** with an asymmetric nine-slice (no bottom border) so they sink into the panel.
- Text fields are **200 × 20** with a 1 px border — the current fixed insets must come from the sprite.
- Checkboxes are **20 × 20**; the cross button is **14 × 14**.

## 3. Font metrics

- Fonts are **provider lists**, not one bitmap:
  `assets/minecraft/font/default.json` references `include/space`, `include/default` (filter `uniform:false`)
  and `include/unifont`.
- `include/default.json` uses `bitmap` providers with `ascent: 7` (e.g. `font/nonlatin_european.png`) plus the
  ASCII glyph sheet and per-glyph size data.

Rules:

- Glyph advance comes from the font data, never from a fixed per-character constant.
- Shadow is drawn at **+1 px** offset in the vanilla shadow colour, not a generic black.
- `TODO(source)`: extract `glyph_sizes.bin` advances and the ASCII bitmap sheet dimensions into a table the
  renderer can consume; record the measured values here.

## 4. Tooltip

Current code draws a black box tinted `(0.1, 0.05, 0.2)`. That is an approximation.

- `TODO(source)`: locate the tooltip background/border sprites and their colours, and the vanilla
  background gradient; replace the hand-tuned box.
- Tooltip text uses the normal font with shadow; wrap width and cursor offset must match vanilla.

## 5. Menu background / blur

- `TODO(source)`: 26.3 menu background uses the blurred world/panorama; identify the blur parameters and
  the fade-in timing, then implement it as a render pass, not a static tint.

## 6. Acceptance for the UI module

1. Every widget rect comes from a sprite + `.mcmeta`, never a literal size in code.
2. Font advance, ascent and shadow offset come from font providers.
3. A probe in `selftest` prints the loaded sprite count and font table size with a stable hash, and fails if it
   drops below the JAR's counts.
4. The user confirms the visual result on the target laptop; CI proves the data.
