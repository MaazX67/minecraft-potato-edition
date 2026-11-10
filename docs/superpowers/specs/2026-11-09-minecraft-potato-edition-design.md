# Minecraft Potato Edition — Design Spec

Date: 2026-11-09
Status: Approved design (private repo), pending spec review
Author: MaazGAMER76 + Qoder

## 1. Goal

A from-scratch C++ voxel engine ("Minecraft Potato Edition") rendering Minecraft-style
terrain at a locked 70+ FPS, 1366x768, on:

- Intel Core i3-M380 (2 cores / 4 threads)
- 1st-gen Intel HD Graphics (Ironlake), Direct3D feature level 10_0, WDDM 1.1
- 4 GB RAM, Windows 10
- Raw Win32 + Direct3D 10 backend. No VM, no GC, no OpenGL, no modern-API overhead.

Engine only: fly camera over generated terrain. No gameplay systems.

## 2. Verified environment facts

- GitHub CLI 2.103.3 installed; account MaazGAMER76; token was invalid at design time.
  User runs `gh auth login -h github.com -s codespace` before M0.
- Local asset store (verified, read-only):
  - Index: `C:\Users\Admin\AppData\Roaming\Migurinth\meta\assets\indexes\34.json`
    JSON `{"objects": {"minecraft/textures/block/stone.png": {"hash": "<sha1>", "size": N}, ...}}`
  - Objects: `C:\Users\Admin\AppData\Roaming\Migurinth\meta\assets\objects\<first-2-hex>\<sha1>`
    — raw PNG files, no ZIP/jar parsing required.
  - Version jar exists (`meta/versions/26.3-0.20.1/`) but is NOT needed.
- Workspace starts empty; not a git repo. Git 2.52 available.

## 3. Cloud workflow (local machine does no heavy work)

- GitHub repo: `MaazGAMER76/minecraft-potato-edition`, **private**, created empty.
- GitHub Codespace (Linux, 2-core): code authoring and git operations only.
  Codespaces cannot run Windows/DX10/MSVC — no builds or visual tests there.
- GitHub Actions `windows-latest`: real MSVC v143 + Windows 10 SDK compile on every
  push; runs `--selftest`; uploads `PotatoEdition.exe` artifact.
- Visual/FPS verification happens only on the user's laptop (the target GPU).
  CI proves compilation + CPU-side logic; it cannot prove frame rate.
- Mojang textures are never committed. The engine reads them from the local asset
  store at boot on the user's machine.

## 4. Repository layout

```
minecraft-potato-edition/
├── CMakeLists.txt
├── .github/workflows/build.yml
├── src/
│   ├── main.cpp        # Win32 + D3D10 boot, message loop, frame loop
│   ├── engine.h        # QPC delta clock, input state, fly camera
│   ├── assets.h/.cpp   # index parse, PNG decode, atlas build, block registry
│   ├── world.h/.cpp    # chunks, Perlin gen, worker pool, greedy meshing
│   └── shader.fx       # vs_4_0 / ps_4_0 HLSL (both entry points in one file)
├── third_party/
│   ├── stb_image.h     # vendored, public domain PNG decoder
│   └── json.hpp        # vendored nlohmann/json, index parsing only
└── docs/superpowers/specs/  # this spec
```

## 5. Module specifications

### 5.1 main.cpp — bootloader

- Raw Win32: `RegisterClassEx`, `CreateWindowEx`, fixed 1366x768 client area
  (`AdjustWindowRect`), non-resizable, `ShowCursor(FALSE)`.
- `D3D10CreateDeviceAndSwapChain`: `D3D10_DRIVER_TYPE_HARDWARE`, feature level
  `D3D10_FEATURE_LEVEL_10_0`, `BufferCount 1`, `DXGI_FORMAT_R8G8B8A8_UNORM`,
  `DXGI_SWAP_EFFECT_DISCARD`, `SwapChainFlags 0`. VSync off: `Present(0, 0)`.
- Depth stencil: `DXGI_FORMAT_D24_UNORM_S8_UINT`, depth test enabled, backface cull.
- Viewport 1366x768. Shaders compiled at boot from `shader.fx` source via
  `D3DCompile` (d3dcompiler.lib; d3dcompiler_47.dll ships on Win10) — two calls,
  entry points `VSMain` / `PSMain`, profiles `vs_4_0` / `ps_4_0`. No D3DX10
  (not in modern SDK), no effects framework.
- Frame loop: poll messages → dt update → camera → chunk job dispatch → upload
  finished meshes → draw → Present. FPS shown in the window title every 0.5 s
  (`SetWindowText`) — no font rendering, zero GPU cost.
- `shader.fx` is embedded into the exe as a string at build time (CMake
  `configure_file`); boot compiles from memory, so the artifact is a single
  self-contained exe.
