# AGENTS.md — Potato Edition

C++ rewrite of **Minecraft: Java Edition 26.3** with one-to-one vanilla parity ("same to same", not "similar"),
targeting **i3-M380 / 4 GB RAM / first-gen Intel HD (DirectX 10 era)**.

- Repo: `MaazX67/minecraft-potato-edition` · working branch `ship`
- Target: **DirectX 10 `FL10_0`** only. Never resurrect the old OpenGL path.
- Reference project for method: [Horizon](https://github.com/horizon-rust/horizon) (Java 26.3 rewritten in Rust).

## Who you work with

The user is the product owner, not a reviewer of code:

- Give **working game features** in testable chunks; they test each build on their own laptop, then dictate the next.
- **Simple English**, brief status at every pivot. Hinglish is fine. No lectures.
- **Never idle-wait** when a workaround exists — pivot and say so in one sentence.
- They care about exact vanilla behaviour: menus, textures, keys, combat, inventory, terrain.

## Hard constraints

1. **No Mojang code or assets in the repo.** Runtime reads the user's install:
   - JAR: `%APPDATA%\Migurinth\meta\versions\<ver>\<ver>.jar`
   - Assets: `%APPDATA%\Migurinth\meta\assets\{indexes\34.json,objects\*}`
2. **No local compiler and no GPU on this machine.** All C++ compiles in GitHub Actions (MSVC x64). Visual checks
   (water, held items, lighting) are the user's job.
3. **Full releases only**, new tags per version, older tags preserved.
4. Never expose, print, or commit the GitHub token or credential files.

## How we work: the parity factory

Stolen from Horizon, adapted to C++/DX10/no-workflow-scope:

| Mechanism | Ours |
| --- | --- |
| Pinned version | 26.3, one-to-one parity is the acceptance bar |
| Differential harnesses | probes inside `PotatoSelfTest.exe`; golden hashes committed, oracle data generated locally |
| Port ledger | `tools/parity/ledger.ps1` → class/method index + area gap counts |
| Parity gate | the existing CI job already runs `PotatoSelfTest.exe`; parity failures fail the build — **no workflow edit needed** |
| Layered architecture | modules below; each module owns its harness |
| Strangler refactor | rewrite one module at a time behind a green harness; the game stays playable |

### Rules every port keeps

- Java-exact math (`Math.floor` floors negatives, never truncate) and Java randoms (Xoroshiro128++/SplitMix64) in
  vanilla's draw order.
- Keep vanilla iteration orders: ticks, entity lists, feature placement, NBT key order where observable.
- Typed errors where Java throws, without changing observable behaviour.
- Data comes from the JAR: no hardcoded block/item/recipe subsets.

## Target module map (strangler destination)

```
src/core/       jmath, random, nbt, palette, datafixers
src/assets/     jar reader, asset index, packs, data-driven registries
src/world/      chunks, sections, lighting, region files, worldgen
src/sim/        blocks/ticking/redstone, entities/physics/AI, inventory/menus, commands
src/render/     dx10 device, section meshing, block/entity models, HUD, text
src/ui/         screens, widgets, fonts
src/audio/      sound engine, music, ambience
src/net/        protocol, integrated server thread, LAN
src/platform/   win32 window/input/timing
src/app/        game loop + screen state machine
```

Current monolith to be drained: `main.cpp` (3,926 lines), `selftest.cpp` (1,950), `gui.cpp` (1,563),
`world.cpp` (1,420), `engine.cpp` (1,019), `assets.cpp` (695), `renderer.cpp` (488), `registry.cpp` (402),
`commands.cpp` (383), `sound.cpp` (363) — 13,773 lines total.

## Roadmap

| Phase | Scope | Exit criteria |
| --- | --- | --- |
| 0 Foundation | ledger, harness skeleton, `PARITY.md`, module split plan | accurate gap table, first probe green in CI |
| 1 Data core | JAR-driven registries, NBT/region/palette, packs | every 26.3 block/item/recipe loads; 0 hardcoded tables |
| 2 Worldgen | seeds, multi-noise, caves/aquifers, ores, structures | chunk hashes match vanilla (N seeds × 3 dimensions) |
| 3 Simulation | ticking/redstone, entities/physics/AI, inventory/menus, commands | tick-diff and menu-diff harnesses green |
| 4 Render/UI/audio | DX10 passes, meshing, models, HUD, sounds | meshing hashes match; user confirms visuals |
| 5 Low-end budget | streaming, culling, memory ceiling, frame floor | playable FPS on the i3/HD at RD 8–12 |

**Current milestone: Phase 1 — full JAR registry.**

## Release pipeline

CI (`build.yml`, `on: push` any branch) compiles MSVC x64, runs `PotatoSelfTest.exe`, uploads the exe artifact, and
**overwrites the fixed tag `v1.0.0-Alpha` on every push**. The token has `repo` scope but **no `workflow` scope**, so
never push changes under `.github/workflows/`.

Before any push that may publish: original `v1.0.0-Alpha` backup lives at
`run/release-backup-v1.0.0/PotatoEdition.exe` (workspace root, **not** inside `remote-repo/`).

Push form that works here (portable git is at `.qoder\bin\git\cmd\git.exe`, not on PATH):

```sh
GIT_TERMINAL_PROMPT=0 .qoder/bin/git/cmd/git.exe \
  -c credential.helper= \
  -c credential.helper='!"C:/Users/Administrator/.qoder/bin/gh/bin/gh.exe" auth git-credential' \
  -c user.name="MaazX67" -c user.email="maazx67@users.noreply.github.com" \
  push origin <branch>
```

Releases are created through the GitHub REST API (find run → download artifact → create release → upload asset),
then `v1.0.0-Alpha` is restored (`DELETE` overwritten asset, re-upload backup, set prerelease). Never force-push.

## Status (2026-11-14)

- **v2.0.0** released (commit `967fcea`): world management, options, key rebinds, tabbed creative inventory,
  drag-swipe, entity shadows, LOD terrain, frame limiter.
- `v1.0.0-Alpha` restored as prerelease from the backup.
- Vanilla 26.3 ledger: **11,383 classes / 98,739 methods** across 11 areas (see `PARITY.md`).
- Biggest gap: registry breadth — 136 blocks / 66 items / 64 recipes vs full vanilla.
