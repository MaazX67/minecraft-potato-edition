#include "assets.h"
#include "engine.h"
#include "world.h"
#include "shader_embed.h"
#include <d3dcompiler.h>
#include <algorithm>
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

static void TestShaders() {
    struct Shader { const char* entry; const char* profile; };
    const Shader shaders[] = {
        {"VSMain", "vs_4_0"}, {"PSMain", "ps_4_0"},
        {"PSMainUI", "ps_4_0"}, {"PSMainSky", "ps_4_0"}
    };
    for (const Shader& shader : shaders) {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT result = D3DCompile(gShaderSource, strlen(gShaderSource), "shader.fx",
            nullptr, nullptr, shader.entry, shader.profile, D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &code, &errors);
        if (FAILED(result) && errors)
            fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
        char label[128];
        snprintf(label, sizeof(label), "embedded %s compiles as %s", shader.entry, shader.profile);
        CHECK(SUCCEEDED(result) && code && code->GetBufferSize() > 0, label);
        if (errors) errors->Release();
        if (code) code->Release();
    }
}

static void TestFrusta() {
    using namespace DirectX;
    Frustum unit;
    ExtractFrustum(XMMatrixIdentity(), unit);
    CHECK(AABBInFrustum(unit, -1, -1, 0, 1, 1, 1), "identity frustum includes D3D clip volume boundaries");
    CHECK(!AABBInFrustum(unit, 0, 0, -0.1f, 0, 0, -0.1f), "D3D near plane rejects negative clip Z");
    CHECK(!AABBInFrustum(unit, 0, 0, 1.1f, 0, 0, 1.1f), "D3D far plane rejects Z greater than W");

    struct CameraCase { float x, y, z, yaw, pitch; };
    const CameraCase cameras[] = {
        {0, 0, 0, 0, 0},
        {8.5f, 65.62f, 8.5f, -28, -19.5f},
        {8.5f, 65.62f, 8.5f, 0, 0},
        {8.5f, 65.62f, 8.5f, 90, 0},
        {8.5f, 65.62f, 8.5f, 180, 0},
        {8.5f, 65.62f, 8.5f, -90, 0},
        {-47, 92, 133, 0, 90},
        {-47, 92, 133, 0, -90},
        {-123.5f, 180.25f, -41.75f, 133, 27},
    };
    const float aspect = 1366.0f / 768.0f;
    const float fov = XMConvertToRadians(70.0f);
    const XMMATRIX projection = XMMatrixPerspectiveFovRH(fov, aspect, 0.1f, 1000.0f);
    bool normalized = true, visible = true, rejected = true, intersecting = true;
    bool planeEquivalent = true, pointEquivalent = true;
    int insidePoints = 0, outsidePoints = 0;
    for (const CameraCase& camera : cameras) {
        const float yaw = XMConvertToRadians(camera.yaw), pitch = XMConvertToRadians(camera.pitch);
        const XMVECTOR eye = XMVectorSet(camera.x, camera.y, camera.z, 1);
        const XMVECTOR forward = XMVectorSet(sinf(yaw) * cosf(pitch), sinf(pitch),
                                             -cosf(yaw) * cosf(pitch), 0);
        const XMVECTOR up = fabsf(camera.pitch) == 90.0f ? XMVectorSet(0, 0, 1, 0)
                                                                       : XMVectorSet(0, 1, 0, 0);
        const XMMATRIX view = XMMatrixLookAtRH(eye, XMVectorAdd(eye, forward), up);
        const XMMATRIX vp = XMMatrixMultiply(view, projection);
        const XMMATRIX inverseView = XMMatrixInverse(nullptr, view);
        Frustum frustum;
        ExtractFrustum(vp, frustum);
        for (const XMFLOAT4& plane : frustum.planes) {
            const float length = sqrtf(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
            if (fabsf(length - 1.0f) > 1e-5f) normalized = false;
        }
        auto worldPoint = [&](float x, float y, float depth) {
            XMFLOAT3 point;
            XMStoreFloat3(&point, XMVector3TransformCoord(XMVectorSet(x, y, -depth, 1), inverseView));
            return point;
        };
        auto boxVisible = [&](float x, float y, float depth, float half) {
            const XMFLOAT3 p = worldPoint(x, y, depth);
            return AABBInFrustum(frustum, p.x - half, p.y - half, p.z - half,
                                 p.x + half, p.y + half, p.z + half);
        };
        if (!boxVisible(0, 0, 8, 0.25f) || !boxVisible(0, 0, 800, 1)) visible = false;
        if (boxVisible(0, 0, -8, 0.25f) || boxVisible(0, 0, 0.025f, 0.005f) ||
            boxVisible(0, 0, 1200, 1) || boxVisible(-40, 0, 8, 0.25f) ||
            boxVisible(40, 0, 8, 0.25f) || boxVisible(0, -40, 8, 0.25f) ||
            boxVisible(0, 40, 8, 0.25f)) rejected = false;
        if (!boxVisible(0, 0, 0.1f, 0.025f) || !boxVisible(0, 0, 1000, 2)) intersecting = false;
        if (camera.x == 8.5f) {
            CHECK(AABBInFrustum(frustum, 0, 0, 0, float(CHUNK_X), float(CHUNK_Y), float(CHUNK_Z)),
                  "spawn chunk remains visible for pitched and cardinal cameras at (8.5,65.62,8.5)");
        }
        const float depths[] = {-0.5f, 0.025f, 0.2f, 4, 100, 800, 1200};
        const float offsets[] = {-2, -0.75f, 0, 0.75f, 2};
        for (float depth : depths) {
            for (float ox : offsets) {
                for (float oy : offsets) {
                    const float height = depth * tanf(fov * 0.5f);
                    const XMFLOAT3 p = worldPoint(ox * height * aspect, oy * height, depth);
                    XMFLOAT4 clip;
                    XMStoreFloat4(&clip, XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), vp));
                    const float distances[6] = {clip.w + clip.x, clip.w - clip.x,
                        clip.w + clip.y, clip.w - clip.y, clip.z, clip.w - clip.z};
                    bool clipInside = true;
                    for (int i = 0; i < 6; ++i) {
                        const XMFLOAT4& plane = frustum.planes[i];
                        const float distance = plane.x * p.x + plane.y * p.y + plane.z * p.z + plane.w;
                        if ((distance >= 0) != (distances[i] >= 0)) planeEquivalent = false;
                        if (distances[i] < 0) clipInside = false;
                    }
                    if (clipInside) ++insidePoints; else ++outsidePoints;
                    if (AABBInFrustum(frustum, p.x, p.y, p.z, p.x, p.y, p.z) != clipInside)
                        pointEquivalent = false;
                }
            }
        }
    }
    CHECK(normalized, "translated and rotated frusta have normalized planes");
    CHECK(visible, "forward AABBs are visible in all six cardinal and oblique orientations");
    CHECK(rejected, "AABBs behind, before near, beyond far and outside each side are rejected");
    CHECK(intersecting, "AABBs crossing near and far planes remain visible");
    CHECK(planeEquivalent, "all six extracted plane signs match homogeneous D3D clip inequalities");
    CHECK(pointEquivalent && insidePoints > 0 && outsidePoints > 0,
          "point AABB visibility matches direct clip-space tests for translated and rotated cameras");
}

