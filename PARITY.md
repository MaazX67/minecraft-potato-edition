# PARITY.md — Potato Edition parity dashboard

Acceptance bar: **one-to-one parity with Minecraft Java 26.3**. A score is evidence, not opinion: it moves only when
a harness covers the area and passes.

Scores: `0` absent · `0.25` started · `0.5` playable, major features missing · `0.75` mostly complete · `1` harnessed and passing.

## Vanilla 26.3 surface (measured)

Generated from the user's own client JAR with `tools/parity/ledger.ps1` (11,383 classes, 98,739 methods).
The ledger JSON is derived from Mojang's jar and stays **local/gitignored**; only counts are committed.

| Area | Vanilla classes | Vanilla methods | Our module | Score | Next harness |
| --- | ---: | ---: | --- | ---: | --- |
| core | 4,059 | 31,637 | `engine.*` | 0.25 | math/random probes (Java-exact) |
| entities | 1,618 | 17,935 | — | 0 | — |
| network | 1,328 | 12,798 | — | 0 | — |
| render | 1,153 | 7,971 | `renderer.*`, `shader.fx` | 0.5 | section-mesh hash |
| ui | 794 | 6,928 | `gui.*` | 0.5 | menu diff |
| blocks | 650 | 6,879 | `world.*` | 0.25 | block-state / tick diff |
| worldgen | 747 | 6,283 | `world.*` | 0.25 | **chunk hash vs vanilla** |
| world-data | 454 | 4,012 | `world.*`, `assets.*` | 0.25 | NBT/region round-trip |
| commands-chat | 295 | 2,254 | `commands.*` | 0.5 | command-table diff |
| assets | 249 | 1,778 | `assets.*`, `registry.*` | 0.25 | **full registry load** |
| audio | 36 | 264 | `sound.*` | 0.25 | sound-event diff |
| **total** | **11,383** | **98,739** | `src/` | — | — |

Scores are **seed values** from the v2.0.0 state; each is replaced by a harness result in Phase 1–4.

## Current code (what the strangler drains)

| Files | Lines |
| --- | ---: |
| `main.cpp` (app loop + screens + input) | 3,926 |
| `selftest.cpp` (harness home) | 1,950 |
| `gui.cpp/.h` | 1,833 |
| `world.cpp/.h` | 1,731 |
| `engine.cpp/.h` | 1,395 |
| `assets.cpp/.h` + `registry.cpp/.h` | 1,348 |
| `renderer.cpp/.h` + `shader.fx` | 704 |
| `sound.cpp/.h` | 442 |
| `commands.cpp/.h` | 441 |
| `shader_embed.h.in` | 3 |
| **total** | **13,773** |

## Registry gap (Phase 1 target)

| Registry | Now | Vanilla 26.3 |
| --- | ---: | ---: |
| Blocks | 136 | full JAR set |
| Items | 66 | full JAR set |
| Recipes | 64 | full JAR set |
| Loot tables | partial | full JAR set |

Exit criteria: every 26.3 block, item, recipe and loot table is loaded from the JAR/index at runtime, with **zero
hardcoded subsets**, proven by a probe that counts and hashes the loaded registry.

## How the gate works

- Probes live in `PotatoSelfTest.exe`. CI already runs that binary, so parity failures fail the build without any
  `.github/workflows` change (the token has no `workflow` scope).
- Golden values are hashes/counts committed to the repo. Oracle runs (vanilla reference) happen locally against the
  user's install using the launcher's bundled JRE at
  `%APPDATA%\Migurinth\meta\java_versions\zulu25.38.109-ca-jre25.0.5.0.3-win_x64`.
- Never commit Mojang code, assets, decompiled source, or the generated ledger.

## Update procedure

1. Run `pwsh -File tools/parity/ledger.ps1` (writes local `ledger.json`, prints the area table).
2. Change a score **only** when the area's harness is green, and cite the harness in the commit message.
3. Add the new gap counts from the probe output.
