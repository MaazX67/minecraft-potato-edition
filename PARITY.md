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
| Loot tables | 133 | full JAR set |

**Baseline captured from the user's machine** (`PotatoEdition.exe --parity`, step 1.1 build):

```
PARITY-REGISTRY: jar=yes blocks=136 items=66 recipes=64 loot=133 names=202 dup=0 hash=91ec9356
```

That 16-bit hash replaces the retired Step 0 8-bit hash. It may only change when the
registry really changes, and `dup` must stay 0.

Exit criteria: every 26.3 block, item, recipe and loot table is loaded from the JAR/index at runtime, with **zero
hardcoded subsets**, proven by a probe that counts and hashes the loaded registry.

## Phase 0 status

| Step | State | Evidence |
| --- | --- | --- |
| Port ledger (`tools/parity/ledger.ps1`) | done | 11,383 classes / 98,739 methods from the 26.3 JAR |
| `PARITY-REGISTRY` probe | done | `TestRegistryParity` in `selftest.cpp`; CI run 43585847454 green |
| Specs | done | `docs/specs/ui.md`, `docs/specs/golden.md` (measured from the JAR) |
| ID widening (`ItemId`/`BlockId` -> 16-bit) | done (step 1.1) | `enum BlockId/ItemId : uint16_t`, 16-bit palette index tier, edits blob v2; see `docs/registry-widening.md` |
| `--parity` gate (real numbers without a compiler) | done (step 1.1) | `PotatoEdition.exe --parity` writes `parity.txt` on the user's machine |
| Opaque/liquid index split (P0 render bug) | done | `BuildChunkMeshCPU` records `opaqueIndexCount` before appending trail liquid quads; selftests: solid cube stays fully opaque, stone+water splits 36/36 with disjoint vertex ranges |
| JAR bytecode probe (`tools/parity/classdump.ps1`) | done | reads vanilla constants straight from the client JAR without a JDK; used for the swing duration and hand transforms below |
| First-person hand pitch (A) | done — no change needed | `DrawHand` renders with projection only (identity view), so the arm is pure view-space and cannot follow camera pitch on `ship` |
| Arm swing triggers (B) | done | `ArmSwing` fires on attack (hit or miss), air punch, mining tick, block break, block place, firework use and portal light; attack cooldown stays separate |
| Swing duration | done | 6 ticks = 300 ms. JAR: `SwingAnimation.<clinit>` `bipush 6`; `LivingEntity$SwingState.tick` `min(ticks/durationTicks,1)` then reset |
| Held-item swing math | done | `ComputeSwingPose` reproduces `FirstPersonHandsAndItemsRenderer.swingArm` (-0.4 / 0.2 / -0.2) and `applyItemArmAttackTransform` (45 / -20 / -80 / -45, order YP,ZP,XP,YP); selftests pin p=0, 0.25, 1 and left-hand mirroring |
| Swing tick interpolation | UNVERIFIED | Java lerps `oldAnimation`→`animation` per 20 TPS tick (`SwingState.getAnimation`); we sample continuous time per frame. Close, not tick-exact |
| Arm rig translation | UNVERIFIED | the bare-arm rig still uses a `sin(pi*p)` lift; only the held item uses the JAR-exact pose. Needs a screenshot comparison |
| Bare-arm transform (cited, port pending) | UNVERIFIED | `FirstPersonHandsAndItemsRenderer.renderPlayerArm` read in full: translate(side*(-0.3*sin(sqrt(p)*pi)+0.64), 0.4*sin(sqrt(p)*2pi)-0.6-0.6*equip, -0.4*sin(p*pi)-0.72); YP side*45; YP side*sin(sqrt(p)*pi)*70; ZP side*sin(p*p*pi)*-20; translate(side*-1, 3.6, 3.5); ZP side*120; XP 200; YP side*-135; translate(side*5.6, 0, 0). Port needs the arm mesh in Java model units |
| Creative panel layout (C) | written, pending CI | `src/ui_layout.h` pins Java 195x136 panel, 9x5 picker at (9,18), the 9 hotbar slots as the last row at (9,112) INSIDE the panel, no storage rows, borderless search EditBox at (82,6) 80x9 max 50. JAR: `CreativeModeInventoryScreen.<init>` sipush 195/136; `ItemPickerMenu.<init>`; `AbstractContainerMenu.addInventoryHotbarSlots`; `init` EditBox. `TestCreativeLayout` pins every rect plus the pick->place->mirror chain |
| Creative storage rows removed (C root cause) | done (parity) | Java's `ItemPickerMenu` adds only the hotbar slots. Our 27 storage rows sat where the player expects the hotbar, so picked items landed in storage and the HUD hotbar stayed empty; closing creative then discarded the cursor |
| Hotbar stack counts + durability | written, pending CI | `DrawHotbar` now draws `DrawStackCount` per slot like Java's hotbar; the HUD passes real stacks in survival AND creative |
| Creative scrollbar / tab strip geometry | UNVERIFIED | knob/track sizes and Java's top+bottom tab wrapping not yet read from the JAR; current strip is a single row above the panel |
| Humanoid mob mesh (E, researched) | port pending | JAR `HumanoidModel.createMesh` (dump: run/parity/HumanoidModel.txt). Units: 16 = 1 block, Y down, feet at y=24. head tex(0,0) box(-4,-8,-4, 8,8,8) pivot(0,0,0); hat tex(32,0) same box grown 0.5; body tex(16,16) box(-4,0,-2, 8,12,4) pivot(0,0,0); right_arm tex(40,16) box(-3,-2,-2, 4,12,4) pivot(-5,2,0); left_arm tex(40,16) mirror box(-1,-2,-2, 4,12,4) pivot(5,2,0); right_leg tex(0,16) box(-2,0,-2, 4,12,4) pivot(-1.9,12,0); left_leg mirror pivot(1.9,12,0). `ZombieModel` adds no geometry, only `AnimationUtils.animateZombieArms` when aggressive |
| Skeleton mesh overrides (E, researched) | port pending | JAR `SkeletonModel.createDefaultSkeletonMesh`: arms tex(40,16) box(-1,-2,-1, 2,12,2) pivots(-5,2,0)/(5,2,0) mirror; legs tex(0,16) box(-1,0,-1, 2,12,2) pivots(-2,12,0)/(2,12,0) mirror; head/body unchanged from HumanoidModel |
| Creeper mesh (E, researched) | port pending | JAR `CreeperModel.createBodyLayer`, texture 64x32: head tex(0,0) box(-4,-8,-4, 8,8,8) pivot(0,6,0); body tex(16,16) box(-4,0,-2, 8,12,4) pivot(0,6,0); 4 legs tex(0,16) box(-2,0,-2, 4,6,4) pivots hind(±2,18,4) front(±2,18,-4). setupAnim: head xRot/yRot = state degrees * 0.01745329; leg xRot = cos(walkPos*0.6662 [+pi on left hind & right front]) * 1.4 * walkSpeed |
| Spider mesh (E, researched) | port pending | JAR `SpiderModel.createSpiderBodyLayer`, texture 64x32: head tex(32,4) box(-4,-4,-8, 8,8,8) pivot(0,15,-3); body0 tex(0,0) box(-3,-3,-3, 6,6,6) pivot(0,15,0); body1 tex(0,12) box(-5,-4,-6, 10,8,12) pivot(0,15,9); legs tex(18,0) right box(-15,-1,-1, 16,2,2), left mirror box(-1,-1,-1, 16,2,2); right pivots/rot(xRot,yRot,zRot): hind(-4,15,2)(0,+0.7853982,-0.7853982), mid-hind(-4,15,1)(0,+0.3926991,-0.5811946), mid-front(-4,15,0)(0,-0.3926991,-0.5811946), front(-4,15,-1)(0,-0.7853982,-0.7853982); left = x negated, yRot/zRot signs flipped |
| Quadruped mesh (E, researched) | port pending | JAR `QuadrupedModel.createBodyMesh(legHeight h, mirror flags)`: head tex(0,0) box(-4,-4,-8, 8,8,8) pivot(0,18-h,-6); body tex(28,8) box(-5,-10,-7, 10,16,8) pivot(0,17-h,2) with xRot=pi/2 (horizontal); legs tex(0,16) box(-2,0,-2, 4,h,4) hind pivots(±3,24-h,7) front(±3,24-h,-5). Per-animal h comes from each subclass call site (cow/sheep/pig not yet dumped) |
| JAR-driven block textures (step 1.2a) | done | `ResolveBlockTextures` reads blockstates/models; chest + crafting-table magenta fixed; `PARITY-BLOCKTEX` gate; see `docs/block-textures.md` |
| Full 26.3 registry + one shared atlas (step 1.3) | written, pending CI | `RegisterRuntimeRegistry` appends 1,288 blockstates + item namespace from 0x1000; only full cubes placeable; one 1024×1024 block+item atlas; `cubes=` in parity line; see `docs/registry-m1.md` |
| Sky-light BFS (F) | written, pending CI | `BuildChunkMeshCPU` replaces the crude skyAt with a Java SkyLightEngine-style BFS: sky=15 above the heightmap, straight-down propagation keeps full level, horizontal/up −1, opaque blocks seal it. Out-of-chunk treated as sealed (matches block-light pass). `TestSkyCrevice` pins sideways bleed under an overhang (0<sky<15) plus the existing sealed-roof pins. Cross-chunk sky bleed = UNVERIFIED (minor edge seam) |
| Mace combat | done (math) / wired | `combat.h` `MaceCanSmash/SmashBonus/KnockbackPower` port `MaceItem.class` bytecode: base dmg 6, speed 0.6; smash needs fall>1.5 & not gliding; bonus 4d / 12+2(d−3) / d+14; knockback (3.5−dist)·0.7·(fall>5?2:1). `main.cpp` adds the bonus, resets fall distance and bounces the attacker on smash. `TestCombatMath` pins golden values. Mace durability 500 = wiki, UNVERIFIED vs JAR component |
| Spear combat (7 tiers) | done (math) / wired | Spear is data-driven in 26.3 (no SpearItem class). `WeaponFromName` resolves wooden/stone/copper/iron/golden/diamond/netherite from JAR+wiki stats (jab dmg 1/2/2/3/1/4/5, speed 1.54/1.33/1.18/1.05/1.05/0.95/0.87, dur 59/131/190/250/32/1561/2031). Reach [2,4.5], multi-hit forward arc, NO crit/sprint-knockback, jab gated to full charge. Charge attack (velocity damage ≥4.6 b/s, knockback ≥5.1) math pinned; the hold-to-charge input + Lunge enchant are UNVERIFIED/pending |
| Trident combat | done (melee) | `WeaponFromName("trident")`: 9 dmg, 1.1 speed, 250 dur (wiki). Melee wired; throw/riptide projectile UNVERIFIED/pending |
| Wind charge | PARTIAL | `WindChargeItem.class` use: projectile velocity 1.5, inaccuracy 1.0, consume 1. Current build applies an instant burst (knockback radius 3.5 + self-boost) on right-click; the traveling projectile entity and exact AbstractWindCharge burst radius are UNVERIFIED/deferred |
| Creative hidden blocks | done (set) / UNVERIFIED (order) | `IsCreativeHidden` removes operator/technical blocks (command/structure/jigsaw/barrier/light/debug_stick/spawner/trial_spawner/vault/ominous_vault/knowledge_book/reinforced_deepslate/budding_amethyst) from every tab AND search, matching Java's permission-gated Operator Utilities tab. Exact per-tab membership/order pending a full `CreativeModeTabs` parse |
| XP bar hidden in creative | done (parity) | `DrawHUD` gates the experience bar on `!creative`, same as hearts/hunger (Java hides XP in creative/spectator) |
| Creative Java textures (C visual) | written, pending CI | `DrawInventory` draws the real JAR `creative_inventory/tab_items.png` / `tab_item_search.png` panel (195x136 of 256x256), the shaped `tab_top/bottom_selected/unselected_1..7` sprites and `scroller.png` knob instead of gray boxes + generic widget tabs; gray bevel fallback only when the jar is absent. Picker/hotbar slot outlines come from the background, hover uses the selection frame |
| Per-mob sounds | each mob loads its own say/hurt/death from `sounds/mob/<id>/`; `MobSoundCat(type,kind)`; ambient/hurt/death call sites use the mob's voice. `TestMobSound` pins mapping | DONE (pending user audio check) |
| XP orbs + Java curve | mobs drop orbs (hostile 5, animal 1-3) that magnet+collect; `XpToNextLevel`=2L+7/5L-38/9L-158; HUD XP bar+level wired. `TestXpCurve` | DONE (pending user visual check) |
| Void + drowning damage | void 4 dmg/0.5 s below y=0; air 300 ticks then 1 heart/s underwater (Java) | DONE (pending user check) |
| Village water guard | surface structures rejected when ground < sea level 62 (submerged); shoreline (==62) still allowed; village selftest picks a dry-land seed; tree-clearing still pending | PARTIAL |
| Boss mobs | Ender Dragon 200 HP, Wither 300 HP, Elder Guardian 80 HP (wiki base health); speed/height/halfWidth are approximations, not Java attribute-exact; drops dragon_egg/nether_star/prismarine_shard. Mob-stats selftest green | UNVERIFIED (sizes/speed approx; models may checker) |
| Spawn eggs | right-click any `*_spawn_egg` spawns its mob via `ParseMobId`; consumed in survival; listed in creative Tools tab | DONE (pending user visual check) |
| End dimension + portal blocks | `WorldGenerator::End` end-stone platform + portal-frame ring; `end_portal`/`end_portal_frame` added to registry (BLOCK_COUNT 140, tables aligned). NOT Java island/obsidian-pillar gen | PARTIAL |
| End portal activation | eye-of-ender frame fill uses a bit hack on block ids (wrong); no held-ender_eye gate; teleport hook present but activation unreliable | BLOCKED (needs proper filled-frame state) |
| Held-item texture smear (P0) | extruded item mesh now maps pixel boundaries (x/16) across the full 16-texel tile (`AtlasTileRectExact`); old +0.5-texel shift into a 15-texel inset made each quad straddle two texels. `TestExtrudedUV` pins pixel 8 -> 8/16..9/16 | DONE (pending user visual check) |
| Empty-slot air icon spam (P0) | `DrawItemIcon` returns early for id 0; hotbar/inventory empty slots no longer log `missing icon id=0 name=air` | DONE |
| GUI sprites from JAR (P1) | hotbar/selection/offhand, XP bar bg/progress, crosshair, creative/search panels, font all loaded from `textures/gui/sprites/...` at runtime (`LoadGui`) | DONE (already jar-driven) |
| Spear charge attack (P6) | sprinting full-charge jab adds `SpearChargeDamage(chargeMult, speed)`; player horizontal speed proxies relative velocity (target approach not tracked). Formula pinned by `TestCombatMath` | UNVERIFIED (speed proxy; needs server/mob closing-speed test) |
| P6 remaining gaps | trident throw projectile, wind charge projectile + burst knockback, armor trims from jar, dedicated Spawn Eggs creative tab (eggs currently in Tools) | BLOCKED (next batch) |

