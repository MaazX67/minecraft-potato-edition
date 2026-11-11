#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace potato {

// Verified local launcher layout (Migurinth): the index maps logical asset
// names to SHA1 hashes; objects/<first-2-hex>/<hash> holds the raw files.
// Resolved from %APPDATA% at runtime so any Windows username works.
std::string DefaultAssetIndexPath();

constexpr int ATLAS_SIZE = 1024;
constexpr int ATLAS_TILE = 16;
constexpr int ATLAS_COLS = ATLAS_SIZE / ATLAS_TILE;

// Contiguous 1024x1024 RGBA atlas, 16x16 tiles, 4096 slots.
// CPU-side only: the renderer uploads Pixels() as one immutable texture.
class Atlas {
public:
    static constexpr int MAX_TILES = ATLAS_COLS * ATLAS_COLS;
    Atlas() : mPixels(ATLAS_SIZE * ATLAS_SIZE * 4, 0) {}

    int AddTile(const std::string& name, const uint8_t* rgba16x16);
    int AddCheckerTile(const std::string& name);
    int FindTile(const std::string& name) const;
    bool GetTileOrigin(int tile, float& u, float& v) const;
    int TileCount() const { return mNext; }
    const uint8_t* Pixels() const { return mPixels.data(); }

private:
    std::vector<uint8_t> mPixels;
    std::unordered_map<std::string, int> mNames;
    int mNext = 0;
};

class AssetStore {
public:
    bool LoadIndex(const std::string& indexPath);
    bool ReadFile(const std::string& name, std::vector<uint8_t>& out) const;
    std::vector<std::string> ListPrefix(const std::string& prefix) const;
    size_t EntryCount() const { return mObjects.size(); }

private:
    std::unordered_map<std::string, std::string> mObjects; // logical name -> sha1
    std::string mObjectsDir;
};

enum BlockId : uint8_t {
    BLOCK_AIR = 0, BLOCK_STONE, BLOCK_DIRT, BLOCK_GRASS, BLOCK_COBBLE,
    BLOCK_PLANKS, BLOCK_LOG, BLOCK_SAND, BLOCK_GRAVEL, BLOCK_BRICK, BLOCK_COUNT
};

struct BlockDef {
    const char* name;
    const char* top;
    const char* side;
    const char* bottom;
};
extern const BlockDef BLOCK_DEFS[BLOCK_COUNT];

// Decodes a PNG to exactly 16x16 RGBA. Animated strips (h > 16) use frame 0;
// other sizes are nearest-neighbor resampled. out must hold 16*16*4 bytes.
bool DecodePNG16(const std::vector<uint8_t>& bytes, uint8_t* out16);

// Decodes a PNG at its native size (GUI sprites, font). out is w*h*4 RGBA.
bool DecodePNGAny(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& out, int& w, int& h);

// Loads every minecraft/textures/block/*.png into the atlas.
// Tile 0 is always the missing-texture checker. Returns block tiles loaded.
int BuildBlockAtlas(AssetStore& store, Atlas& atlas);

} // namespace potato