# JAR-driven block textures (M1 step 1.2a)

The first build the user tested (step 1.1) showed **magenta blocks**. Magenta is the
missing-texture checker (`Atlas::AddCheckerTile("__missing")`, tile 0), so a block was
asking for a PNG that does not exist in the 26.3 client jar.

## The bug

`BLOCK_DEFS` hardcoded three texture names per block. Auditing those 411 names against
`assets/minecraft/textures/block/` in the user's own jar found four that do not exist in
26.3:

| Block | Hardcoded face | 26.3 reality |
| --- | --- | --- |
| chest | `chest_top`, `chest_side`, `chest_bottom` | chest is a block entity: `textures/entity/chest/normal.png`; the block model only declares `particle: block/oak_planks` |
| crafting_table | `crafting_table_bottom` | model says `down: block/oak_planks`; the block has `_top`, `_side`, `_front` only |

Every one of those paths fell through `ResolveTile()` to tile 0, painting the block, its
held-item cube and its inventory icon in magenta/black.

## The fix

Texture bindings now come from the jar itself, the same way vanilla resolves them:

```
blockstates/<block>.json          variants / multipart -> model id
models/block/<model>.json         "parent" chain merged, child textures override
textures/<namespace>/<path>.png   the pixels
```

`ResolveBlockTextures()` in `assets.cpp`:

1. reads `blockstates/<block>.json`, preferring the canonical `""` variant, then the first
   `variants` entry, then the first `multipart` `apply`;
2. walks the `parent` chain (depth-limited to 8) merging every `textures` map;
3. resolves `#alias` indirection;
4. picks one texture per cube face using vanilla's own alias vocabulary:
   - top: `up`, `top`, `end`, `all`, `particle`
   - side: `north`, `side`, `front`, `all`, `particle`
   - bottom: `down`, `bottom`, `end`, `all`, `particle`

`particle` is last on purpose: block-entity models such as `block/chest` declare only
that, and vanilla itself uses it as the break-particle material, so a cube drawn from it
is at least the correct material. `BLOCK_DEFS` stays as a jar-less fallback only.

The inventory icon uses the same resolved side texture (`SetBlockItemTexture`), so an
icon and the block in the world can never disagree again.

## Regression gates

- `PotatoEdition.exe --parity` now writes a second line:
  `PARITY-BLOCKTEX: missing=<n> <first 16 paths>`.
  `PotatoSelfTest.exe` prints the same audit when a jar is present and fails CI (`CHECK`)
  on any missing face once the jar is available. CI has no jar, so it prints
  `missing=0` there; the real gate is the user's run.
- Any texture actually referenced during boot but absent from the jar is appended to
  `missingtextures.txt` next to the exe, so a future 26.x rename is named, not silent.

## Still hardcoded (and why)

Block *behaviour* is code in vanilla (hardness, collision, light, block entity type), not
JSON, so the fallback table keeps existing until those properties are ported per block.
The texture side — the part that caused the visible bug — is now fully data-driven.

## Known temporary visual

Chest renders as a solid oak-planks cube and uses a planks icon. Vanilla draws it with a
block-entity model (`entity/chest/normal.png`, lid split, latch). That mesh belongs to the
render phase; it is queued, not approximated with wrong UVs here.
