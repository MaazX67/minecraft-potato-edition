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

struct Chunk {
    uint8_t blocks[CHUNK_VOL];
    std::atomic<bool> generated{false};
    bool genQueued = false;
    bool dirty = true;
    bool meshQueued = false;
    Chunk() { memset(blocks, 0, sizeof(blocks)); }
};

class PerlinNoise {
public:
    explicit PerlinNoise(uint32_t seed);
    float Noise2(float x, float y) const;
    float Fbm2(float x, float y, int octaves) const; // normalized to ~[-1, 1]
    float Noise3(float x, float y, float z) const;
    float Fbm3(float x, float y, float z, int octaves) const;
private:
    uint8_t mP[512];
};

// Full 3-axis greedy mesher. Free function so the headless selftest can run it
// without a World. Neighbor pointers may be null (treated as air). y<0 and
// y>=256 are air. Merge key = (tile, light, face sign).
void BuildChunkMeshCPU(const uint8_t* blocks, const uint8_t* negX, const uint8_t* posX,
                       const uint8_t* negZ, const uint8_t* posZ, const BlockTiles& tiles,
                       MeshData& out);

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

// Async voxel world. Two worker threads: generation jobs first, then meshing.
// The render thread drains finished meshes via PopUpload and owns all GPU
// resources. Map and queues are guarded by mQueueMutex; block reads/writes by
// mBlocksMutex. Lock order is always mQueueMutex -> mBlocksMutex, never nested
// in reverse. Chunks are shared_ptr so in-flight jobs survive map eviction.
class World {
public:
    World(uint32_t seed, int renderDistance);
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

    const uint32_t mSeed;
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
};

} // namespace potato