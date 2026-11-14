#pragma once
#include "engine.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace potato {

constexpr int CHUNK_X = 16, CHUNK_Y = 256, CHUNK_Z = 16;
constexpr int CHUNK_VOL = CHUNK_X * CHUNK_Y * CHUNK_Z; // 65536 bytes flat

struct MeshVertex {
    float x, y, z;    // world position
    float tu, tv;     // atlas tile origin (UV)
    float u, v;       // local UV in block units (greedy tiling via frac in PS)
    float light;      // raw 0..15
    float cr, cg, cb; // vertex tint (1,1,1 = untinted; GUI text colors)
};

struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    // indices [0, opaqueIndexCount) are opaque/cutout; the rest are liquid
    // (drawn last with alpha blend, depth write off).
    uint32_t opaqueIndexCount = 0;
};

// Resolved atlas tile ids per block id and face class.
struct BlockTiles {
    int top[256];
    int side[256];
    int bottom[256];
    BlockTiles() {
        memset(top, 0, sizeof(top));
        memset(side, 0, sizeof(side));
        memset(bottom, 0, sizeof(bottom));
    }
};

// Java-style PalettedContainer for one 16x16x16 chunk section. Uniform
// sections keep a single id (no allocation); mixed sections store a palette
// with 2/4/8-bit packed indices, so a section never exceeds 4 KB + palette.
class PalettedSection {
public:
    uint8_t Get(int index) const {
        if (mBits == 0) return mUniform;
        const int per = 32 / mBits;
        const uint32_t word = mWords[size_t(index) / per];
        const uint32_t shift = uint32_t(index % per) * mBits;
        return mPalette[(word >> shift) & ((1u << mBits) - 1u)];
    }
    void Set(int index, uint8_t value) {
        if (mBits == 0) {
            if (value == mUniform) return;
            mPalette.push_back(mUniform);
            mPalette.push_back(value);
            mBits = 2;
            mWords.assign(4096 * 2 / 32, 0);
            Write(index, 1);
            return;
        }
        int slot = Find(value);
        if (slot < 0) {
            if (mPalette.size() >= (size_t(1) << mBits)) {
                if (mBits >= 8) return;
                Repack(mBits == 2 ? 4 : 8);
            }
            slot = int(mPalette.size());
            mPalette.push_back(value);
        }
        Write(index, uint32_t(slot));
    }
    bool IsUniform() const { return mBits == 0; }
    size_t StorageBytes() const {
        return mBits == 0 ? sizeof(*this) : sizeof(*this) + mWords.size() * 4 + mPalette.size();
    }
private:
    int Find(uint8_t value) const {
        for (size_t i = 0; i < mPalette.size(); ++i)
            if (mPalette[i] == value) return int(i);
        return -1;
    }
    void Write(int index, uint32_t slot) {
        const int per = 32 / mBits;
        uint32_t& word = mWords[size_t(index) / per];
        const uint32_t shift = uint32_t(index % per) * mBits;
        const uint32_t mask = (1u << mBits) - 1u;
        word = (word & ~(mask << shift)) | ((slot & mask) << shift);
    }
    void Write(int index, uint32_t slot, uint8_t value) {
        Write(index, slot);
        (void)value;
    }
    void Repack(uint8_t newBits) {
        std::vector<uint32_t> packed(4096 * newBits / 32, 0);
        for (int i = 0; i < 4096; ++i) {
            const int per = 32 / mBits;
            const uint32_t word = mWords[size_t(i) / per];
            const uint32_t shift = uint32_t(i % per) * mBits;
            const uint32_t slot = (word >> shift) & ((1u << mBits) - 1u);
            const int nper = 32 / newBits;
            const uint32_t nshift = uint32_t(i % nper) * newBits;
            packed[size_t(i) / nper] |= slot << nshift;
        }
        mWords = std::move(packed);
        mBits = newBits;
    }
    uint8_t mBits = 0;            // 0 = uniform, else 2/4/8 bits per index
    uint8_t mUniform = 0;         // 0 == BLOCK_AIR (world.h has no assets.h include)
    std::vector<uint8_t> mPalette;
    std::vector<uint32_t> mWords;
};

struct Chunk {
    static constexpr int SECTION_COUNT = CHUNK_Y / 16;
    PalettedSection sections[SECTION_COUNT];
    std::atomic<bool> generated{false};
    bool genQueued = false;
    bool dirty = true;
    bool meshQueued = false;

