<div align="center">

```
================================================================================

   __  __ ___ _  _ ___ ___ ___   _   ___ _____
  |  \/  |_ _| \| | __/ __| _ \ /_\ | __|_   _|
  | |\/| || || .` | _| (__|   // _ \| _|  | |
  |_|  |_|___|_|\_|___\___|_|_/_/ \_\_|   |_|

         P O T A T O   E D I T I O N
         DirectX 10  -  Feature Level 10_0  -  70+ FPS

================================================================================
```

A from-scratch, hyper-optimized Minecraft Java Edition clone in C++17 with a raw
DirectX 10 backend — engineered to run on a 2010 potato laptop.

[**Download v1.0.0-Alpha**](https://github.com/MaazX67/minecraft-potato-edition/releases/tag/v1.0.0-Alpha)
·
[CI Builds](https://github.com/MaazX67/minecraft-potato-edition/actions)
·
[Design Spec](docs/superpowers/specs/2026-11-09-minecraft-potato-edition-design.md)

</div>

---

## System Specs Analysis

Every architectural decision maps to a constraint of the target machine:

| Component | Target Hardware | Engine Response |
|---|---|---|
| **CPU** | Intel Core i3-M380 (2C/4T, 2.53 GHz) | Exactly 2 worker threads; hard per-frame slice budgets |
| **GPU** | 1st-Gen Intel HD Graphics (Ironlake, FL 10_0) | Raw D3D10, no D3DX, one point-sampled hardware atlas |
| **RAM** | 4 GB system memory | < 300 MB engine budget (vs 1.5 GB cap); 64 KB flat chunks |
| **OS** | Windows 10, WDDM 1.1 | XAudio2 (inbox), DXGI `DISCARD` swapchain, 1 backbuffer |
| **Display** | 1366 x 768 native | Fixed-size backbuffer, VSync off (`Present(0,0)`) |
| **Goal** | Locked **70+ FPS** | Greedy meshing (~90% vertex cut), frustum culling, validated on hardware in M4 |

---

## Asset Pipeline: `34.json` to Texture Atlas

No Mojang textures are committed to this repository. The engine reads your **own
local Minecraft installation** at boot and stitches a single hardware atlas.

The `Performium` profile (launched via Migurinth) stores its assets in the
launcher's shared meta store. The parser targets the index:

```
C:\Users\Admin\AppData\Roaming\Migurinth\meta\assets\indexes\34.json
```

Boot sequence:

1. **Index parse** — `34.json` maps logical names (`minecraft/textures/block/stone.png`)
   to SHA-1 hashes via nlohmann/json.
2. **Object lookup** — each hash resolves to `objects/<first-2-hex>/<sha1>` in the
   same meta store; raw bytes are read straight from disk.
3. **Decode** — vendored `stb_image` decodes every block PNG to 16x16 RGBA
   (animated strips use frame 0). Grass receives the plains tint `0x91BD59` baked
   at load time.
4. **Atlas stitch** — all tiles pack into one contiguous **1024 x 1024** atlas
   (4096 slots, tile 0 = missing-texture checker) and upload as a single immutable
   D3D10 texture. Greedy-merged quads repeat tiles in the pixel shader via `frac()`.

Override the index path for any other launcher/profile:

```powershell
PotatoEdition.exe --assets "D:\path\to\assets\indexes\34.json"
```

The same store feeds the **sound engine**: vanilla dig / hit / crit `.ogg` files
are located by name, decoded by `stb_vorbis`, and packed into one contiguous PCM
arena for zero-latency, 3D-positional XAudio2 playback.

---

## Milestone Roadmap

| Milestone | Scope | Status |
|---|---|---|
| **M0** | Auth, private repo, Codespace, CI skeleton | Done |
| **M1** | D3D10 FL10_0 boot, embedded HLSL (`vs_4_0`/`ps_4_0`), clear + quad | Done |
| **M2** | `34.json` parser, SHA-1 object store, boot-time texture atlas | Done |
| **M3** | Chunks, Perlin terrain, async workers, 3-axis greedy meshing, fly camera | Done |
| **Phase 2** | 1.9+ PvP combat matrix, native audio architecture, vanilla GUI, time-sliced anti-lag buffer | Done |
| **M4** | Run on the i3-M380; tune render distance + slice budgets to locked 70+ FPS | **Next** |

Future sprint candidates (not silently in scope): collision physics, mobs,
water/transparency, caves/trees, save/load, flood-fill lighting, multiplayer.

---

## Module Map

| File | Responsibility |
|---|---|
| `src/main.cpp` | Win32 + D3D10 boot, input, game loop, block interaction, particle pass |
| `src/engine.h/.cpp` | QPC clock, fly camera, 1.9+ attack cooldown, `PlayerState`, rigid `RingQueue`, crit particles |
| `src/world.h/.cpp` | 64 KB chunks, Perlin gen, 2 workers, greedy meshing, DDA raycast, budgeted eviction |
| `src/renderer.h/.cpp` | Device/swapchain, shaders, states, frustum extraction |
| `src/assets.h/.cpp` | `34.json` index, SHA-1 objects, PNG decode, atlas builder |
| `src/sound.h/.cpp` | XAudio2, PCM arena, ring-queued background dispatch, voice pool, 3D pan |
| `src/gui.h/.cpp` | Ortho quad pass, batched `ascii.png` text, vanilla menus + HUD |
| `src/shader.fx` | World tiling PS + plain UI PS (embedded into the exe at build time) |
| `src/selftest.cpp` | 31 headless checks — runs in CI on every push |

---

## Controls

| Input | Action |
|---|---|
| `W A S D` / `Space` / `Shift` | Fly move / up / down |
| Mouse | Look (raw input) |
| Left click | Break block — damage follows the 1.9+ charge cooldown |
| Right click | Place selected block |
| `1`–`9` / wheel | Hotbar slot |
| `Esc` | Pause / back |

Fully charged (`> 0.9`) strikes while falling are **critical hits**: x1.5 damage,
particle sweep, crit sound. Spam clicking deals as little as 20% damage.

---

## Build & CI

Every push triggers `windows-latest` MSVC: CMake configure -> Release build ->
headless selftest -> artifact upload -> **auto-publish `v1.0.0-Alpha` release**
with `PotatoEdition.exe` attached.

Local build (requires VS 2022 / Build Tools + Windows 10 SDK):

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
.\build\Release\PotatoSelfTest.exe
```

---

<div align="center">

Fan project for educational use. Not affiliated with Mojang or Microsoft.
Minecraft is a trademark of Mojang Synergies AB.

</div>