static void CheckBoxMesh(const MeshData& mesh, float minX, float minY, float minZ,
                         float maxX, float maxY, float maxZ) {
    CHECK(mesh.vertices.size() == 24 && mesh.indices.size() == 36, "box mesh has six indexed quads");
    if (mesh.vertices.size() != 24 || mesh.indices.size() != 36) return;
    const float lo[3] = {minX, minY, minZ}, hi[3] = {maxX, maxY, maxZ};
    int faces[6] = {};
    bool winding = true, vertical = true, horizontal = true, extents = true, bounds = true;
    for (size_t q = 0; q < mesh.vertices.size(); q += 4) {
        const MeshVertex* v = &mesh.vertices[q];
        int axis = -1;
        for (int d = 0; d < 3; ++d) {
            const float position = d == 0 ? v[0].x : (d == 1 ? v[0].y : v[0].z);
            bool planar = true;
            for (int k = 1; k < 4; ++k) {
                if ((d == 0 ? v[k].x : (d == 1 ? v[k].y : v[k].z)) != position) planar = false;
            }
            if (planar) axis = d;
        }
        if (axis < 0) { bounds = false; continue; }
        const float position = axis == 0 ? v[0].x : (axis == 1 ? v[0].y : v[0].z);
        const int sign = position == lo[axis] ? -1 : (position == hi[axis] ? 1 : 0);
        if (sign == 0) { bounds = false; continue; }
        ++faces[axis * 2 + (sign > 0 ? 1 : 0)];
        for (int t = 0; t < 6; t += 3) {
            const size_t index = q / 4 * 6 + t;
            const uint32_t ia = mesh.indices[index], ib = mesh.indices[index + 1], ic = mesh.indices[index + 2];
            if (ia < q || ia >= q + 4 || ib < q || ib >= q + 4 || ic < q || ic >= q + 4) {
                winding = false;
                continue;
            }
            const MeshVertex& a = mesh.vertices[ia];
            const MeshVertex& b = mesh.vertices[ib];
            const MeshVertex& c = mesh.vertices[ic];
            const float ab[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
            const float ac[3] = {c.x - a.x, c.y - a.y, c.z - a.z};
            const float normal[3] = {ab[1] * ac[2] - ab[2] * ac[1],
                ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]};
            if (normal[axis] * sign <= 0 || normal[(axis + 1) % 3] != 0 ||
                normal[(axis + 2) % 3] != 0) winding = false;
        }
        float minU = v[0].u, maxU = v[0].u, minV = v[0].v, maxV = v[0].v;
        for (int k = 0; k < 4; ++k) {
            const float p[3] = {v[k].x, v[k].y, v[k].z};
            for (int d = 0; d < 3; ++d) if (p[d] != lo[d] && p[d] != hi[d]) bounds = false;
            minU = std::min(minU, v[k].u); maxU = std::max(maxU, v[k].u);
            minV = std::min(minV, v[k].v); maxV = std::max(maxV, v[k].v);
            for (int j = 0; j < 4; ++j) {
                const float dx = v[j].x - v[k].x, dy = v[j].y - v[k].y, dz = v[j].z - v[k].z;
                const float du = v[j].u - v[k].u, dv = v[j].v - v[k].v;
                if (axis != 1 && dv != -dy) vertical = false;
                const float expectedU = axis == 0 ? -sign * dz : sign * (axis == 1 ? dz : dx);
                if (du != expectedU) horizontal = false;
            }
        }
        const float width = axis == 2 ? maxX - minX : maxZ - minZ;
        const float height = axis == 1 ? maxX - minX : maxY - minY;
        if (minU != 0 || minV != 0 || maxU != width || maxV != height) extents = false;
    }
    for (int count : faces) if (count != 1) bounds = false;
    CHECK(bounds, "box has exactly one quad on each positive and negative face");
    CHECK(winding, "both indexed triangles on every box face have outward cross products");
    CHECK(vertical, "side-face V decreases one tile per block as Y increases on both X and Z faces");
    CHECK(horizontal, "face U follows horizontal distance with consistent opposite-face mirroring");
    CHECK(extents, "merged quad UV extents preserve one tile per block on both axes");
}

