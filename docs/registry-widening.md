# 16-bit item/block ids (step 1.1)

Step 1.1 removes the 8-bit ceilings that would have blocked a full 26.3 registry. Nothing about the game's
behaviour changes yet: the registry still holds the same 138 blocks and 72 items, but every id that crosses a
module boundary is now 16-bit, so step 1.2 can load the whole JAR without re-cutting the interfaces again.

## What changed

| Layer | Before | After |
| --- | --- | --- |
| `enum BlockId`, `enum ItemId` | `uint8_t` | `uint16_t` |
| Item/block APIs (`FindTool`, `ItemName`, `ItemTexture`, `ItemStackLimit`, `LoadItemRGBA`, `CanHarvest`, `BlockDrops`, `MiningSeconds`, `BlockHardness`, `ItemCreativeTab`, `CreativeTabIcon`, `ItemMatchesSearch`, `ItemIdFromResourceName`, `ResourceNameForItem`, `RecipeCellMatches`, `LootDropForBlock`) | `uint8_t` ids | `uint16_t` ids |
| `ItemSlot::id`, hotbar arrays, `World::GetBlock/SetBlock`, mesh/raycast callbacks | `uint8_t` | `uint16_t` |
| `ChunkEdits` values, `PalettedSection` palette/uniform, chunk scratch buffers | `uint8_t` | `uint16_t` |
| Palette index tiers | 2/4/8 bits | 2/4/8/**16** bits |
| Edits blob (`PTED`) | v1, 8-bit block id | **v2**, 16-bit block id |
| World save `format` | 3 | **4** |
| Recipe tag base | `0x100` | `0x8000` |

### Why the palette grew a 16-bit tier

A 16×16×16 section has 4,096 cells, so a mixed section can legitimately hold up to 4,096 distinct ids. The old code
gave up (`if (mBits >= 8) return;`) and silently dropped writes once a section used more than 256 states. Section 1.2
will push the registry well past 256 blocks in a single chunk far more easily than people expect (deepslate variants,
copper oxidation, leaves, flowers). The new ladder is 2 → 4 → 8 → 16 bits, and the last tier is only reached when
the section really needs it (≈8 KB indices + 2 bytes per palette entry).

### Compatibility

- Saves with `format` 1–4 still load. The `+96` item remap for formats < 3 is unchanged.
- The edits blob decoder accepts v1 (3-byte entries, 8-bit block id) and v2 (4-byte entries, 16-bit block id).
  New saves always write v2.
- Hotbar arrays were stored as msgpack integers; `get<std::vector<uint16_t>>()` reads old 8-bit values unchanged.
- `PARITY-REGISTRY` now hashes `name\0id-lo\0id-hi`, so any mapping change, including a widened id, changes the
  hash. The Step 0 8-bit hash is therefore retired; re-baseline from the first `--parity` run.

## Ceilings that remain (deliberate, with the step that removes them)

1. **Item icon arrays (`texItemIcon[256]`, `gItemPixels[256]`, `gItemHasPixels[256]`).** Still 256 entries; every
   lookup is guarded (`id >= 256` is skipped) so ids above 255 have no icon yet but cannot read out of bounds.
   Removed by the **dynamic item atlas in step 1.3**.
2. **`BlockTiles` (`top/side/bottom[256]`).** Block-atlas lookup is still an 8-bit table. Safe today because
   `BLOCK_COUNT == 138`, but it must be widened before the block registry passes 256 (step 1.2/1.3, together with
   the dynamic atlas).
3. **Recipe tag space (`TAG_* = 0x8000…`).** Recipe cells keep the top half of `uint16_t` for tags, so item ids
   `>= 0x8000` would collide. Vanilla 26.3 has ~1,500 entries, three orders of magnitude of headroom; revisit only if
   a future registry makes this real.
4. **World save revision.** The block-edit blob is v2, but the overall save `format` moves from 3 to 4. Older
   binaries cannot read the new save; new binaries read both.

## Probes

`PotatoSelfTest.exe` gains three checks that would have failed on the 8-bit build:

- `16-bit palette tier round-trips 300 distinct ids in one section` — crosses the old 256-state ceiling.
- `version 2 edit entries store 16-bit block ids` — pins the 4-byte entry size.
- `version 1 edit blobs upconvert to the 16-bit format` — hand-built v1 blob decodes correctly.

`PotatoEdition.exe --parity` prints the same `PARITY-REGISTRY` line as the selftest and writes `parity.txt`, so the
user's machine produces the real JAR numbers without a compiler. CI prints `jar=no` (no JAR on the runner).