`--parity` runs the same probe as `PotatoSelfTest.exe` but inside the shipped executable: it loads the user's own
26.3 JAR from `%APPDATA%\Migurinth\meta`, prints `PARITY-REGISTRY: ...` and exits before creating a window. CI has no
JAR, so it prints `jar=no` there; the real gate is the user's `parity.txt`.

Step 1.1 also made the hash 16-bit aware (`name\0id-lo\0id-hi`), so the number printed on the user's machine differs
from the Step 0 8-bit hash by design. Re-baseline it from the first `--parity` run, then never let it regress.

## How the gate works

- Probes live in `PotatoSelfTest.exe`. CI already runs that binary, so parity failures fail the build without any
  `.github/workflows` change (the token has no `workflow` scope).
- The first probe is `PARITY-REGISTRY` (`TestRegistryParity` in `selftest.cpp`): it loads the user's 26.3 JAR,
  prints `blocks/items/recipes/loot/names/dup/hash`, and fails below the v2.0.0 baseline or on a duplicate name.
  On a CI runner there is no JAR, so it prints `SKIP` and the build stays green; the real gate runs on the
  dev machine.
- Probe format and golden rules: `docs/specs/golden.md`.
- Golden values are hashes/counts committed to the repo. Oracle runs (vanilla reference) happen locally against the
  user's install using the launcher's bundled JRE at
  `%APPDATA%\Migurinth\meta\java_versions\zulu25.38.109-ca-jre25.0.5.0.3-win_x64`.
- Never commit Mojang code, assets, decompiled source, or the generated ledger.

## Update procedure

1. Run `pwsh -File tools/parity/ledger.ps1` (writes local `ledger.json`, prints the area table).
2. Change a score **only** when the area's harness is green, and cite the harness in the commit message.
3. Add the new gap counts from the probe output.
