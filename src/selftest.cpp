#include "assets.h"
#include "engine.h"
#include "world.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace potato;

static int gFail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); } \
    else { printf("FAIL: %s\n", msg); ++gFail; } \
} while (0)

int main() {
    // --- Perlin determinism and bounds ---
    PerlinNoise a(1337), b(1337), c(7);
    bool same = true, bounded = true, differs = false;
    for (int i = 0; i < 100; ++i) {
        const float x = i * 0.37f, z = i * 0.91f;
        const float na = a.Fbm2(x, z, 4), nb = b.Fbm2(x, z, 4);
        if (na != nb) same = false;
        if (na < -1.01f || na > 1.01f) bounded = false;
        if (na != c.Fbm2(x, z, 4)) differs = true;
    }
    CHECK(same, "Perlin deterministic for same seed");
    CHECK(bounded, "Perlin fBm bounded to [-1, 1]");
    CHECK(differs, "Perlin differs across seeds");

    BlockTiles tiles;
    for (int i = 0; i < 256; ++i) {
        tiles.top[i] = 1;
        tiles.side[i] = 2;
        tiles.bottom[i] = 3;
    }

    // --- Greedy meshing ---
    const std::vector<uint8_t> solid(CHUNK_VOL, BLOCK_STONE);
    MeshData md;
    BuildChunkMeshCPU(solid.data(), nullptr, nullptr, nullptr, nullptr, tiles, md);
    CHECK(md.vertices.size() == 24 && md.indices.size() == 36,
          "solid chunk greedy-meshes to exactly 6 quads");
    bool hasTop = false, hasBottom = false;
    for (const MeshVertex& v : md.vertices) {
        if (v.light == 15) hasTop = true;
        if (v.light == 8) hasBottom = true;
    }
    CHECK(hasTop && hasBottom, "face light levels present (top 15, bottom 8)");

    std::vector<uint8_t> one(CHUNK_VOL, 0);
    one[5 + 16 * 5 + 256 * 64] = BLOCK_STONE;
    BuildChunkMeshCPU(one.data(), nullptr, nullptr, nullptr, nullptr, tiles, md);
    CHECK(md.vertices.size() == 24, "isolated voxel -> 6 quads");
    bool inPlace = true;
    for (const MeshVertex& v : md.vertices) {
        if (v.x < 5 || v.x > 6 || v.y < 64 || v.y > 65 || v.z < 5 || v.z > 6) inPlace = false;
    }
    CHECK(inPlace, "voxel quads sit on the voxel (no plane-origin drift)");

    std::vector<uint8_t> two(CHUNK_VOL, 0);
    two[5 + 16 * 5 + 256 * 64] = BLOCK_STONE;
    two[6 + 16 * 5 + 256 * 64] = BLOCK_STONE;
    BuildChunkMeshCPU(two.data(), nullptr, nullptr, nullptr, nullptr, tiles, md);
    CHECK(md.vertices.size() == 24, "two adjacent voxels merge to 6 quads (10 faces -> 6)");

    // --- Raycast ---
    auto get = [&](int, int y, int) -> uint8_t {
        return (y >= 0 && y < CHUNK_Y) ? BLOCK_STONE : 0;
    };
    int hx, hy, hz, nx, ny, nz;
    bool hit = RaycastDDA(get, 8, 300, 8, 0, -1, 0, 500, hx, hy, hz, nx, ny, nz);
    CHECK(hit && hy == CHUNK_Y - 1 && ny == 1, "raycast down hits top face at y=255");
    hit = RaycastDDA(get, 8, 300, 8, 0, 1, 0, 500, hx, hy, hz, nx, ny, nz);
    CHECK(!hit, "raycast up misses");

    // --- Atlas ---
    Atlas atlas;
    uint8_t red[16 * 16 * 4], blue[16 * 16 * 4];
    for (int i = 0; i < 256; ++i) {
        red[i * 4] = 255; red[i * 4 + 1] = 0; red[i * 4 + 2] = 0; red[i * 4 + 3] = 255;
        blue[i * 4] = 0; blue[i * 4 + 1] = 0; blue[i * 4 + 2] = 255; blue[i * 4 + 3] = 255;
    }
    const int t0 = atlas.AddCheckerTile("__missing");
    const int t1 = atlas.AddTile("red", red);
    const int t2 = atlas.AddTile("blue", blue);
    CHECK(t0 == 0 && t1 == 1 && t2 == 2, "atlas tile ids sequential");
    const uint8_t* px = atlas.Pixels();
    CHECK(px[0] == 255 && px[2] == 255, "checker fallback occupies tile 0");
    CHECK(px[16 * 4] == 255 && px[16 * 4 + 2] == 0, "red tile lands at x=16");
    CHECK(px[32 * 4 + 2] == 255, "blue tile lands at x=32");
    float u = 0, v = 0;
    CHECK(atlas.GetTileOrigin(t2, u, v) && u == 32.0f / 1024.0f && v == 0.0f, "tile origin UV");
    CHECK(atlas.FindTile("blue") == t2 && atlas.FindTile("nope") == -1, "tile name lookup");
    CHECK(atlas.AddTile("red", red) == t1, "duplicate tile dedup");

    // --- 1.9+ combat cooldown math ---
    AttackCooldown cd;
    CHECK(cd.Strength(1000.0f) == 1.0f, "fully charged after idle");
    const float fullDmg = cd.Swing(1000.0f);
    CHECK(fullDmg == 1.0f, "full-strength swing deals base damage");
    const float spamDmg = cd.Swing(1000.0f);
    CHECK(spamDmg < 0.25f, "instant re-swing suffers the damage penalty");
    CHECK(cd.Strength(1200.0f) == 0.9f, "strength 0.9 at 200ms (crit gate boundary)");
    CHECK(cd.Strength(1250.0f) == 1.0f, "recharged after one 250ms period");
    PlayerState ps;
    ps.UpdateCombat(cd, 1100.0f);
    CHECK(ps.attackCooldownProgress == cd.Strength(1100.0f),
          "player cooldown progress mirrors QPC-driven strength");

    // --- Rigid ring queue ---
    RingQueue<int, 4> ring;
    CHECK(ring.TryPush(1) && ring.TryPush(2) && ring.TryPush(3) && ring.TryPush(4),
          "ring fills to capacity");
    CHECK(!ring.TryPush(5), "ring rejects overflow (backpressure, no allocation)");
    int rv = 0;
    CHECK(ring.TryPop(rv) && rv == 1 && ring.TryPop(rv) && rv == 2, "ring pops FIFO");
    CHECK(ring.TryPush(5) && ring.TryPush(6), "ring wrap-around reuses slots");
    CHECK(ring.TryPop(rv) && rv == 3 && ring.TryPop(rv) && rv == 4 &&
          ring.TryPop(rv) && rv == 5 && ring.TryPop(rv) && rv == 6, "ring drains in order");
    CHECK(!ring.TryPop(rv), "empty ring pop fails");

    // --- Crit particle sweep ---
    ParticleSystem parts;
    parts.SpawnCrit(0, 70, 0);
    CHECK(parts.List().size() == 16, "crit spawns 16 particles");
    for (int i = 0; i < 100; ++i) parts.SpawnCrit(0, 70, 0);
    CHECK(parts.List().size() <= MAX_PARTICLES, "particle count hard-capped");
    parts.Update(2.0f);
    CHECK(parts.List().empty(), "expired particles are swept");

    // --- Block flags ---
    CHECK(BLOCK_FLAGS[BLOCK_AIR] == 0, "air has no flags");
    CHECK((BLOCK_FLAGS[BLOCK_WATER] & BF_LIQUID) && !(BLOCK_FLAGS[BLOCK_WATER] & BF_SOLID),
          "water is liquid and non-solid");
    CHECK((BLOCK_FLAGS[BLOCK_OAK_LEAVES] & BF_CUTOUT) && (BLOCK_FLAGS[BLOCK_OAK_LEAVES] & BF_SOLID),
          "leaves are solid cutout");
    CHECK((BLOCK_FLAGS[BLOCK_STONE] & BF_OPAQUE) && (BLOCK_FLAGS[BLOCK_STONE] & BF_SOLID),
          "stone is solid opaque");
    CHECK((BLOCK_FLAGS[BLOCK_GLASS] & BF_CUTOUT) && !(BLOCK_FLAGS[BLOCK_GLASS] & BF_OPAQUE),
          "glass is cutout, not opaque");

    // --- Greedy mesher emits all six face directions ---
    {
        std::vector<uint8_t> cube(CHUNK_VOL, 0);
        for (int y = 0; y < 16; ++y)
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x)
                    cube[x + 16 * z + 256 * y] = BLOCK_STONE;
        BlockTiles bt;
        MeshData cm;
        BuildChunkMeshCPU(cube.data(), nullptr, nullptr, nullptr, nullptr, bt, cm);
        CHECK(cm.vertices.size() == 24, "solid cube meshes to exactly 6 quads");
        CHECK(cm.indices.size() == 36, "solid cube index count");
    }

    // --- Liquid mesh partition ---
    std::vector<uint8_t> water(CHUNK_VOL, 0);
    water[5 + 16 * 5 + 256 * 64] = BLOCK_WATER;
    MeshData wm;
    BuildChunkMeshCPU(water.data(), nullptr, nullptr, nullptr, nullptr, tiles, wm);
    CHECK(wm.opaqueIndexCount == 0 && wm.indices.size() == 36,
          "isolated water block meshes to 6 liquid quads (opaque count 0)");

    // --- Player physics: gravity, landing, jump ---
    {
        InputState in;
        FlyCamera cam;
        PlayerBody body;
        body.flying = false;
        body.x = 8.5f; body.y = 70.0f; body.z = 8.5f;
        auto ground = [](int, int y, int) -> uint8_t {
            return (y >= 0 && y < 64) ? BLOCK_STONE : BLOCK_AIR;
        };
        for (int i = 0; i < 240; ++i) body.Update(in, 1.0f / 60.0f, cam, ground);
        CHECK(body.onGround && fabsf(body.y - 64.0f) < 0.05f,
              "player falls under gravity and lands on ground at y=64");
        in.keys[VK_SPACE] = true;
        body.Update(in, 1.0f / 60.0f, cam, ground);
        CHECK(body.y > 64.0f, "jump lifts the player off the ground");
    }

    // --- Terrain generation (real async World) ---
    {
        World world(1337, 2);
        world.SetBlockTiles(tiles);
        world.Update(8.5f, 8.5f);
        int sy = -1;
        for (int i = 0; i < 300 && sy < 0; ++i) {
            sy = world.SurfaceY(8, 8);
            if (sy < 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(sy > 0, "terrain generates a surface near spawn");
        CHECK(world.GetBlock(8, 0, 8) == BLOCK_BEDROCK, "bedrock floor at y=0");
        if (sy > 0) CHECK((BLOCK_FLAGS[world.GetBlock(8, sy, 8)] & BF_SOLID) != 0,
                          "surface block is solid");
        std::vector<uint8_t> cb(CHUNK_VOL);
        for (int y = 0; y < CHUNK_Y; ++y)
            for (int z = 0; z < CHUNK_Z; ++z)
                for (int x = 0; x < CHUNK_X; ++x)
                    cb[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y] = world.GetBlock(x, y, z);
        MeshData gm;
        BuildChunkMeshCPU(cb.data(), nullptr, nullptr, nullptr, nullptr, tiles, gm);
        int qy = 0, qx = 0, qz = 0;
        for (size_t q = 0; q + 3 < gm.vertices.size(); q += 4) {
            const MeshVertex& a = gm.vertices[q];
            const MeshVertex& b = gm.vertices[q + 1];
            const MeshVertex& c = gm.vertices[q + 2];
            if (a.y == b.y && b.y == c.y) ++qy;
            else if (a.x == b.x && b.x == c.x) ++qx;
            else ++qz;
        }
        printf("generated chunk mesh: %zu verts, %zu idx, Y=%d X=%d Z=%d\n",
               gm.vertices.size(), gm.indices.size(), qy, qx, qz);
        int exposedTop = 0;
        for (int z = 0; z < CHUNK_Z; ++z) {
            for (int x = 0; x < CHUNK_X; ++x) {
                for (int y = 0; y < CHUNK_Y - 1; ++y) {
                    const uint8_t a = cb[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y];
                    const uint8_t b = cb[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * (y + 1)];
                    if ((BLOCK_FLAGS[a] & BF_SOLID) && !(BLOCK_FLAGS[b] & BF_SOLID)) ++exposedTop;
                }
            }
        }
        printf("exposed top cells: %d\n", exposedTop);
        CHECK(qy > 0 && qy * 256 >= exposedTop, "greedy top quads cover the exposed surface");
        CHECK(qx + qz > 0, "generated chunk meshes side faces");
    }

    // --- Seed determinism: same seed must rebuild the identical column ---
    {
        World a(424242, 2), b(424242, 2);
        a.SetBlockTiles(tiles);
        b.SetBlockTiles(tiles);
        a.Update(8.5f, 8.5f);
        b.Update(8.5f, 8.5f);
        int sa = -1, sb = -1;
        for (int i = 0; i < 300 && (sa < 0 || sb < 0); ++i) {
            if (sa < 0) sa = a.SurfaceY(8, 8);
            if (sb < 0) sb = b.SurfaceY(8, 8);
            if (sa < 0 || sb < 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(sa > 0 && sa == sb, "same seed produces the same spawn height");
        bool same = sa == sb;
        if (same) {
            for (int y = 0; y <= sa && same; ++y)
                same = a.GetBlock(8, y, 8) == b.GetBlock(8, y, 8);
        }
        CHECK(same, "same seed produces identical blocks (seed drives generation)");
    }

    if (gFail) {
        printf("\n%d TEST(S) FAILED\n", gFail);
        return 1;
    }
    printf("\nALL TESTS PASSED\n");
    return 0;
}