- `--selftest` flag: headless mode, no window/device; runs logic tests (section 7).

### 5.2 assets.h/.cpp — boot-time atlas pipeline

1. Read index JSON (path configurable, default the verified Migurinth path).
2. Select keys starting `minecraft/textures/block/`, ending `.png`.
3. Decode each object file with `stb_image` (force RGBA). Textures taller than
   16 px (animated strips) use the top 16x16 frame. Non-16x16 textures are
   nearest-neighbor resampled to 16x16.
4. Blit into a contiguous 1024x1024 CPU atlas (64x64 = 4096 tile slots), tile id
   = insertion order. Missing/undecodable files get a magenta/black checker tile.
5. Upload once as `ID3D10Texture2D` (immutable) + `ID3D10ShaderResourceView`.
   Point min/mag filter, no mips, clamp addressing. Half-texel UV inset baked
   into mesh UVs to prevent bleeding.
6. Block registry: block id → {top, side, bottom} tile indices. Initial blocks:
   air (0, sentinel), stone, dirt, grass (grass_top / grass_side / dirt tiles).

### 5.3 world.h/.cpp — voxel pipeline

- Chunk: `struct Chunk { uint8_t blocks[16*256*16]; }` — 64 KB, flat, no pointers.
  Index = `x + 16*z + 256*y`. Render distance 8 → 17x17 = 289 chunks ≈ 18.8 MB.
- Terrain: improved Perlin noise, 2D fBm (4 octaves), fixed seed.
  `height = 64 + fbm(x,z) * 24`; stone below h-4, dirt h-4..h-1, grass at h.
  No caves, trees, or water in v1.
- Workers: 2 `std::thread`s (matches 2 physical cores). Two mutex-protected queues:
  gen jobs (nearest chunks first) and mesh jobs. A chunk is meshed when it and
  its 4 horizontal neighbors inside the loaded set are generated; neighbors
  outside the loaded set count as air (their faces are drawn, then remeshed when
  a neighbor arrives). Generating a chunk marks existing neighbors dirty. Finished CPU meshes go to an upload queue consumed by the
  render thread (D3D10 device calls stay on the main thread; device is created
  without multithread protection).
- Greedy meshing (active, all 3 axes): for each axis, sweep 16/256 slices; build
  a 2D mask of exposed faces keyed by (tile id, light, direction); merge adjacent
  mask cells into maximal rectangles; emit one quad (4 verts, 6 indices) per
  rectangle. Face exposure = neighbor block is air. Merge key includes light so
  shading boundaries stay correct.
- Light model v1: per-face orientation values — top 15, bottom 8, X sides 12,
  Z sides 10; stored raw (0–15) per vertex, divided by 15 and multiplied in the
  vertex shader. No flood-fill propagation yet.
- GPU data per chunk: one vertex buffer + one 32-bit index buffer, re-created on
  remesh. Vertex = pos (3f), tile origin (2f), local UV in block units (2f),
  light (1f) = 32 bytes. The pixel shader repeats tiles across merged quads via
  `frac(localUV)` — required so greedy quads don't stretch textures.
  Draw: `DrawIndexed` per visible chunk, frustum-culled by chunk AABB.

### 5.4 engine.h — clock and input

- `QueryPerformanceCounter` delta time, clamped to 100 ms (debugger/alt-tab safe).
- Raw `WM_MOUSEMOVE` deltas → yaw/pitch (sens 0.0025 rad/px, pitch clamped ±89°).
- Keyboard state array from WM_KEYDOWN/UP: W/A/S/D + Space (up) + Shift (down),
  fly speed 10 units/s, dt-corrected. ESC releases cursor and exits.
- View/proj via DirectXMath (ships with SDK): perspective 70° FOV, near 0.1,
  far 1000; row-major upload into a per-frame constant buffer.

### 5.5 shader.fx

- `cbuffer PerFrame { row_major float4x4 gViewProj; }`
- VS: transform position; pass UV; `color = light / 15.0` (hardware multiply).
- PS: `atlas.Sample(pointSampler, uv) * color`; no lighting loops, no branches.

## 6. Performance budget

- Block storage ~19 MB, meshes est. < 150 MB, atlas 4 MB, total < 300 MB —
  far under the 1.5 GB cap.
- Draw calls ≤ 289/frame; greedy meshing targets ~90% vertex reduction vs
  naive per-face quads. 70+ FPS target verified on hardware in M4, not in CI.

## 7. Verification

- CI (windows-latest, MSVC v143, /W4): `cmake -A x64` Release build must succeed.
- CI executes `PotatoSelfTest.exe` (separate console target linking the CPU-only
  `potato_core` library — no D3D device, no window):
  - Greedy mesh of a fully solid chunk yields exactly 6 quads / 24 verts.
  - Single isolated voxel yields 6 quads; known asymmetric pattern yields a
    precomputed quad count.
  - Perlin: same seed → identical heightmap; values bounded.
  - Atlas builder with synthetic tiles: correct tile placement + checker fallback.
  - Exit 0 with PASS lines, exit 1 with FAIL detail on any mismatch.
