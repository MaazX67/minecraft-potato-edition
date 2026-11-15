# M1 data core — step 1.3: full 26.3 registry + one shared atlas

Status: written, pending first CI compile on `ship`.

## What changed

- `assets.h/.cpp`
  - `REGISTRY_ID_BASE = 0x1000`, `BLOCK_ID_LIMIT = 0x8000`, `BLOCK_TILE_SLOTS = 0x8000`.
  - `Atlas::GetTilePixels` + free `AtlasTileRect` (half-texel inset, point-sampler safe).
  - `ItemIconTile` / `SetItemIconTile` / `BlockFlags` / `IsBlockId` (every engine lookup goes through these, so a jar id can never index past `BLOCK_COUNT`).
  - `ResolveBlockModel` returns `fullCube` (one `[0,0,0]→[16,16,16]` element) and `cutout` (leaves/glass/ice, `force_translucent`, `render_type` cutout/translucent).
  - `BuildBlockAtlas` now loads **both** `textures/block/` and `textures/item/` into one 1024×1024 atlas.
  - `BindItemIconTiles` points every valid id at its atlas tile; `ItemTexture` prefers the model-resolved side texture.
- `registry.h/.cpp`
  - `RegisterRuntimeRegistry` scans `blockstates/` + `items/`, assigns append-only ids from 0x1000 in sorted-name order. Only full cubes get flags and become placeable; slabs, stairs, doors, torches stay inventory-only.
  - `REGISTRY_BLOCK_FLAGS` filled per cube; loot guard widened to `BLOCK_ID_LIMIT`; aliases `grass_block`/`magma_block`.
  - `PARITY-REGISTRY` line now reports `cubes=N`.
- `world.h/.cpp`
  - `BlockTiles` is now three `std::vector<int>` of `BLOCK_TILE_SLOTS` (heap, no 400 KB stack objects).
  - `FaceTile` guards ids ≥ `BLOCK_TILE_SLOTS`.
  - `BuildExtrudedItemMesh` takes an atlas UV rect so the held item samples the shared atlas.
- `gui.h/.cpp`
  - `texItemIcon[256]` → `texItemAtlas`; `DrawItemIcon`/`DrawScaledIcon` use `ItemIconTile` + `AtlasTileRect`; the 256-icon load/release loops removed (atlas aliases `gRC.atlasSRV`, so `Shutdown` must not release it).
- `main.cpp`
  - `gItemPixels`/`gItemHasPixels` removed; boot sets registry tiles + binds icon tiles + points `gGui.texItemAtlas` at the atlas SRV.
  - held item pulled from the atlas; placement guard uses `IsBlockId`; automation probe reports `iconsBound` + `atlasTiles`.
- `selftest.cpp`
  - `BLOCK_FLAGS[...]` → `BlockFlags(...)`; added `cubes=`, determinism and reset seam checks in `TestRegistryParity`.

## Expected parity line (user's machine)

`PARITY-REGISTRY: jar=yes blocks=1288 items=1658 cubes=<~1200> recipes=... loot=... names=... dup=0 hash=...`

## Not done in this step

- Non-cube models (slabs/stairs/torches/doors) remain inventory-only; their meshes land in M4.
- Chest stays an oak-planks cube (decided in 1.2).
- M2 UI replica work follows.