    uint8_t GetIndex(int index) const { return sections[index >> 12].Get(index & 4095); }
    void SetIndex(int index, uint8_t value) { sections[index >> 12].Set(index & 4095, value); }
    void SetRaw(const uint8_t* raw) {
        for (int s = 0; s < SECTION_COUNT; ++s)
            for (int i = 0; i < 4096; ++i) sections[s].Set(i, raw[s * 4096 + i]);
    }
    void GetRaw(uint8_t* out) const {
        for (int s = 0; s < SECTION_COUNT; ++s)
            for (int i = 0; i < 4096; ++i) out[s * 4096 + i] = sections[s].Get(i);
    }
    size_t StorageBytes() const {
        size_t total = sizeof(*this);
        for (const PalettedSection& s : sections) total += s.StorageBytes() - sizeof(PalettedSection);
        return total;
    }
};

class PerlinNoise {
public:
    explicit PerlinNoise(uint64_t seed);
    float Noise2(float x, float y) const;
    float Fbm2(float x, float y, int octaves) const; // normalized to ~[-1, 1]
    float Noise3(float x, float y, float z) const;
    float Fbm3(float x, float y, float z, int octaves) const;
private:
    uint8_t mP[512];
};

// Java-style 64-bit world seeds. SplitMix64 expands one seed into an endless
// stream of independent, well-distributed values.
inline uint64_t SplitMix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
inline uint64_t MixSeed64(uint64_t seed) {
    return SplitMix64(seed);
}
inline uint32_t SeedHash32(uint64_t seed) {
    return uint32_t(MixSeed64(seed) >> 32);
}


enum class WorldGenerator { Legacy, Climate, Flat, Nether };
const char* GeneratorName(WorldGenerator generator);
bool ParseGenerator(const std::string& name, WorldGenerator& generator);

enum class Biome { Plains, Forest, BirchForest, Taiga, Desert, Snowy, Mountains, River, Ocean, CherryGrove };
const char* BiomeName(Biome biome);

struct TerrainColumn {
    int height;
    Biome biome;
    float temperature;
    float humidity;
};

TerrainColumn SampleTerrain(const PerlinNoise& noise, int x, int z);
void GenerateClimateChunk(uint8_t* blocks, const PerlinNoise& noise, uint64_t seed, int cx, int cz);
void GenerateNetherChunk(uint8_t* blocks, const PerlinNoise& noise, uint64_t seed, int cx, int cz);

// Seeded structure placement. Java 26.3 places one candidate per region cell;
// we use a 512-block grid (256 in the Nether) keyed with SplitMix64 so a world
// seed alone determines every village, temple, outpost, mineshaft and fortress.
enum class StructureType {
    Village, DesertPyramid, JungleTemple, RuinedPortal, PillagerOutpost,
    Mineshaft, Stronghold, NetherFortress, Bastion, Count
};
const char* StructureId(StructureType type);          // "village", "desert_pyramid", ...
const char* StructureName(StructureType type);        // "Village", "Desert Pyramid", ...
bool IsNetherStructure(StructureType type);
bool ParseStructure(const std::string& name, StructureType& type); // resource ids accepted
// Nearest candidate of the requested type (block coords). Distance is the
// horizontal block distance from (fromX, fromZ).
bool FindStructure(uint64_t seed, int fromX, int fromZ, StructureType type,
                   int& outX, int& outZ, int& distance);
// Stamps the parts of every intersecting structure that land inside this
// chunk. nether selects the Nether grid (fortresses only).
void ApplyStructures(uint8_t* blocks, const PerlinNoise& noise, uint64_t seed,
                     int cx, int cz, bool nether);

// Full 3-axis greedy mesher. Free function so the headless selftest can run it
// without a World. Neighbor pointers may be null (treated as air). y<0 and
// y>=256 are air. Merge key = (tile, light, face sign).
void BuildChunkMeshCPU(const uint8_t* blocks, const uint8_t* negX, const uint8_t* posX,
                       const uint8_t* negZ, const uint8_t* posZ, const BlockTiles& tiles,
                       MeshData& out);

// Extrudes a 16x16 RGBA item sprite into a 1-pixel-thick voxel mesh (front/back
// faces plus side walls on transparent borders), centered at the origin in local
// space. u/v carry sprite UVs. Mirrors Java's item model generator.
void BuildExtrudedItemMesh(const uint8_t* rgba, std::vector<MeshVertex>& out);

