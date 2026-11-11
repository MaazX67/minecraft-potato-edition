#include "assets.h"
#include <windows.h>
#include <algorithm>
#include <cstdlib>
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

std::string DefaultAssetIndexPath() {
    const char* appdata = getenv("APPDATA");
    if (appdata && appdata[0]) {
        return std::string(appdata) + "/Migurinth/meta/assets/indexes/34.json";
    }
    return "C:/Users/Admin/AppData/Roaming/Migurinth/meta/assets/indexes/34.json";
}

std::string DefaultClientJarPath() {
    const char* appdata = getenv("APPDATA");
    const std::string base = std::string(appdata && appdata[0] ? appdata : "C:/Users/Admin/AppData/Roaming")
                           + "/Migurinth/meta/versions";
    std::string best, bestDir;
    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA((base + "/*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return best;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::string dir = fd.cFileName;
        if (dir == "." || dir == "..") continue;
        const std::string jar = base + "/" + dir + "/" + dir + ".jar";
        if (GetFileAttributesA(jar.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (dir > bestDir) { bestDir = dir; best = jar; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return best;
}

// ---------------------------------------------------------------------------
// ZipArchive (read-only jar)
// ---------------------------------------------------------------------------

bool ZipArchive::Open(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const size_t size = size_t(f.tellg());
    if (size < 22) return false;
    const size_t scan = std::min(size, size_t(22 + 65535));
    std::vector<uint8_t> tail(scan);
    f.seekg(std::streamoff(size - scan));
    f.read(reinterpret_cast<char*>(tail.data()), std::streamsize(scan));
    if (!f) return false;
    size_t eocd = SIZE_MAX;
    for (size_t i = scan - 22; ; --i) {
        if (tail[i] == 0x50 && tail[i + 1] == 0x4B && tail[i + 2] == 0x05 && tail[i + 3] == 0x06) {
            eocd = i;
            break;
        }
        if (i == 0) break;
    }
    if (eocd == SIZE_MAX) return false;
    auto rd32 = [&](size_t p) {
        return uint32_t(tail[p]) | (uint32_t(tail[p + 1]) << 8) |
               (uint32_t(tail[p + 2]) << 16) | (uint32_t(tail[p + 3]) << 24);
    };
    const uint32_t cdSize = rd32(eocd + 12), cdOff = rd32(eocd + 16);
    if (cdSize == 0 || size_t(cdOff) + cdSize > size) return false;
    std::vector<uint8_t> cd(cdSize);
    f.seekg(std::streamoff(cdOff));
    f.read(reinterpret_cast<char*>(cd.data()), std::streamsize(cdSize));
    if (!f) return false;
    size_t p = 0;
    while (p + 46 <= cd.size()) {
        if (!(cd[p] == 0x50 && cd[p + 1] == 0x4B && cd[p + 2] == 0x01 && cd[p + 3] == 0x02)) break;
        auto c16 = [&](size_t q) { return uint16_t(cd[q] | (uint16_t(cd[q + 1]) << 8)); };
        auto c32 = [&](size_t q) {
            return uint32_t(cd[q]) | (uint32_t(cd[q + 1]) << 8) |
                   (uint32_t(cd[q + 2]) << 16) | (uint32_t(cd[q + 3]) << 24);
        };
        Entry e;
        e.method = c16(p + 10);
        e.compSize = c32(p + 20);
        e.uncompSize = c32(p + 24);
        e.localOffset = c32(p + 42);
        const uint16_t nameLen = c16(p + 28), extraLen = c16(p + 30), commentLen = c16(p + 32);
        if (p + 46 + nameLen > cd.size()) break;
        std::string name(reinterpret_cast<const char*>(&cd[p + 46]), nameLen);
        if (!name.empty() && name.back() != '/') mEntries.emplace(std::move(name), e);
        p += size_t(46) + nameLen + extraLen + commentLen;
    }
    if (mEntries.empty()) return false;
    mPath = path;
    return true;
}

bool ZipArchive::Read(const std::string& name, std::vector<uint8_t>& out) const {
    const auto it = mEntries.find(name);
    if (it == mEntries.end()) return false;
    const Entry& e = it->second;
    std::ifstream f(mPath, std::ios::binary);
    if (!f.is_open()) return false;
    uint8_t lh[30];
    f.seekg(std::streamoff(e.localOffset));
    f.read(reinterpret_cast<char*>(lh), 30);
    if (!f || !(lh[0] == 0x50 && lh[1] == 0x4B && lh[2] == 0x03 && lh[3] == 0x04)) return false;
    const uint16_t nameLen = uint16_t(lh[26] | (uint16_t(lh[27]) << 8));
    const uint16_t extraLen = uint16_t(lh[28] | (uint16_t(lh[29]) << 8));
    f.seekg(std::streamoff(uint64_t(e.localOffset) + 30 + nameLen + extraLen));
    if (e.method == 0) {
        out.resize(e.uncompSize);
        f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
        return bool(f) && !out.empty();
    }
    if (e.method != 8 || e.compSize == 0) return false;
    std::vector<uint8_t> comp(e.compSize);
    f.read(reinterpret_cast<char*>(comp.data()), std::streamsize(comp.size()));
    if (!f) return false;
    int outLen = 0;
    char* raw = stbi_zlib_decode_malloc_guesssize_headerflag(
        reinterpret_cast<const char*>(comp.data()), int(comp.size()),
        int(e.uncompSize > 0 ? e.uncompSize : 16384), &outLen, 0); // 0 = raw deflate
    if (!raw || outLen <= 0) {
        if (raw) stbi_image_free(raw);
        return false;
    }
    out.assign(reinterpret_cast<uint8_t*>(raw), reinterpret_cast<uint8_t*>(raw) + outLen);
    stbi_image_free(raw);
    return true;
}

std::vector<std::string> ZipArchive::List() const {
    std::vector<std::string> names;
    names.reserve(mEntries.size());
    for (const auto& kv : mEntries) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    return names;
}

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
    if (it != mObjects.end() && it->second.size() >= 3) {
        const std::string& hash = it->second;
        const std::string path = mObjectsDir + "/" + hash.substr(0, 2) + "/" + hash;
        std::ifstream f(path, std::ios::binary);
        if (f.is_open()) {
            out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            if (!out.empty()) return true;
        }
    }
    return mJar.Read("assets/" + name, out);
}

std::vector<std::string> AssetStore::ListPrefix(const std::string& prefix) const {
    std::vector<std::string> names;
    for (const auto& kv : mObjects) {
        if (kv.first.compare(0, prefix.size(), prefix) == 0) names.push_back(kv.first);
    }
    for (const std::string& jarName : mJar.List()) {
        if (jarName.compare(0, 7, "assets/") != 0) continue;
        const std::string name = jarName.substr(7);
        if (name.compare(0, prefix.size(), prefix) == 0 && mObjects.find(name) == mObjects.end()) {
            names.push_back(std::move(name));
        }
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