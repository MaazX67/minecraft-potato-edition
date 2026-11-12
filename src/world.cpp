#include "world.h"
#include "assets.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <random>

namespace potato {

// ---------------------------------------------------------------------------
// Improved Perlin noise (2D), fixed-seed permutation
// ---------------------------------------------------------------------------

static float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
static float LerpN(float t, float a, float b) { return a + t * (b - a); }
static float Grad2(int h, float x, float y) {
    switch (h & 7) {
        case 0: return  x + y;
        case 1: return -x + y;
        case 2: return  x - y;
        case 3: return -x - y;
        case 4: return  x;
        case 5: return -x;
        case 6: return  y;
        default: return -y;
    }
}

PerlinNoise::PerlinNoise(uint32_t seed) {
    uint8_t base[256];
    std::iota(base, base + 256, 0);
    std::mt19937 rng(seed);
    std::shuffle(base, base + 256, rng);
    for (int i = 0; i < 512; ++i) mP[i] = base[i & 255];
}

float PerlinNoise::Noise2(float x, float y) const {
    const int X = int(floorf(x)) & 255, Y = int(floorf(y)) & 255;
    x -= floorf(x); y -= floorf(y);
    const float u = Fade(x), v = Fade(y);
    const int A = mP[X] + Y, B = mP[X + 1] + Y;
    return LerpN(v,
        LerpN(u, Grad2(mP[A], x, y),         Grad2(mP[B], x - 1, y)),
        LerpN(u, Grad2(mP[A + 1], x, y - 1), Grad2(mP[B + 1], x - 1, y - 1)));
}

float PerlinNoise::Fbm2(float x, float y, int octaves) const {
    float sum = 0, amp = 1, freq = 1, norm = 0;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * Noise2(x * freq, y * freq);
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum / norm;
}

static float Grad3(int h, float x, float y, float z) {
    switch (h & 15) {
        case 0: return  x + y;
        case 1: return -x + y;
        case 2: return  x - y;
        case 3: return -x - y;
        case 4: return  x + z;
        case 5: return -x + z;
        case 6: return  x - z;
        case 7: return -x - z;
        case 8: return  y + z;
        case 9: return -y + z;
        case 10: return  y - z;
        case 11: return -y - z;
        case 12: return  y + x;
        case 13: return -y + z;
        case 14: return  y - x;
        default: return -y - z;
    }
}

float PerlinNoise::Noise3(float x, float y, float z) const {
    const int X = int(floorf(x)) & 255, Y = int(floorf(y)) & 255, Z = int(floorf(z)) & 255;
    x -= floorf(x); y -= floorf(y); z -= floorf(z);
    const float u = Fade(x), v = Fade(y), w = Fade(z);
    const int A = mP[X] + Y, AA = mP[A] + Z, AB = mP[A + 1] + Z;
    const int B = mP[X + 1] + Y, BA = mP[B] + Z, BB = mP[B + 1] + Z;
    return LerpN(w,
        LerpN(v,
            LerpN(u, Grad3(mP[AA], x, y, z),         Grad3(mP[BA], x - 1, y, z)),
            LerpN(u, Grad3(mP[AB], x, y - 1, z),     Grad3(mP[BB], x - 1, y - 1, z))),
        LerpN(v,
            LerpN(u, Grad3(mP[AA + 1], x, y, z - 1),     Grad3(mP[BA + 1], x - 1, y, z - 1)),
            LerpN(u, Grad3(mP[AB + 1], x, y - 1, z - 1), Grad3(mP[BB + 1], x - 1, y - 1, z - 1))));
}

float PerlinNoise::Fbm3(float x, float y, float z, int octaves) const {
    float sum = 0, amp = 1, freq = 1, norm = 0;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * Noise3(x * freq, y * freq, z * freq);
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum / norm;
}

// Deterministic per-voxel hash for ore/tree placement (stable across runs).
static uint32_t Hash3(uint32_t x, uint32_t y, uint32_t z, uint32_t seed) {
    uint32_t h = seed ^ (x * 374761393u) ^ (y * 668265263u) ^ (z * 2246822519u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// ---------------------------------------------------------------------------
// Greedy meshing (all three axes)
// ---------------------------------------------------------------------------

namespace {

struct MaskCell {
    int tile = -1;
    int light = 0;
    int sign = 0;
    bool liquid = false;
    bool Empty() const { return tile < 0; }
    bool operator==(const MaskCell& o) const {
        return tile == o.tile && light == o.light && sign == o.sign && liquid == o.liquid;
    }
};

int FaceTile(const BlockTiles& t, uint8_t b, int d, int sign) {
    if (d == 1) return sign > 0 ? t.top[b] : t.bottom[b];
    return t.side[b];
}

int FaceLight(int d, int sign) {
    if (d == 1) return sign > 0 ? 15 : 8;
    return d == 0 ? 12 : 10;
}

} // namespace

void BuildChunkMeshCPU(const uint8_t* blocks, const uint8_t* negX, const uint8_t* posX,
                       const uint8_t* negZ, const uint8_t* posZ, const BlockTiles& tiles,
                       MeshData& out) {
    out.vertices.clear();
    out.indices.clear();
    out.opaqueIndexCount = 0;
    MeshData liquid;
    const int dims[3] = {CHUNK_X, CHUNK_Y, CHUNK_Z};

    auto sample = [&](int x, int y, int z) -> uint8_t {
        if (y < 0 || y >= CHUNK_Y) return 0;
        if (x < 0)        return negX ? negX[(CHUNK_X - 1) + CHUNK_X * z + CHUNK_X * CHUNK_Z * y] : 0;
        if (x >= CHUNK_X) return posX ? posX[0 + CHUNK_X * z + CHUNK_X * CHUNK_Z * y] : 0;
        if (z < 0)        return negZ ? negZ[x + CHUNK_X * (CHUNK_Z - 1) + CHUNK_X * CHUNK_Z * y] : 0;
        if (z >= CHUNK_Z) return posZ ? posZ[x + CHUNK_X * 0 + CHUNK_X * CHUNK_Z * y] : 0;
        return blocks[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y];
    };

    for (int d = 0; d < 3; ++d) {
        const int u = (d + 1) % 3, v = (d + 2) % 3;
        int x[3] = {0, 0, 0}, q[3] = {0, 0, 0};
        q[d] = 1;
        const int dimU = dims[u], dimV = dims[v];
        std::vector<MaskCell> mask(size_t(dimU) * size_t(dimV));

        for (x[d] = -1; x[d] < dims[d]; ) {
            // Build the face mask between slice x[d] and x[d]+1.
            size_t n = 0;
            for (x[v] = 0; x[v] < dimV; ++x[v]) {
                for (x[u] = 0; x[u] < dimU; ++x[u]) {
                    const uint8_t a = sample(x[0], x[1], x[2]);
                    const uint8_t b = sample(x[0] + q[0], x[1] + q[1], x[2] + q[2]);
                    const bool aOpaque = (BLOCK_FLAGS[a] & BF_OPAQUE) != 0;
                    const bool bOpaque = (BLOCK_FLAGS[b] & BF_OPAQUE) != 0;
                    uint8_t blk = 0;
                    int sign = 0;
                    if (aOpaque != bOpaque) {
                        blk = aOpaque ? a : b;
                        sign = aOpaque ? 1 : -1;
                    } else if (!aOpaque && a != b && (a == 0 || b == 0)) {
                        // non-opaque vs air (water surface, glass, leaves)
                        blk = a != 0 ? a : b;
                        sign = a != 0 ? 1 : -1;
                    }
                    if (blk == 0) {
                        mask[n++] = MaskCell{};
                    } else {
                        mask[n].tile = FaceTile(tiles, blk, d, sign);
                        mask[n].light = FaceLight(d, sign);
                        mask[n].sign = sign;
                        mask[n].liquid = (BLOCK_FLAGS[blk] & BF_LIQUID) != 0;
                        ++n;
                    }
                }
            }
            ++x[d];

            // Greedy rectangle merge over the mask.
            n = 0;
            for (int j = 0; j < dimV; ++j) {
                for (int i = 0; i < dimU; ) {
                    const MaskCell cell = mask[n];
                    if (cell.Empty()) { ++i; ++n; continue; }

                    int w = 1;
                    while (i + w < dimU && mask[n + size_t(w)] == cell) ++w;
                    int h = 1;
                    while (j + h < dimV) {
                        bool rowOk = true;
                        for (int k = 0; k < w; ++k) {
                            if (!(mask[n + size_t(h) * size_t(dimU) + size_t(k)] == cell)) {
                                rowOk = false;
                                break;
                            }
                        }
                        if (!rowOk) break;
                        ++h;
                    }

                    // One quad on plane x[d], spanning w along u and h along v
                    // from mask cell (i, j). x[u]/x[v] are stale after the mask
                    // loops, so the origin is rebuilt from d, i and j.
                    float p[3] = {0, 0, 0};
                    p[d] = float(x[d]);
                    p[u] = float(i);
                    p[v] = float(j);
                    float e1[3] = {0, 0, 0}, e2[3] = {0, 0, 0};
                    e1[u] = float(w);
                    e2[v] = float(h);
                    float tileU = 0, tileV = 0;
                    if (cell.tile >= 0) {
                        tileU = float((cell.tile % ATLAS_COLS) * ATLAS_TILE) / float(ATLAS_SIZE);
                        tileV = float((cell.tile / ATLAS_COLS) * ATLAS_TILE) / float(ATLAS_SIZE);
                    }
                    MeshData& dst = cell.liquid ? liquid : out;
                    const uint32_t base = uint32_t(dst.vertices.size());
                    const float corners[4][2] = {
                        {0, 0}, {float(w), 0}, {float(w), float(h)}, {0, float(h)}
                    };
                    for (int c = 0; c < 4; ++c) {
                        const float fx = corners[c][0] / float(w);
                        const float fy = corners[c][1] / float(h);
                        MeshVertex vert;
                        vert.x = p[0] + e1[0] * fx + e2[0] * fy;
                        vert.y = p[1] + e1[1] * fx + e2[1] * fy;
                        vert.z = p[2] + e1[2] * fx + e2[2] * fy;
                        vert.tu = tileU;
                        vert.tv = tileV;
                        vert.u = corners[c][0];
                        vert.v = corners[c][1];
                        vert.light = float(cell.light);
                        vert.cr = vert.cg = vert.cb = 1.0f;
                        dst.vertices.push_back(vert);
                    }
                    dst.indices.push_back(base);
                    dst.indices.push_back(base + 1);
                    dst.indices.push_back(base + 2);
                    dst.indices.push_back(base);
                    dst.indices.push_back(base + 2);
                    dst.indices.push_back(base + 3);

                    for (int hh = 0; hh < h; ++hh)
                        for (int ww = 0; ww < w; ++ww)
                            mask[n + size_t(hh) * size_t(dimU) + size_t(ww)] = MaskCell{};
                    i += w;
                    n += size_t(w);
                }
            }
        }
    }

    // Liquid quads trail the opaque ones so the renderer can draw them in a
    // second blended pass with depth writes disabled.
    out.opaqueIndexCount = uint32_t(out.indices.size());
    const uint32_t vbase = uint32_t(out.vertices.size());
    out.vertices.insert(out.vertices.end(), liquid.vertices.begin(), liquid.vertices.end());
    for (const uint32_t idx : liquid.indices) out.indices.push_back(vbase + idx);
}

// ---------------------------------------------------------------------------
// Voxel raycast (Amanatides & Woo)
// ---------------------------------------------------------------------------

bool RaycastDDA(const std::function<uint8_t(int, int, int)>& getBlock,
                float ox, float oy, float oz, float dx, float dy, float dz, float maxDist,
                int& hx, int& hy, int& hz, int& nx, int& ny, int& nz) {
    const float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 1e-8f) return false;
    dx /= len; dy /= len; dz /= len;

    int x = int(floorf(ox)), y = int(floorf(oy)), z = int(floorf(oz));
    const int stepX = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    const int stepY = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    const int stepZ = dz > 0 ? 1 : (dz < 0 ? -1 : 0);
    const float tDeltaX = dx != 0 ? fabsf(1.0f / dx) : 1e30f;
    const float tDeltaY = dy != 0 ? fabsf(1.0f / dy) : 1e30f;
    const float tDeltaZ = dz != 0 ? fabsf(1.0f / dz) : 1e30f;
    float tMaxX = dx != 0 ? (dx > 0 ? float(x + 1) - ox : ox - float(x)) * tDeltaX : 1e30f;
    float tMaxY = dy != 0 ? (dy > 0 ? float(y + 1) - oy : oy - float(y)) * tDeltaY : 1e30f;
    float tMaxZ = dz != 0 ? (dz > 0 ? float(z + 1) - oz : oz - float(z)) * tDeltaZ : 1e30f;
    float t = 0;
    nx = ny = nz = 0;

    for (int i = 0; i < 512; ++i) {
        const uint8_t b = getBlock(x, y, z);
        if (b != 0 && !(BLOCK_FLAGS[b] & BF_LIQUID)) {
            hx = x; hy = y; hz = z;
            return true;
        }
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            x += stepX; t = tMaxX; tMaxX += tDeltaX;
            nx = -stepX; ny = 0; nz = 0;
        } else if (tMaxY < tMaxZ) {
            y += stepY; t = tMaxY; tMaxY += tDeltaY;
            nx = 0; ny = -stepY; nz = 0;
        } else {
            z += stepZ; t = tMaxZ; tMaxZ += tDeltaZ;
            nx = 0; ny = 0; nz = -stepZ;
        }
        if (t > maxDist) return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------

World::World(uint32_t seed, int renderDistance)
    : mSeed(seed), mRd(renderDistance), mNoise(seed) {
    mWorkers.emplace_back([this] { WorkerLoop(); });
    mWorkers.emplace_back([this] { WorkerLoop(); });
}

World::~World() {
    {
        std::lock_guard<std::mutex> lk(mQueueMutex);
        mStop = true;
    }
    mCv.notify_all();
    for (std::thread& t : mWorkers) t.join();
}

void World::SetRenderDistance(int rd) {
    std::lock_guard<std::mutex> lk(mQueueMutex);
    if (rd < 2) rd = 2;
    if (rd > 16) rd = 16;
    mRd = rd;
}

int World::RenderDistance() const {
    std::lock_guard<std::mutex> lk(mQueueMutex);
    return mRd;
}

std::shared_ptr<Chunk> World::FindLocked(int cx, int cz) const {
    const auto it = mChunks.find(ChunkKey(cx, cz));
    return it == mChunks.end() ? nullptr : it->second;
}

bool World::HasChunk(int cx, int cz) const {
    std::lock_guard<std::mutex> lk(mQueueMutex);
    return mChunks.find(ChunkKey(cx, cz)) != mChunks.end();
}

size_t World::ChunkCount() const {
    std::lock_guard<std::mutex> lk(mQueueMutex);
    return mChunks.size();
}

void World::Update(float camX, float camZ) {
    const int ccx = FloorDiv16(int(floorf(camX))), ccz = FloorDiv16(int(floorf(camZ)));
    std::vector<GenJob> jobs;
    int rd;
    {
        std::lock_guard<std::mutex> lk(mQueueMutex);
        rd = mRd;
        for (int dx = -rd; dx <= rd; ++dx) {
            for (int dz = -rd; dz <= rd; ++dz) {
                const int cx = ccx + dx, cz = ccz + dz;
                const uint64_t k = ChunkKey(cx, cz);
                if (mChunks.find(k) != mChunks.end()) continue;
                auto c = std::make_shared<Chunk>();
                mChunks.emplace(k, c);
                c->genQueued = true;
                jobs.push_back({c, cx, cz, float(dx * dx + dz * dz)});
            }
        }
        for (auto it = mChunks.begin(); it != mChunks.end(); ) {
            const int cx = KeyCX(it->first), cz = KeyCZ(it->first);
            if (std::max(std::abs(cx - ccx), std::abs(cz - ccz)) > rd + 1) {
                // Detach only: the 64KB free is time-sliced via EvictStep.
                mEvictQueue.push_back(std::move(it->second));
                it = mChunks.erase(it);
            } else {
                ++it;
            }
        }
    }
    std::sort(jobs.begin(), jobs.end(),
              [](const GenJob& a, const GenJob& b) { return a.dist < b.dist; });
    if (!jobs.empty()) {
        {
            std::lock_guard<std::mutex> lk(mQueueMutex);
            for (GenJob& j : jobs) mGenQueue.push_back(std::move(j));
        }
        mCv.notify_all();
    }
}

void World::GenerateChunk(Chunk& c, int cx, int cz) const {
    constexpr int SEA = 60;
    int heights[CHUNK_X][CHUNK_Z];
    for (int x = 0; x < CHUNK_X; ++x) {
        for (int z = 0; z < CHUNK_Z; ++z) {
            const float wx = float(cx * CHUNK_X + x), wz = float(cz * CHUNK_Z + z);
            const float cont = mNoise.Fbm2(wx * 0.0035f, wz * 0.0035f, 3);
            const float hills = mNoise.Fbm2(wx * 0.018f + 100.0f, wz * 0.018f + 100.0f, 4);
            int h = int(63.0f + cont * 20.0f + hills * 9.0f);
            if (h < 4) h = 4;
            if (h > CHUNK_Y - 10) h = CHUNK_Y - 10;
            heights[x][z] = h;
            const bool sandy = h <= SEA + 1;
            for (int y = 0; y <= h; ++y) {
                uint8_t b;
                if (y == 0) {
                    b = BLOCK_BEDROCK;
                } else if (y == h) {
                    b = sandy ? BLOCK_SAND : BLOCK_GRASS;
                } else if (y >= h - 3) {
                    b = sandy ? BLOCK_SAND : BLOCK_DIRT;
                } else {
                    b = BLOCK_STONE;
                    const uint32_t r = Hash3(uint32_t(wx), uint32_t(y), uint32_t(wz), mSeed) % 1000;
                    if (y < 15 && r < 8) b = BLOCK_DIAMOND_ORE;
                    else if (y < 16 && r < 34) b = BLOCK_REDSTONE_ORE;
                    else if (y < 30 && r < 46) b = BLOCK_LAPIS_ORE;
                    else if (y < 32 && r < 60) b = BLOCK_GOLD_ORE;
                    else if (y < 64 && r < 125) b = BLOCK_IRON_ORE;
                    else if (r < 220) b = BLOCK_COAL_ORE;
                    else if (r > 940) b = BLOCK_GRANITE;
                    else if (r > 900) b = BLOCK_DIORITE;
                    else if (r > 865) b = BLOCK_ANDESITE;
                }
                // Cheese caves: 3D fBm pockets, never under oceans, never
                // breaking the surface or the bedrock floor.
                if (b != BLOCK_BEDROCK && y > 6 && y < h - 3 && !sandy) {
                    const float cave = mNoise.Fbm3(wx * 0.055f, float(y) * 0.08f, wz * 0.055f, 3);
                    if (cave > 0.58f) b = BLOCK_AIR;
                }
                c.blocks[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y] = b;
            }
            for (int y = h + 1; y <= SEA; ++y) {
                c.blocks[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y] = BLOCK_WATER;
            }
        }
    }

    // Oak trees. Trunks stay 2 blocks inside the chunk border so the radius-2
    // canopy never writes into a neighbor (no cross-chunk gen writes needed).
    for (int x = 2; x < CHUNK_X - 2; ++x) {
        for (int z = 2; z < CHUNK_Z - 2; ++z) {
            const int h = heights[x][z];
            if (h <= SEA + 1 || h + 9 >= CHUNK_Y) continue;
            if (c.blocks[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * h] != BLOCK_GRASS) continue;
            const uint32_t r = Hash3(uint32_t(cx * CHUNK_X + x), 0, uint32_t(cz * CHUNK_Z + z), mSeed ^ 0x51EDu);
            if (r % 96 != 0) continue;
            const int base = h + 1, th = 4 + int(r % 3);
            auto setIfAir = [&](int lx, int y, int lz, uint8_t blk) {
                const size_t idx = size_t(lx) + CHUNK_X * size_t(lz) + CHUNK_X * CHUNK_Z * size_t(y);
                if (c.blocks[idx] == BLOCK_AIR) c.blocks[idx] = blk;
            };
            for (int dx = -2; dx <= 2; ++dx) {
                for (int dz = -2; dz <= 2; ++dz) {
                    const bool corner = abs(dx) == 2 && abs(dz) == 2;
                    for (int dy = th - 2; dy <= th - 1; ++dy) {
                        if (corner && (Hash3(uint32_t(x + dx), uint32_t(dy), uint32_t(z + dz), r) & 1)) continue;
                        setIfAir(x + dx, base + dy, z + dz, BLOCK_OAK_LEAVES);
                    }
                    if (abs(dx) <= 1 && abs(dz) <= 1) {
                        const bool plus = abs(dx) + abs(dz) <= 1;
                        if (plus || !corner) setIfAir(x + dx, base + th, z + dz, BLOCK_OAK_LEAVES);
                        if (plus) setIfAir(x + dx, base + th + 1, z + dz, BLOCK_OAK_LEAVES);
                    }
                }
            }
            for (int i = 0; i < th; ++i) {
                c.blocks[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * (base + i)] = BLOCK_LOG;
            }
        }
    }
}

int World::SurfaceY(int x, int z) const {
    const int cx = FloorDiv16(x), cz = FloorDiv16(z);
    std::shared_ptr<Chunk> c;
    {
        std::lock_guard<std::mutex> lk(mQueueMutex);
        c = FindLocked(cx, cz);
    }
    if (!c || !c->generated.load(std::memory_order_acquire)) return -1;
    const int lx = x - cx * CHUNK_X, lz = z - cz * CHUNK_Z;
    std::shared_lock<std::shared_mutex> blk(mBlocksMutex);
    for (int y = CHUNK_Y - 1; y >= 0; --y) {
        const uint8_t b = c->blocks[lx + CHUNK_X * lz + CHUNK_X * CHUNK_Z * y];
        if (BLOCK_FLAGS[b] & BF_SOLID) return y;
    }
    return -1;
}

void World::QueueMeshIfReady(int cx, int cz) {
    const std::shared_ptr<Chunk> c = FindLocked(cx, cz);
    if (!c || !c->generated.load(std::memory_order_acquire) || !c->dirty || c->meshQueued) return;
    auto neighborReady = [&](int nx, int nz) -> bool {
        const std::shared_ptr<Chunk> n = FindLocked(nx, nz);
        return !n || n->generated.load(std::memory_order_acquire);
    };
    if (!neighborReady(cx - 1, cz) || !neighborReady(cx + 1, cz) ||
        !neighborReady(cx, cz - 1) || !neighborReady(cx, cz + 1)) return;
    MeshJob j;
    j.chunk = c;
    j.negX = FindLocked(cx - 1, cz);
    j.posX = FindLocked(cx + 1, cz);
    j.negZ = FindLocked(cx, cz - 1);
    j.posZ = FindLocked(cx, cz + 1);
    j.cx = cx;
    j.cz = cz;
    j.key = ChunkKey(cx, cz);
    c->dirty = false;
    c->meshQueued = true;
    mMeshQueue.push_back(std::move(j));
    mCv.notify_all();
}

void World::MarkNeighborDirty(int cx, int cz) {
    const std::shared_ptr<Chunk> n = FindLocked(cx, cz);
    if (!n || !n->generated.load(std::memory_order_acquire)) return;
    n->dirty = true;
    QueueMeshIfReady(cx, cz);
}

void World::WorkerLoop() {
    for (;;) {
        GenJob gj{};
        MeshJob mj{};
        bool isGen = false;
        {
            std::unique_lock<std::mutex> lk(mQueueMutex);
            mCv.wait(lk, [this] { return mStop || !mGenQueue.empty() || !mMeshQueue.empty(); });
            if (mStop) return;
            if (!mGenQueue.empty()) {
                gj = std::move(mGenQueue.front());
                mGenQueue.pop_front();
                isGen = true;
            } else {
                mj = std::move(mMeshQueue.front());
                mMeshQueue.pop_front();
            }
        }
        if (isGen) {
            GenerateChunk(*gj.chunk, gj.cx, gj.cz);
            gj.chunk->generated.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(mQueueMutex);
                QueueMeshIfReady(gj.cx, gj.cz);
                QueueMeshIfReady(gj.cx - 1, gj.cz);
                QueueMeshIfReady(gj.cx + 1, gj.cz);
                QueueMeshIfReady(gj.cx, gj.cz - 1);
                QueueMeshIfReady(gj.cx, gj.cz + 1);
            }
        } else {
            MeshData md;
            {
                std::shared_lock<std::shared_mutex> blk(mBlocksMutex);
                BuildChunkMeshCPU(mj.chunk->blocks,
                                  mj.negX ? mj.negX->blocks : nullptr,
                                  mj.posX ? mj.posX->blocks : nullptr,
                                  mj.negZ ? mj.negZ->blocks : nullptr,
                                  mj.posZ ? mj.posZ->blocks : nullptr,
                                  mTiles, md);
            }
            // Vertices are chunk-local; bake the chunk's world offset so each
            // chunk lands at its own position instead of stacking at origin.
            const float ox = float(mj.cx * CHUNK_X), oz = float(mj.cz * CHUNK_Z);
            for (MeshVertex& v : md.vertices) { v.x += ox; v.z += oz; }
            {
                std::lock_guard<std::mutex> lk(mQueueMutex);
                mj.chunk->meshQueued = false;
                if (mj.chunk->dirty) QueueMeshIfReady(mj.cx, mj.cz);
            }
            // Rigid ring backpressure: if the render thread hasn't drained
            // enough slots, hold the mesh and retry rather than allocating
            // an unbounded queue. Failed TryPush leaves the upload intact.
            Upload up{mj.key, std::move(md)};
            while (!mUploads.TryPush(std::move(up))) {
                {
                    std::lock_guard<std::mutex> lk(mQueueMutex);
                    if (mStop) return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
}

uint8_t World::GetBlock(int x, int y, int z) const {
    if (y < 0 || y >= CHUNK_Y) return 0;
    const int cx = FloorDiv16(x), cz = FloorDiv16(z);
    std::shared_ptr<Chunk> c;
    {
        std::lock_guard<std::mutex> lk(mQueueMutex);
        c = FindLocked(cx, cz);
    }
    if (!c || !c->generated.load(std::memory_order_acquire)) return 0;
    std::shared_lock<std::shared_mutex> blk(mBlocksMutex);
    return c->blocks[(x - cx * CHUNK_X) + CHUNK_X * (z - cz * CHUNK_Z) + CHUNK_X * CHUNK_Z * y];
}

void World::SetBlock(int x, int y, int z, uint8_t block) {
    if (y < 0 || y >= CHUNK_Y) return;
    const int cx = FloorDiv16(x), cz = FloorDiv16(z);
    const int lx = x - cx * CHUNK_X, lz = z - cz * CHUNK_Z;
    std::shared_ptr<Chunk> c;
    {
        std::lock_guard<std::mutex> lk(mQueueMutex);
        c = FindLocked(cx, cz);
        if (!c || !c->generated.load(std::memory_order_acquire)) return;
    }
    {
        std::unique_lock<std::shared_mutex> blk(mBlocksMutex);
        c->blocks[lx + CHUNK_X * lz + CHUNK_X * CHUNK_Z * y] = block;
    }
    {
        std::lock_guard<std::mutex> lk(mQueueMutex);
        c->dirty = true;
        QueueMeshIfReady(cx, cz);
        if (lx == 0) MarkNeighborDirty(cx - 1, cz);
        if (lx == CHUNK_X - 1) MarkNeighborDirty(cx + 1, cz);
        if (lz == 0) MarkNeighborDirty(cx, cz - 1);
        if (lz == CHUNK_Z - 1) MarkNeighborDirty(cx, cz + 1);
    }
    mCv.notify_all();
}

bool World::PopUpload(uint64_t& key, MeshData& out) {
    Upload up;
    if (!mUploads.TryPop(up)) return false;
    key = up.key;
    out = std::move(up.mesh);
    return true;
}

void World::EvictStep(int maxCount) {
    for (int i = 0; i < maxCount; ++i) {
        std::shared_ptr<Chunk> c;
        {
            std::lock_guard<std::mutex> lk(mQueueMutex);
            if (mEvictQueue.empty()) return;
            c = std::move(mEvictQueue.front());
            mEvictQueue.pop_front();
        }
        c.reset(); // 64KB free outside the lock; in-flight jobs keep their ref
    }
}

} // namespace potato