- Visual verification: user downloads the exe artifact, runs on the laptop,
  reports FPS (on-screen FPS counter included) and screenshots.

## 8. Milestones

- M0: auth fixed → create private repo, push spec, create Codespace, CI skeleton
  builds an empty-window exe. (Requires user's gh login.)
- M1: D3D10 boot, clear color, textured quad with shader.fx compiling under MSVC.
- M2: atlas pipeline live (reads local store on the laptop), textured cube.
- M3: chunks + Perlin terrain + workers + greedy meshing + fly camera + FPS counter.
- M4: user runs on i3 laptop; tune render distance / mesh upload until 70+ FPS.

## 9. Out of scope (v1)

Block place/break, collision physics, water/transparency, caves, trees, mobs,
saving/loading, audio, multiplayer, sky/fog, mipmapping, flood-fill lighting.
Each is a candidate follow-up, not silent scope creep.

## 10. Known limits, stated honestly

- Codespaces cannot compile with MSVC or run DX10; all MSVC verification is via
  the Actions Windows runner, all visual verification via the user's laptop.
- Ironlake has no cloud equivalent; 70 FPS is an engineering target, validated
  only on the real machine in M4.

## 11. Scope addendum (user-directed, 2026-11-09)

Added to v1 by explicit user request; "pixel-perfect full Minecraft" remains
out of scope — these are the delivered systems:

- **Audio** (`sound.h/.cpp`): XAudio2 (ships with Win10). Ogg decoded at
  boot via vendored `stb_vorbis.c`; dig/attack/crit sounds pre-buffered as PCM;
  Stop/FlushSourceBuffers/SubmitSourceBuffer retrigger = no play-time decode or
  disk I/O. Missing sounds degrade to silence.
- **UI** (`gui.h/.cpp`): orthographic quad pass (depth off, alpha blend) using
  vanilla GUI sprites from the object store (buttons, logo, crosshair, hotbar,
  ascii font). Title / Options / Pause screens; options (render distance, FOV,
  sensitivity, sound) are functional. HUD: crosshair, attack-recharge bar,
  9-slot hotbar with block icons, actionbar text. Missing sprites show checker.
- **Combat** (`engine.h/.cpp` AttackCooldown + main): 1.9+ cooldown —
  strength = clamp((elapsed+25ms)/period), damage = base * (0.2+0.8*s^2),
  falling-hit crits x1.5 with crit sound. Left click = DDA raycast break,
  right click = place selected hotbar block (never inside the camera).
- Grass textures get the plains biome tint (0x91BD59) baked at atlas load.

## 12. Phase 2 addendum (user-directed, 2026-11-10)

- **PvP combat matrix**: `PlayerState.attackCooldownProgress` refreshed every
  frame from raw QPC milliseconds (`SystemTimeMs`, never the clamped render
  delta). Vanilla crit gate enforced: strength > 0.9 AND falling; short-cycled
  clicks take the 0.2+0.8*s^2 damage penalty and spawn no particle sweep.
  True crits spawn a 16-particle `critical_hit.png` sweep (CPU-simulated in
  `engine.cpp`, rendered as camera-facing quads with depth-test-on/write-off,
  alpha fade by remaining life, hard cap 256).
- **Native audio architecture**: all PCM packed into one contiguous arena
  reserved once at boot (submitted pointers can never dangle). Playback
  requests enter a rigid 64-slot ring queue served by a background thread —
  the render thread never calls XAudio2. Voice pool: 3 pre-created source
  voices per variant so overlapping plays don't cut each other. 3D positional
  playback: 16-block linear attenuation + equal-power stereo pan via
  SetOutputMatrix against the camera listener. Full ring = dropped request,
  never a blocked frame.
- **Time-sliced anti-lag buffer**: mesh uploads moved from an unbounded deque
  to a rigid 128-slot `RingQueue` with worker backpressure (failed TryPush
  leaves the mesh intact; worker retries at 1 ms). Chunk eviction is two-phase:
  `World::Update` only detaches evicted subchunks from the map; `EvictStep(4)`
  frees at most 4 x 64 KB per frame. GPU-side cleanup sweeps at most 32 map
  entries per frame from a rotating cursor. Ceilings live in `engine.h`:
  8 uploads / 4 evicts / 32 GPU checks per frame.
- **Verification**: selftest extended to 31 checks (cooldown math incl. the
  0.9 crit boundary, ring FIFO/overflow/wrap, particle cap and expiry). All
  pass under a local MinGW syntax+unit run; MSVC build remains CI-gated.