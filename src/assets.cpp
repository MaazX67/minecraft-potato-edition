#include "assets.h"
#include <algorithm>
#include <cstring>
#include <fstream>

#pragma warning(push)
#pragma warning(disable: 4244 4245 4456 4457 4701 4702 4706 4996)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#pragma warning(pop)

#include "json.hpp"

namespace potato {

// ---------------------------------------------------------------------------
// Atlas
// ---------------------------------------------------------------------------

int Atlas::AddTile(const std::string& name, const uint8_t* rgba) {
    auto it = mNames.find(name);
    if (it != mNames.end()) return it->second;
    if (mNext >= MAX_TILES) return -1;
    const int tile = mNext++;
    const int col = tile % ATLAS_COLS, row = tile / ATLAS_COLS;
    for (int y = 0; y < ATLAS_TILE; ++y) {
        memcpy(&mPixels[((row * ATLAS_TILE + y) * ATLAS_SIZE + col * ATLAS_TILE) * 4],
               rgba + y * ATLAS_TILE * 4, ATLAS_TILE * 4);
    }
    mNames.emplace(name, tile);
    return tile;
}

int Atlas::AddCheckerTile(const std::string& name) {
    uint8_t px[ATLAS_TILE * ATLAS_TILE * 4];
    for (int y = 0; y < ATLAS_TILE; ++y) {
        for (int x = 0; x < ATLAS_TILE; ++x) {
            const bool magenta = ((x / 8) + (y / 8)) % 2 == 0;
            uint8_t* p = px + (y * ATLAS_TILE + x) * 4;
            p[0] = magenta ? 255 : 0;
            p[1] = 0;
            p[2] = magenta ? 255 : 0;
            p[3] = 255;
        }
    }
    return AddTile(name, px);
}

int Atlas::FindTile(const std::string& name) const {
    const auto it = mNames.find(name);
    return it == mNames.end() ? -1 : it->second;
}

bool Atlas::GetTileOrigin(int tile, float& u, float& v) const {
    if (tile < 0 || tile >= mNext) return false;
    u = float((tile % ATLAS_COLS) * ATLAS_TILE) / float(ATLAS_SIZE);
    v = float((tile / ATLAS_COLS) * ATLAS_TILE) / float(ATLAS_SIZE);
    return true;
}

// ---------------------------------------------------------------------------
// AssetStore
// ---------------------------------------------------------------------------

bool AssetStore::LoadIndex(const std::string& indexPath) {
    try {
        std::ifstream f(indexPath);
        if (!f.is_open()) return false;
        nlohmann::json j;
        f >> j;
        const auto& objects = j.at("objects");
        mObjects.clear();
        for (auto it = objects.begin(); it != objects.end(); ++it) {
            mObjects[it.key()] = it.value().at("hash").get<std::string>();
        }
        const size_t s1 = indexPath.find_last_of("/\\");
        const std::string indexesDir = indexPath.substr(0, s1);
        const size_t s2 = indexesDir.find_last_of("/\\");
        mObjectsDir = indexesDir.substr(0, s2) + "/objects";
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool AssetStore::ReadFile(const std::string& name, std::vector<uint8_t>& out) const {
    const auto it = mObjects.find(name);
    if (it == mObjects.end() || it->second.size() < 3) return false;
    const std::string& hash = it->second;
    const std::string path = mObjectsDir + "/" + hash.substr(0, 2) + "/" + hash;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

std::vector<std::string> AssetStore::ListPrefix(const std::string& prefix) const {
    std::vector<std::string> names;
    for (const auto& kv : mObjects) {
        if (kv.first.compare(0, prefix.size(), prefix) == 0) names.push_back(kv.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

// ---------------------------------------------------------------------------
// PNG decode
// ---------------------------------------------------------------------------

bool DecodePNG16(const std::vector<uint8_t>& bytes, uint8_t* out16) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* img = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &comp, 4);
    if (!img) return false;
    if (w == ATLAS_TILE && h == ATLAS_TILE) {
        memcpy(out16, img, ATLAS_TILE * ATLAS_TILE * 4);
    } else if (w == ATLAS_TILE && h > ATLAS_TILE) {
        // Animated strip: frame 0 only.
        memcpy(out16, img, ATLAS_TILE * ATLAS_TILE * 4);
    } else {
        for (int y = 0; y < ATLAS_TILE; ++y) {
            for (int x = 0; x < ATLAS_TILE; ++x) {
                const int sx = x * w / ATLAS_TILE, sy = y * h / ATLAS_TILE;
                memcpy(out16 + (y * ATLAS_TILE + x) * 4, img + (sy * w + sx) * 4, 4);
            }
        }
    }
    stbi_image_free(img);
    return true;
}

bool DecodePNGAny(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& out, int& w, int& h) {
    int comp = 0;
    stbi_uc* img = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &comp, 4);
    if (!img) return false;
    out.assign(img, img + size_t(w) * size_t(h) * 4);
    stbi_image_free(img);
    return true;
}

// ---------------------------------------------------------------------------
// Block atlas
// ---------------------------------------------------------------------------

const BlockDef BLOCK_DEFS[BLOCK_COUNT] = {
    {"air",         "",                "",                ""},
    {"stone",       "stone",           "stone",           "stone"},
    {"dirt",        "dirt",            "dirt",            "dirt"},
    {"grass",       "grass_block_top", "grass_block_side","dirt"},
    {"cobblestone", "cobblestone",     "cobblestone",     "cobblestone"},
    {"oak_planks",  "oak_planks",      "oak_planks",      "oak_planks"},
    {"oak_log",     "oak_log_top",     "oak_log",         "oak_log_top"},
    {"sand",        "sand",            "sand",            "sand"},
    {"gravel",      "gravel",          "gravel",          "gravel"},
    {"bricks",      "bricks",          "bricks",          "bricks"},
};

// Vanilla grass textures are grayscale and biome-tinted at runtime by Java.
// We bake the plains tint (0x91BD59) at load so no shader tint path is needed.
static void ApplyGrassTint(const std::string& name, uint8_t* px) {
    const bool top = name.find("grass_block_top") != std::string::npos;
    const bool side = name.find("grass_block_side") != std::string::npos;
    if (!top && !side) return;
    for (int y = 0; y < ATLAS_TILE; ++y) {
        if (side && y >= 4) break; // side strip only; the rest is plain dirt
        for (int x = 0; x < ATLAS_TILE; ++x) {
            uint8_t* p = px + (y * ATLAS_TILE + x) * 4;
            p[0] = uint8_t(p[0] * 145 / 255);
            p[1] = uint8_t(p[1] * 189 / 255);
            p[2] = uint8_t(p[2] * 89 / 255);
        }
    }
}

int BuildBlockAtlas(AssetStore& store, Atlas& atlas) {
    atlas.AddCheckerTile("__missing"); // tile 0 is always the fallback
    const std::vector<std::string> names = store.ListPrefix("minecraft/textures/block/");
    int loaded = 0;
    uint8_t px[ATLAS_TILE * ATLAS_TILE * 4];
    for (const std::string& name : names) {
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".png") != 0) continue;
        std::vector<uint8_t> bytes;
        if (!store.ReadFile(name, bytes)) continue;
        if (!DecodePNG16(bytes, px)) continue;
        ApplyGrassTint(name, px);
        if (atlas.AddTile(name, px) >= 0) ++loaded;
    }
    return loaded;
}

} // namespace potato