int main() {
    TestShaders();
    TestFrusta();

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
    CheckBoxMesh(md, 0, 0, 0, float(CHUNK_X), float(CHUNK_Y), float(CHUNK_Z));

    std::vector<uint8_t> one(CHUNK_VOL, 0);
    one[5 + 16 * 5 + 256 * 64] = BLOCK_STONE;
    BuildChunkMeshCPU(one.data(), nullptr, nullptr, nullptr, nullptr, tiles, md);
    CHECK(md.vertices.size() == 24, "isolated voxel -> 6 quads");
    bool inPlace = true;
    for (const MeshVertex& v : md.vertices) {
        if (v.x < 5 || v.x > 6 || v.y < 64 || v.y > 65 || v.z < 5 || v.z > 6) inPlace = false;
    }
    if (!inPlace) {
        for (size_t i = 0; i < md.vertices.size(); i += 4) {
            const MeshVertex& v = md.vertices[i];
            printf("quad %zu corner: %.1f %.1f %.1f\n", i / 4, double(v.x), double(v.y), double(v.z));
        }
    }
    CHECK(inPlace, "voxel quads sit on the voxel (no plane-origin drift)");
    CheckBoxMesh(md, 5, 64, 5, 6, 65, 6);

    std::vector<uint8_t> two(CHUNK_VOL, 0);
    two[5 + 16 * 5 + 256 * 64] = BLOCK_STONE;
    two[6 + 16 * 5 + 256 * 64] = BLOCK_STONE;
    BuildChunkMeshCPU(two.data(), nullptr, nullptr, nullptr, nullptr, tiles, md);
    CHECK(md.vertices.size() == 24, "two adjacent voxels merge to 6 quads (10 faces -> 6)");
    CheckBoxMesh(md, 5, 64, 5, 7, 65, 6);

    {
        std::vector<uint8_t> prism(CHUNK_VOL, BLOCK_AIR);
        for (int y = 60; y < 65; ++y)
            for (int z = 4; z < 11; ++z)
                for (int x = 3; x < 6; ++x)
                    prism[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y] = BLOCK_GRASS;
        MeshData merged;
        BuildChunkMeshCPU(prism.data(), nullptr, nullptr, nullptr, nullptr, tiles, merged);
        CHECK(merged.vertices.size() == 24 && merged.indices.size() == 36,
              "3x5x7 grass prism merges to six non-square quads");
        CheckBoxMesh(merged, 3, 60, 4, 6, 65, 11);
    }

    // --- Raycast ---
    auto get = [&](int, int y, int) -> uint8_t {
        return (y >= 0 && y < CHUNK_Y) ? BLOCK_STONE : 0;
    };
    int hx, hy, hz, nx, ny, nz;
    bool hit = RaycastDDA(get, 8, 300, 8, 0, -1, 0, 500, hx, hy, hz, nx, ny, nz);
    CHECK(hit && hy == CHUNK_Y - 1 && ny == 1, "raycast down hits top face at y=255");
    hit = RaycastDDA(get, 8, 300, 8, 0, 1, 0, 500, hx, hy, hz, nx, ny, nz);
    CHECK(!hit, "raycast up misses");

    {
        auto liquidThenStone = [](int, int y, int) -> uint8_t {
            if (y == 65) return BLOCK_WATER;
            return y == 64 ? BLOCK_STONE : BLOCK_AIR;
        };
        hit = RaycastDDA(liquidThenStone, 8, 70, 8, 0, -1, 0, 10, hx, hy, hz, nx, ny, nz);
        CHECK(hit && hy == 64, "selection ray skips water and hits the solid block below");
    }

    {
        InventoryState inv;
        CHECK(inv.Add(BLOCK_LOG, 65) == 0 && inv.slots[27].count == 64 && inv.slots[28].count == 1,
              "pickup fills hotbar first and caps stacks at 64");
        CHECK(inv.Click(28, true, false) && inv.cursor.count == 1 && inv.slots[28].id == 0,
              "right-clicking one item clears the emptied slot id");
        CHECK(inv.Click(36, false, false) && inv.cursor.Empty(), "move log into 2x2 crafting grid");
        CHECK(inv.Result(false).id == BLOCK_PLANKS && inv.Result(false).count == 4,
              "one log previews four planks");
        CHECK(inv.Click(45, false, false) && inv.cursor.count == 4 && inv.craft[0].id == 0,
              "taking output consumes and clears the ingredient");
        CHECK(inv.Result(false).Empty() && !inv.Click(45, false, false) && inv.cursor.count == 4,
              "repeated output click cannot duplicate exhausted ingredients");
        CHECK(inv.Click(28, false, false), "place crafted stack in hotbar");
        inv.slots[0] = {BLOCK_PLANKS, 63};
        CHECK(inv.Click(28, false, false, true) && inv.slots[0].count == 64 && inv.slots[1].count == 3,
              "shift-click merges before filling empty storage cells");
        CHECK(inv.Click(1, true, false) && inv.cursor.count == 2 && inv.slots[1].count == 1,
              "right-click splits an odd stack with ceiling half on cursor");
        CHECK(inv.Click(2, true, false) && inv.slots[2].count == 1 && inv.cursor.count == 1,
              "right-click places exactly one item");
        CHECK(inv.Close() && inv.cursor.Empty() && inv.craft[0].Empty(),
              "closing inventory returns cursor and crafting items");
    }
    {
        InventoryState inv;
        inv.craft[0] = {BLOCK_PLANKS, 1};
        inv.craft[1] = {BLOCK_SPRUCE_PLANKS, 1};
        inv.craft[3] = {BLOCK_BIRCH_PLANKS, 1};
        inv.craft[4] = {BLOCK_PLANKS, 1};
        CHECK(inv.Result(false).id == BLOCK_CRAFTING_TABLE,
              "crafting table accepts mixed plank species");
        inv.cursor = {BLOCK_DIRT, 1};
        CHECK(!inv.Click(45, false, false) && inv.craft[0].count == 1,
              "incompatible cursor does not consume ingredients");
        inv.cursor = {BLOCK_CRAFTING_TABLE, 64};
        CHECK(!inv.Click(45, false, false) && inv.craft[4].count == 1,
              "full cursor does not consume ingredients");
        inv.cursor = {};
        CHECK(inv.Click(45, true, false) && inv.cursor.count == 1 && inv.Result(false).Empty(),
              "right-clicking output crafts exactly once");
        inv = InventoryState{};
        inv.craft[4] = {BLOCK_LOG, 2};
        CHECK(inv.Result(false).id == BLOCK_PLANKS, "single-cell recipes work away from grid origin");
        inv.craft[4] = {BLOCK_LOG, 0};
        CHECK(inv.Result(false).Empty(), "zero-count slot never participates in a recipe");
        CHECK(!inv.Click(-1, false, false) && !inv.Click(46, false, false) && !inv.Click(38, false, false),
              "outside and hidden 2x2 cells cannot be clicked");
    }
    {
        InventoryState inv;
        for (int i = 0; i < 9; ++i) if (i != 4) inv.craft[i] = {BLOCK_COBBLE, 2};
        CHECK(inv.Result(false).Empty() && inv.Result(true).id == BLOCK_FURNACE,
              "furnace recipe requires the 3x3 crafting table");
        inv.craft[4] = {BLOCK_COBBLE, 1};
        CHECK(inv.Result(true).Empty(), "filled center invalidates furnace recipe");
        inv.craft[4] = {};
        CHECK(inv.Click(45, false, true, true) && inv.slots[27].id == BLOCK_FURNACE &&
              inv.slots[27].count == 2 && inv.Result(true).Empty(),
              "shift-click crafts only as many furnaces as ingredients allow");
    }
    {
        InventoryState inv;
        for (ItemSlot& slot : inv.slots) slot = {BLOCK_STONE, 64};
        CHECK(inv.Add(BLOCK_DIRT, 5) == 5 && inv.Capacity(BLOCK_DIRT) == 0,
              "full inventory returns uninserted items instead of deleting them");
        inv.cursor = {BLOCK_LOG, 1};
        inv.craft[0] = {BLOCK_LOG, 1};
        CHECK(!inv.Close() && inv.cursor.count == 1 && inv.craft[0].count == 1,
              "failed close preserves cursor and crafting grid");
        CHECK(!inv.Click(45, false, false, true) && inv.craft[0].count == 1,
              "shift-crafting into a full inventory consumes nothing");
        inv.slots[0] = {BLOCK_PLANKS, 61};
        CHECK(!inv.Click(45, false, false, true) && inv.slots[0].count == 61,
              "craft output is atomic when only part of a batch would fit");
        inv.slots[0].count = 60;
        CHECK(inv.Click(45, false, false, true) && inv.slots[0].count == 64 && inv.craft[0].Empty(),
              "craft output succeeds at exact available capacity");
        inv.craft[0] = {BLOCK_LOG, 1};
        inv.slots[1] = {BLOCK_LOG, 63};
        CHECK(!inv.Close() && inv.slots[1].count == 63 && inv.cursor.count == 1 && inv.craft[0].count == 1,
              "close is all-or-nothing when only cursor fits");
    }
    {
        InventoryState inv;
        inv.slots[0] = {BLOCK_DIRT, 37};
        inv.slots[27] = {BLOCK_LOG, 64};
        inv.slots[28] = {BLOCK_STONE, 13};
        std::mt19937 rng(451);
        bool conserved = true;
        for (int n = 0; n < 2000; ++n) {
            const bool table = (rng() & 1) != 0;
            inv.Click(int(rng() % 45), (rng() & 1) != 0, table, (rng() & 1) != 0);
            if (n % 11 == 0) inv.Close();
            int totals[BLOCK_COUNT] = {};
            auto countSlot = [&](const ItemSlot& slot) {
                if (slot.Empty()) { if (slot.id != 0 || slot.count != 0) conserved = false; }
                else {
                    if (slot.count > 64 || slot.id >= BLOCK_COUNT) conserved = false;
                    else totals[slot.id] += slot.count;
                }
            };
            for (const ItemSlot& slot : inv.slots) countSlot(slot);
            for (const ItemSlot& slot : inv.craft) countSlot(slot);
            countSlot(inv.cursor);
            if (totals[BLOCK_DIRT] != 37 || totals[BLOCK_LOG] != 64 || totals[BLOCK_STONE] != 13)
                conserved = false;
        }
        CHECK(conserved, "2000 randomized slot operations preserve items and canonical stack bounds");
    }
    {
        SurvivalState easy, normal, hard, peaceful;
        easy.hunger = normal.hunger = hard.hunger = peaceful.hunger = 0;
        peaceful.health = 5;
        for (int tick = 0; tick < 2000; ++tick) {
            easy.Update(0.1f, false, false, 1);
            normal.Update(0.1f, false, false, 2);
            hard.Update(0.1f, false, false, 3);
            peaceful.Update(0.1f, false, false, 0);
        }
        CHECK(easy.health == 10 && normal.health == 1 && hard.dead && hard.health == 0,
              "difficulty controls starvation floor");
        CHECK(peaceful.health == 20 && peaceful.hunger == 20 && !peaceful.dead,
              "peaceful regenerates health without hunger depletion");
    }

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
    CheckBoxMesh(wm, 5, 64, 5, 6, 65, 6);

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

    {
        EditMap changes;
        changes[ChunkKey(-2, 3)][0] = BLOCK_AIR;
        changes[ChunkKey(-2, 3)][65535] = BLOCK_BRICK;
        changes[ChunkKey(0, -1)][32888] = BLOCK_FURNACE;
        const auto bytes = EncodeEdits(changes);
        EditMap loaded;
        CHECK(DecodeEdits(bytes.data(), bytes.size(), loaded) && loaded == changes,
              "block edits round-trip with negative chunks, air and boundary indices");
        CHECK(EncodeEdits(loaded) == bytes, "block edit encoding is deterministic");
        bool truncatedRejected = true;
        for (size_t i = 0; i < bytes.size(); ++i)
            if (DecodeEdits(bytes.data(), i, loaded)) truncatedRejected = false;
        CHECK(truncatedRejected && loaded == changes,
              "all truncated edit streams are rejected without replacing valid edits");
        auto invalid = bytes;
        invalid.back() = 255;
        CHECK(!DecodeEdits(invalid.data(), invalid.size(), loaded), "unknown saved block ids are rejected");
        invalid = bytes;
        invalid.push_back(0);
        CHECK(!DecodeEdits(invalid.data(), invalid.size(), loaded), "trailing saved block bytes are rejected");
        invalid = bytes;
        invalid[4] = 99;
        CHECK(!DecodeEdits(invalid.data(), invalid.size(), loaded), "unsupported edit versions are rejected");
        const auto empty = EncodeEdits({});
        CHECK(DecodeEdits(empty.data(), empty.size(), loaded) && loaded.empty(), "empty world edits round-trip");
    }
    {
        World edited(1337, 2);
        edited.SetBlockTiles(tiles);
        auto waitFor = [&](int cx, int cz) {
            edited.Update(float(cx * 16 + 8), float(cz * 16 + 8));
            for (int i = 0; i < 500; ++i) {
                uint64_t key; MeshData mesh;
                while (edited.PopUpload(key, mesh)) {}
                if (edited.SurfaceY(cx * 16, cz * 16) >= 0) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        };
        CHECK(waitFor(0, 0), "edit fixture chunk is generated");
        edited.SetBlock(1, 0, 1, BLOCK_AIR);
        edited.SetBlock(2, 100, 2, BLOCK_FURNACE);
        const auto saved = edited.SnapshotEdits();
        CHECK(saved.at(ChunkKey(0, 0)).size() == 2, "player edits track removed and placed blocks");
        CHECK(waitFor(20, 20), "streaming reaches a distant chunk");
        CHECK(!edited.HasChunk(0, 0), "edit fixture chunk is evicted");
        CHECK(waitFor(0, 0) && edited.GetBlock(1, 0, 1) == BLOCK_AIR &&
              edited.GetBlock(2, 100, 2) == BLOCK_FURNACE,
              "player edits survive eviction and regeneration");
        World restored(1337, 2);
        restored.SetBlockTiles(tiles);
        restored.SetEdits(saved);
        restored.Update(8.5f, 8.5f);
        for (int i = 0; i < 500 && restored.SurfaceY(8, 8) < 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(restored.GetBlock(1, 0, 1) == BLOCK_AIR && restored.GetBlock(2, 100, 2) == BLOCK_FURNACE,
              "saved edits apply to a fresh world before meshing");
    }

    if (gFail) {
        printf("\n%d TEST(S) FAILED\n", gFail);
        return 1;
    }
    printf("\nALL TESTS PASSED\n");
    return 0;
}