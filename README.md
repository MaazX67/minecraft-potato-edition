# Minecraft Potato Edition

A Minecraft Java Edition clone written from scratch in C++17. No game engine, no framework — just Win32 and DirectX 10 on top of the Windows SDK.

The whole point of the project is to see how far you can push a first-person block game on a genuinely old machine. My target box is an i3-M380 (2 cores, 4 GB RAM) with first-gen Intel HD graphics, so everything in here is built around tight frame budgets and a hard memory ceiling. It doubles as a study of how Java Edition actually works under the hood: chunk streaming, greedy meshing, the 1.9 attack cooldown, atlas batching, and so on.

Not affiliated with Mojang or Microsoft. Textures, sounds and the font are loaded at runtime from a Minecraft install on the same machine — nothing copyrighted is stored in this repo.

## Download

Grab the latest `PotatoEdition.exe` from the Releases page. It's one self-contained binary; the CRT is linked statically so there's no redistributable to install.

## Parity probe

Run `PotatoEdition.exe --parity` next to your Minecraft install. It loads the Java 26.3 JAR from
`%APPDATA%\Migurinth\meta` (or the folders you pass with `--assets` / `--jar`), then writes `parity.txt` with one
line:

```
PARITY-REGISTRY: jar=yes blocks=... items=... recipes=... loot=... names=... dup=0 hash=........
```

`dup=0` means no two ids share a resource name. The build is gated on that line, so paste it back when it changes.
No window opens and nothing is installed.

## What works so far

- Versioned climate terrain with nine biome categories, rivers, mountains, layered surfaces, a deepslate layer below y=8, coherent ore deposits with deepslate variants, underground caves and oak/birch/spruce trees; legacy terrain and Superflat remain available
- Create New World: Game / World / More tabs, functional world type, name, seed, draft difficulty and Survival/Creative modes; unavailable features stay disabled
- Seeds currently use a 32-bit generator; terrain and seed results do not match Java Edition
- Select World list with paging, new-world creation, and Save and Quit; player state and changed blocks survive restarting or chunk eviction
- Day/night at 20 ticks per second: reads the installed Java 26.3 timeline for eased celestial motion, sky/light/fog colours, twilight and star brightness; installed sun and eight moon textures
- Pause freezes world time; inventory keeps the celestial clock running; world time survives saving and reopening
- Chunk streaming with a render-distance setting, two worker threads, greedy meshing and frustum culling
- Survival: health, hunger, fall damage, sprinting and swimming — no flying
- Creative: double-tap-space flight, no hearts/hunger on the HUD
- Basic textured zombies: night spawning capped at eight, collision-aware pursuit, contact damage, three-block attack reach and terrain occlusion
- Tool-based attack cooldowns, falling critical hits, hurt immunity, knockback and durability; this is local combat, not multiplayer PvP
- Timed tool-aware survival mining, ten installed crack stages, material mining/break sounds, harvest tiers and basic ore drops
- 24 tools with installed item textures and durability bars; wood/stone/iron/diamond/gold crafting-table recipes and sticks (Netherite tools are creative-only)
- Break and place blocks, selection outline, positioned 3D sound
- Footsteps per material, water splash, fall-damage hurt and UI click sounds
- Menus: title screen with rotating splash text and the panorama background, pause menu, death screen
- Separate video, sound and controls pages with persistent FOV, render distance, sensitivity, master volume, brightness, view bobbing, VSync and fullscreen
- Local/offline profiles with username, nine installed default skins, wide/slim models, and a textured inventory preview; no Microsoft account integration
- Survival inventory and crafting table: stack counts, splitting, shift transfers, ingredient consumption, and safe return of cursor/crafting items
- HUD with hearts, hunger, hotbar and the attack-cooldown indicator
- 125 blocks using the real vanilla textures, including the deepslate family, deepslate/copper/emerald ores, mineral storage blocks, nether/basalt/blackstone/quartz blocks, terracotta, the full wool palette, stone-brick variants, red sandstone, prismarine, End stone/purpur, crying obsidian/ancient debris and bone/hay/melon blocks; the creative picker scrolls with the mouse wheel to fit them all
- Headless regression tests run on every cloud build, covering camera frusta, mesh orientation, item conservation, crafting and survival rules
- Native Windows smoke tests exercise real clicks and compare screenshot sidecars against expected inventory state

## Controls