// Amanatides & Woo voxel traversal. Returns the hit voxel and the face normal.
bool RaycastDDA(const std::function<uint8_t(int, int, int)>& getBlock,
                float ox, float oy, float oz, float dx, float dy, float dz, float maxDist,
                int& hx, int& hy, int& hz, int& nx, int& ny, int& nz);

inline uint64_t ChunkKey(int cx, int cz) {
    return (uint64_t(uint32_t(cx)) << 32) | uint32_t(cz);
}
inline int KeyCX(uint64_t k) { return int(int32_t(k >> 32)); }
inline int KeyCZ(uint64_t k) { return int(int32_t(k & 0xFFFFFFFFu)); }
inline int FloorDiv16(int v) { return v >= 0 ? v / 16 : (v - 15) / 16; }

// Save only player edits; regenerate untouched terrain from the seed.
using ChunkEdits = std::unordered_map<uint16_t, uint8_t>;
using EditMap = std::unordered_map<uint64_t, ChunkEdits>;
std::vector<uint8_t> EncodeEdits(const EditMap& edits);
bool DecodeEdits(const uint8_t* data, size_t size, EditMap& out);

// Async voxel world. Two worker threads: generation jobs first, then meshing.
// The render thread drains finished meshes via PopUpload and owns all GPU
// resources. Map and queues are guarded by mQueueMutex; block reads/writes by
// mBlocksMutex. Lock order is always mQueueMutex -> mBlocksMutex, never nested
// in reverse. Chunks are shared_ptr so in-flight jobs survive map eviction.
class World {
public:
    World(uint64_t seed, int renderDistance, WorldGenerator generator = WorldGenerator::Legacy);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    void SetRenderDistance(int rd);
    int RenderDistance() const;
    void SetBlockTiles(const BlockTiles& tiles) { mTiles = tiles; }
    void Update(float camX, float camZ); // load/unload chunks around the camera
    uint8_t GetBlock(int x, int y, int z) const;
    void SetBlock(int x, int y, int z, uint8_t block); // marks dirty, remeshes
    // Highest solid block in the column, or -1 if the chunk isn't generated.
    int SurfaceY(int x, int z) const;
    bool HasChunk(int cx, int cz) const;
    size_t ChunkCount() const;
    bool PopUpload(uint64_t& key, MeshData& out);
    // Time-sliced cleanup: destroys at most maxCount evicted subchunks per
    // call. Update() only detaches them from the map; the 64KB frees happen
    // here, under the render thread's hard per-frame ceiling.
    void EvictStep(int maxCount);
    uint64_t Seed() const { return mSeed; }
    WorldGenerator Generator() const { return mGenerator; }
    EditMap SnapshotEdits() const;
    // Call before the first Update(); edits apply as chunks generate.
    void SetEdits(EditMap edits);

private:
    struct GenJob {
        std::shared_ptr<Chunk> chunk;
        int cx, cz;
        float dist;
    };
    struct MeshJob {
        std::shared_ptr<Chunk> chunk, negX, posX, negZ, posZ;
        int cx, cz;
        uint64_t key;
    };
    struct Upload {
        uint64_t key;
        MeshData mesh;
    };

    void WorkerLoop();
    void GenerateChunk(Chunk& c, int cx, int cz) const;
    void QueueMeshIfReady(int cx, int cz); // mQueueMutex must be held
    void MarkNeighborDirty(int cx, int cz); // mQueueMutex must be held
    std::shared_ptr<Chunk> FindLocked(int cx, int cz) const;

    const uint64_t mSeed;
    const WorldGenerator mGenerator;
    int mRd;
    PerlinNoise mNoise;
    BlockTiles mTiles;
    std::unordered_map<uint64_t, std::shared_ptr<Chunk>> mChunks;
    std::deque<GenJob> mGenQueue;
    std::deque<MeshJob> mMeshQueue;
    std::deque<std::shared_ptr<Chunk>> mEvictQueue;
    RingQueue<Upload, 128> mUploads; // rigid ring; workers block when full
    mutable std::mutex mQueueMutex;
    mutable std::shared_mutex mBlocksMutex;
    std::condition_variable mCv;
    bool mStop = false;
    std::vector<std::thread> mWorkers;
    EditMap mEdits;
    mutable std::mutex mEditsMutex; // leaf lock; never held with the others
};

} // namespace potato