| Input | Action |
|---|---|
| `W` `A` `S` `D` | Move |
| `Space` / `Shift` | Jump / sneak (Creative: double-tap `Space` toggles flight) |
| Mouse | Look |
| Left click | Break block, or attack |
| Right click | Place a block or open a crafting table; `Shift` bypasses opening |
| `1`–`9`, mouse wheel | Select hotbar slot |
| `E` | Inventory |
| `F2` | Screenshot (saved to `screenshots/`) |
| `F3` | Debug overlay |
| `Esc` | Pause menu |

## Building

Needs Visual Studio 2022 (or the Build Tools) and the Windows 10 SDK.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
.\build\Release\PotatoSelfTest.exe
```

Every push to `main` runs the same thing on GitHub Actions: configure, release build, run the
headless tests and upload the exe as a build artifact. Releases are published separately after
native verification, using distinct version tags and feature/limitation notes; previous releases are not overwritten.
The executable includes an original procedural icon, not a copied Minecraft icon.

Run the native checks on Windows with the downloaded executable and an existing output directory:

```powershell
.\native-smoke.ps1 -Exe C:\Games\PotatoEdition.exe -OutputRoot C:\Temp
```

The script launches isolated test instances, checks crafting and save/reload after a process restart, and exercises save failures and damaged edit streams. It also captures noon, sunset, midnight, sunrise and all eight moon phases, checks clock/pause behaviour and tests world-time persistence and old-save loading. New fixtures exercise timed mining, crack stages, item textures, durability, terrain occlusion, zombie/player damage, knockback, critical hits and climate terrain. Test profiles and worlds stay in the output directory; regular preferences live under `%APPDATA%\MinecraftPotatoEdition\preferences.json`.

Worlds live under `%APPDATA%\MinecraftPotatoEdition\saves\world-*\world.potato`. Each file contains the name, generator version, seed, game mode, difficulty, world time, position, rotation, health/hunger, inventory, optional tool wear and block edits in a versioned MessagePack container. Existing saves retain their legacy generator; newly created worlds default to climate terrain. Zombies are session-only and are not saved yet. Terrain remains 0–255 blocks high rather than Java's -64–319. Format-three saves are current; format-two added world time and format-one worlds load at day zero. Older saves stored items at IDs 64–104 and are remapped to the new 128+ item range on load, then upgraded on their next save. Save and Quit and normal window close write a temporary file before atomically replacing the previous save. There is no periodic autosave yet. This is a Potato-only format, not Java's Anvil/NBT format; do not open Java worlds with it.

## Still to do

- Java-compatible world format/import, autosaving, world thumbnails and world-management actions
- Java terrain/biomes, structures, 64-bit seed parity and flowing liquids
- Full mob AI/spawning/persistence, entity drops, randomized block loot, enchantments, equipment, furnace processing and the full item/recipe registry
- Held-tool models/animations, shield blocking, sweeping attacks and complete Java combat rules
- Armor/offhand mechanics, third-person player rendering, custom skin import and account integration
- Clouds, weather, biome sky/fog colours, exact Java lightmap/block lighting, music, subtitles, multiplayer, game rules, data packs and experiments

[Java 26.3](https://www.minecraft.net/en-us/article/minecraft-java-edition-26-3) is the reference baseline, not a claim of full compatibility. The version is checked against [Mojang's release manifest](https://piston-meta.mojang.com/mc/game/version_manifest_v2.json) and the locally installed client. This remains an incomplete implementation.

## Engine notes

A few decisions worth writing down, since they aren't obvious from the code:

- **Rendering** is raw D3D10 at feature level 10_0, because that's the ceiling on the target
  GPU. VSync is off and the game renders as fast as the machine allows.
- **Chunks** are 64 KB flat arrays with a greedy mesher on three axes. Only the visible faces
  are ever uploaded, and liquid faces are kept in a separate index range so water can be drawn
  in one blended pass after the opaque geometry.
- **Streaming** is time-sliced: uploads, chunk evictions and the GPU buffer sweep all have
  per-frame budgets, so a big chunk load never stalls the render thread.
- **Atlas** is a 1024×1024 texture with five tile-aligned mip levels. The shader samples texel centers inside each tile, reducing distant shimmer without bleeding neighboring textures. Cutout blocks remain alpha-tested.
- **Audio** is XAudio2 with a pool of pre-created voices. Sound requests go through a lock-free
  ring queue to a background thread, so the render thread never touches the audio device.

## Credits

The font, textures and sounds are Mojang's; they come from the user's own copy of the game at
runtime and are not redistributed here. `json.hpp`, `stb_image` and `stb_vorbis` are vendored in
`third_party/`.

The code in here is mine, but I haven't written a license file yet — ask if you want to use it.
