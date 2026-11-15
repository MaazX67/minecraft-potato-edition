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

bool Atlas::GetTilePixels(int tile, uint8_t* out16) const {
    if (tile < 0 || tile >= mNext || !out16) return false;
    const int x0 = (tile % ATLAS_COLS) * ATLAS_TILE;
    const int y0 = (tile / ATLAS_COLS) * ATLAS_TILE;
    const uint8_t* src = mPixels.data() + (size_t(y0) * ATLAS_SIZE + x0) * 4;
    for (int y = 0; y < ATLAS_TILE; ++y) {
        memcpy(out16 + size_t(y) * ATLAS_TILE * 4, src + size_t(y) * ATLAS_SIZE * 4,
               ATLAS_TILE * 4);
    }
    return true;
}

// Normalised rect for one atlas tile. Half-texel insets keep a point sampler
// inside the tile on every GPU, so neighbouring icons never bleed.
bool AtlasTileRect(int tile, float& u0, float& v0, float& u1, float& v1) {
    if (tile < 0 || tile >= Atlas::MAX_TILES) return false;
    constexpr float half = 0.5f / float(ATLAS_SIZE);
    const float px = float((tile % ATLAS_COLS) * ATLAS_TILE) / float(ATLAS_SIZE);
    const float py = float((tile / ATLAS_COLS) * ATLAS_TILE) / float(ATLAS_SIZE);
    u0 = px + half;
    v0 = py + half;
    u1 = px + float(ATLAS_TILE) / float(ATLAS_SIZE) - half;
    v1 = py + float(ATLAS_TILE) / float(ATLAS_SIZE) - half;
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

std::vector<std::string> AssetStore::ListJarPrefix(const std::string& prefix) const {
    std::vector<std::string> names;
    for (const std::string& name : mJar.List()) {
        if (name.compare(0, prefix.size(), prefix) == 0) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
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

// Animated Java block textures (water_still.png is a 16x512 vertical strip,
// 32 frames) are sliced into 16x16 tiles; the world pass scrolls within the
// tile on the 20 TPS clock.
void SliceAnimatedFrame(const uint8_t* rgba, int w, int h, int frame, uint8_t* out16) {
    if (w == ATLAS_TILE && h > ATLAS_TILE) {
        const int frames = h / ATLAS_TILE;
        int f = frame % frames;
        if (f < 0) f += frames;
        memcpy(out16, rgba + size_t(f) * ATLAS_TILE * size_t(w) * 4, ATLAS_TILE * ATLAS_TILE * 4);
        return;
    }
    if (w == ATLAS_TILE && h == ATLAS_TILE) {
        memcpy(out16, rgba, ATLAS_TILE * ATLAS_TILE * 4);
        return;
    }
    for (int y = 0; y < ATLAS_TILE; ++y) {
        for (int x = 0; x < ATLAS_TILE; ++x) {
            const int sx = x * w / ATLAS_TILE, sy = y * h / ATLAS_TILE;
            memcpy(out16 + (y * ATLAS_TILE + x) * 4, rgba + (size_t(sy) * w + sx) * 4, 4);
        }
    }
}

bool DecodePNG16(const std::vector<uint8_t>& bytes, uint8_t* out16) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* img = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &comp, 4);
    if (!img) return false;
    SliceAnimatedFrame(img, w, h, 0, out16);
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
    {"air",               "",                     "",                     ""},
    {"stone",             "stone",                "stone",                "stone"},
    {"dirt",              "dirt",                 "dirt",                 "dirt"},
    {"grass",             "grass_block_top",      "grass_block_side",     "dirt"},
    {"cobblestone",       "cobblestone",          "cobblestone",          "cobblestone"},
    {"oak_planks",        "oak_planks",           "oak_planks",           "oak_planks"},
    {"oak_log",           "oak_log_top",          "oak_log",              "oak_log_top"},
    {"sand",              "sand",                 "sand",                 "sand"},
    {"gravel",            "gravel",               "gravel",               "gravel"},
    {"bricks",            "bricks",               "bricks",               "bricks"},
    {"bedrock",           "bedrock",              "bedrock",              "bedrock"},
    {"coal_ore",          "coal_ore",             "coal_ore",             "coal_ore"},
    {"iron_ore",          "iron_ore",             "iron_ore",             "iron_ore"},
    {"gold_ore",          "gold_ore",             "gold_ore",             "gold_ore"},
    {"diamond_ore",       "diamond_ore",          "diamond_ore",          "diamond_ore"},
    {"redstone_ore",      "redstone_ore",         "redstone_ore",         "redstone_ore"},
    {"lapis_ore",         "lapis_ore",            "lapis_ore",            "lapis_ore"},
    {"granite",           "granite",              "granite",              "granite"},
    {"diorite",           "diorite",              "diorite",              "diorite"},
    {"andesite",          "andesite",             "andesite",             "andesite"},
    {"polished_granite",  "polished_granite",     "polished_granite",     "polished_granite"},
    {"polished_diorite",  "polished_diorite",     "polished_diorite",     "polished_diorite"},
    {"polished_andesite", "polished_andesite",    "polished_andesite",    "polished_andesite"},
    {"spruce_planks",     "spruce_planks",        "spruce_planks",        "spruce_planks"},
    {"birch_planks",      "birch_planks",         "birch_planks",         "birch_planks"},
    {"spruce_log",        "spruce_log_top",       "spruce_log",           "spruce_log_top"},
    {"birch_log",         "birch_log_top",        "birch_log",            "birch_log_top"},
    {"oak_leaves",        "oak_leaves",           "oak_leaves",           "oak_leaves"},
    {"spruce_leaves",     "spruce_leaves",        "spruce_leaves",        "spruce_leaves"},
    {"birch_leaves",      "birch_leaves",         "birch_leaves",         "birch_leaves"},
    {"glass",             "glass",                "glass",                "glass"},
    {"sandstone",         "sandstone_top",        "sandstone",            "sandstone_bottom"},
    {"smooth_stone",      "smooth_stone",         "smooth_stone",         "smooth_stone"},
    {"stone_bricks",      "stone_bricks",         "stone_bricks",         "stone_bricks"},
    {"mossy_cobblestone", "mossy_cobblestone",    "mossy_cobblestone",    "mossy_cobblestone"},
    {"obsidian",          "obsidian",             "obsidian",             "obsidian"},
    {"tnt",               "tnt_top",              "tnt_side",             "tnt_bottom"},
    {"crafting_table",    "crafting_table_top",   "crafting_table_front",  "oak_planks"},
    {"furnace",           "furnace_top",          "furnace_side",         "furnace_top"},
    {"bookshelf",         "oak_planks",           "bookshelf",            "oak_planks"},
    {"white_wool",        "white_wool",           "white_wool",           "white_wool"},
    {"red_wool",          "red_wool",             "red_wool",             "red_wool"},
    {"blue_wool",         "blue_wool",            "blue_wool",            "blue_wool"},
    {"lime_wool",         "lime_wool",            "lime_wool",            "lime_wool"},
    {"yellow_wool",       "yellow_wool",          "yellow_wool",          "yellow_wool"},
    {"black_wool",        "black_wool",           "black_wool",           "black_wool"},
    {"water",             "water_still",          "water_still",          "water_still"},
    {"ice",               "ice",                  "ice",                  "ice"},
    {"clay",              "clay",                 "clay",                 "clay"},
    {"snow",              "snow",                 "snow",                 "snow"},
    {"deepslate",             "deepslate_top",          "deepslate",              "deepslate_top"},
    {"cobbled_deepslate",     "cobbled_deepslate",      "cobbled_deepslate",      "cobbled_deepslate"},
    {"polished_deepslate",    "polished_deepslate",     "polished_deepslate",     "polished_deepslate"},
    {"deepslate_bricks",      "deepslate_bricks",       "deepslate_bricks",       "deepslate_bricks"},
    {"deepslate_tiles",       "deepslate_tiles",        "deepslate_tiles",        "deepslate_tiles"},
    {"deepslate_coal_ore",    "deepslate_coal_ore",     "deepslate_coal_ore",     "deepslate_coal_ore"},
    {"deepslate_iron_ore",    "deepslate_iron_ore",     "deepslate_iron_ore",     "deepslate_iron_ore"},
    {"deepslate_gold_ore",    "deepslate_gold_ore",     "deepslate_gold_ore",     "deepslate_gold_ore"},
    {"deepslate_diamond_ore", "deepslate_diamond_ore",  "deepslate_diamond_ore",  "deepslate_diamond_ore"},
    {"deepslate_redstone_ore","deepslate_redstone_ore", "deepslate_redstone_ore", "deepslate_redstone_ore"},
    {"deepslate_lapis_ore",   "deepslate_lapis_ore",    "deepslate_lapis_ore",    "deepslate_lapis_ore"},
    {"deepslate_emerald_ore", "deepslate_emerald_ore",  "deepslate_emerald_ore",  "deepslate_emerald_ore"},
    {"deepslate_copper_ore",  "deepslate_copper_ore",   "deepslate_copper_ore",   "deepslate_copper_ore"},
    {"copper_ore",            "copper_ore",             "copper_ore",             "copper_ore"},
    {"emerald_ore",           "emerald_ore",            "emerald_ore",            "emerald_ore"},
    {"coal_block",            "coal_block",             "coal_block",             "coal_block"},
    {"iron_block",            "iron_block",             "iron_block",             "iron_block"},
    {"gold_block",            "gold_block",             "gold_block",             "gold_block"},
    {"diamond_block",         "diamond_block",          "diamond_block",          "diamond_block"},
    {"emerald_block",         "emerald_block",          "emerald_block",          "emerald_block"},
    {"redstone_block",        "redstone_block",         "redstone_block",         "redstone_block"},
    {"lapis_block",           "lapis_block",            "lapis_block",            "lapis_block"},
    {"copper_block",          "copper_block",           "copper_block",           "copper_block"},
    {"netherite_block",       "netherite_block",        "netherite_block",        "netherite_block"},
    {"raw_iron_block",        "raw_iron_block",         "raw_iron_block",         "raw_iron_block"},
    {"raw_gold_block",        "raw_gold_block",         "raw_gold_block",         "raw_gold_block"},
    {"raw_copper_block",      "raw_copper_block",       "raw_copper_block",       "raw_copper_block"},
    {"netherrack",            "netherrack",             "netherrack",             "netherrack"},
    {"soul_sand",             "soul_sand",              "soul_sand",              "soul_sand"},
    {"glowstone",             "glowstone",              "glowstone",              "glowstone"},
    {"magma",                 "magma",                  "magma",                  "magma"},
    {"basalt",                "basalt_top",             "basalt_side",            "basalt_top"},
    {"polished_basalt",       "polished_basalt_top",    "polished_basalt_side",   "polished_basalt_top"},
    {"smooth_basalt",         "smooth_basalt",          "smooth_basalt",          "smooth_basalt"},
    {"blackstone",            "blackstone_top",         "blackstone",             "blackstone_top"},
    {"polished_blackstone",   "polished_blackstone",    "polished_blackstone",    "polished_blackstone"},
    {"nether_bricks",         "nether_bricks",          "nether_bricks",          "nether_bricks"},
    {"red_nether_bricks",     "red_nether_bricks",      "red_nether_bricks",      "red_nether_bricks"},
    {"cracked_nether_bricks", "cracked_nether_bricks",  "cracked_nether_bricks",  "cracked_nether_bricks"},
    {"quartz_block",          "quartz_block_top",       "quartz_block_side",      "quartz_block_bottom"},
    {"terracotta",            "terracotta",             "terracotta",             "terracotta"},
    {"orange_wool",           "orange_wool",            "orange_wool",            "orange_wool"},
    {"magenta_wool",          "magenta_wool",           "magenta_wool",           "magenta_wool"},
    {"light_blue_wool",       "light_blue_wool",        "light_blue_wool",        "light_blue_wool"},
    {"green_wool",            "green_wool",             "green_wool",             "green_wool"},
    {"brown_wool",            "brown_wool",             "brown_wool",             "brown_wool"},
    {"cyan_wool",             "cyan_wool",              "cyan_wool",              "cyan_wool"},
    {"purple_wool",           "purple_wool",            "purple_wool",            "purple_wool"},
    {"pink_wool",             "pink_wool",              "pink_wool",              "pink_wool"},
    {"gray_wool",             "gray_wool",              "gray_wool",              "gray_wool"},
    {"light_gray_wool",       "light_gray_wool",        "light_gray_wool",        "light_gray_wool"},
    {"amethyst_block",        "amethyst_block",         "amethyst_block",         "amethyst_block"},
    {"dripstone_block",       "dripstone_block",        "dripstone_block",        "dripstone_block"},
    {"moss_block",            "moss_block",             "moss_block",             "moss_block"},
    {"purpur_block",          "purpur_block",           "purpur_block",           "purpur_block"},
    {"mossy_stone_bricks",    "mossy_stone_bricks",     "mossy_stone_bricks",     "mossy_stone_bricks"},
    {"cracked_stone_bricks",  "cracked_stone_bricks",   "cracked_stone_bricks",   "cracked_stone_bricks"},
    {"chiseled_stone_bricks", "chiseled_stone_bricks",  "chiseled_stone_bricks",  "chiseled_stone_bricks"},
    {"red_sandstone",         "red_sandstone_top",      "red_sandstone",          "red_sandstone_bottom"},
    {"red_sand",              "red_sand",               "red_sand",               "red_sand"},
    {"prismarine",            "prismarine",             "prismarine",             "prismarine"},
    {"prismarine_bricks",     "prismarine_bricks",      "prismarine_bricks",      "prismarine_bricks"},
    {"dark_prismarine",       "dark_prismarine",        "dark_prismarine",        "dark_prismarine"},
    {"sea_lantern",           "sea_lantern",            "sea_lantern",            "sea_lantern"},
    {"end_stone",             "end_stone",              "end_stone",              "end_stone"},
    {"end_stone_bricks",      "end_stone_bricks",       "end_stone_bricks",       "end_stone_bricks"},
    {"purpur_pillar",         "purpur_pillar_top",      "purpur_pillar_side",     "purpur_pillar_top"},
    {"nether_wart_block",     "nether_wart_block",      "nether_wart_block",      "nether_wart_block"},
    {"warped_wart_block",     "warped_wart_block",      "warped_wart_block",      "warped_wart_block"},
    {"shroomlight",           "shroomlight",            "shroomlight",            "shroomlight"},
    {"crying_obsidian",       "crying_obsidian",        "crying_obsidian",        "crying_obsidian"},
    {"ancient_debris",        "ancient_debris_top",     "ancient_debris_side",    "ancient_debris_top"},
    {"bone_block",            "bone_block_top",         "bone_block_side",        "bone_block_top"},
    {"hay_block",             "hay_block_top",          "hay_block_side",         "hay_block_top"},
    {"melon",                 "melon_top",              "melon_side",             "melon_top"},
    {"lava",                  "lava_still",             "lava_still",             "lava_still"},
    {"nether_quartz_ore",     "nether_quartz_ore",      "nether_quartz_ore",      "nether_quartz_ore"},
    {"nether_gold_ore",       "nether_gold_ore",        "nether_gold_ore",        "nether_gold_ore"},
    {"nether_portal",         "nether_portal",          "nether_portal",          "nether_portal"},
    {"soul_soil",             "soul_soil",              "soul_soil",              "soul_soil"},
    {"crimson_stem",          "crimson_stem_top",       "crimson_stem",           "crimson_stem_top"},
    {"warped_stem",           "warped_stem_top",        "warped_stem",            "warped_stem_top"},
    {"crimson_planks",        "crimson_planks",         "crimson_planks",         "crimson_planks"},
    {"warped_planks",         "warped_planks",          "warped_planks",          "warped_planks"},
    {"cherry_log",            "cherry_log_top",         "cherry_log",             "cherry_log_top"},
    {"cherry_leaves",         "cherry_leaves",          "cherry_leaves",          "cherry_leaves"},
    {"cherry_planks",         "cherry_planks",          "cherry_planks",          "cherry_planks"},
    // Vanilla renders a chest from entity/chest/normal.png via a block-entity
    // model; its block model only declares a particle texture. Until the
    // render phase grows block-entity meshes, the fallback uses that same
    // particle material (oak planks) instead of the retired magenta tile.
    {"chest",                 "oak_planks",           "oak_planks",           "oak_planks"},
};

const uint8_t BLOCK_FLAGS[BLOCK_COUNT] = {
    0,                                              // air
    BF_SOLID | BF_OPAQUE,                           // stone
    BF_SOLID | BF_OPAQUE,                           // dirt
    BF_SOLID | BF_OPAQUE,                           // grass
    BF_SOLID | BF_OPAQUE,                           // cobblestone
    BF_SOLID | BF_OPAQUE,                           // planks
    BF_SOLID | BF_OPAQUE,                           // log
    BF_SOLID | BF_OPAQUE,                           // sand
    BF_SOLID | BF_OPAQUE,                           // gravel
    BF_SOLID | BF_OPAQUE,                           // brick
    BF_SOLID | BF_OPAQUE,                           // bedrock
    BF_SOLID | BF_OPAQUE,                           // coal_ore
    BF_SOLID | BF_OPAQUE,                           // iron_ore
    BF_SOLID | BF_OPAQUE,                           // gold_ore
    BF_SOLID | BF_OPAQUE,                           // diamond_ore
    BF_SOLID | BF_OPAQUE,                           // redstone_ore
    BF_SOLID | BF_OPAQUE,                           // lapis_ore
    BF_SOLID | BF_OPAQUE,                           // granite
    BF_SOLID | BF_OPAQUE,                           // diorite
    BF_SOLID | BF_OPAQUE,                           // andesite
    BF_SOLID | BF_OPAQUE,                           // polished_granite
    BF_SOLID | BF_OPAQUE,                           // polished_diorite
    BF_SOLID | BF_OPAQUE,                           // polished_andesite
    BF_SOLID | BF_OPAQUE,                           // spruce_planks
    BF_SOLID | BF_OPAQUE,                           // birch_planks
    BF_SOLID | BF_OPAQUE,                           // spruce_log
    BF_SOLID | BF_OPAQUE,                           // birch_log
    BF_SOLID | BF_CUTOUT,                           // oak_leaves
    BF_SOLID | BF_CUTOUT,                           // spruce_leaves
    BF_SOLID | BF_CUTOUT,                           // birch_leaves
    BF_SOLID | BF_CUTOUT,                           // glass
    BF_SOLID | BF_OPAQUE,                           // sandstone
    BF_SOLID | BF_OPAQUE,                           // smooth_stone
    BF_SOLID | BF_OPAQUE,                           // stone_bricks
    BF_SOLID | BF_OPAQUE,                           // mossy_cobblestone
    BF_SOLID | BF_OPAQUE,                           // obsidian
    BF_SOLID | BF_OPAQUE,                           // tnt
    BF_SOLID | BF_OPAQUE,                           // crafting_table
    BF_SOLID | BF_OPAQUE,                           // furnace
    BF_SOLID | BF_OPAQUE,                           // bookshelf
    BF_SOLID | BF_OPAQUE,                           // white_wool
    BF_SOLID | BF_OPAQUE,                           // red_wool
    BF_SOLID | BF_OPAQUE,                           // blue_wool
    BF_SOLID | BF_OPAQUE,                           // lime_wool
    BF_SOLID | BF_OPAQUE,                           // yellow_wool
    BF_SOLID | BF_OPAQUE,                           // black_wool
    BF_LIQUID,                                      // water
    BF_SOLID | BF_OPAQUE,                           // ice
    BF_SOLID | BF_OPAQUE,                           // clay
    BF_SOLID | BF_OPAQUE,                           // snow
    BF_SOLID | BF_OPAQUE,                           // deepslate
    BF_SOLID | BF_OPAQUE,                           // cobbled_deepslate
    BF_SOLID | BF_OPAQUE,                           // polished_deepslate
    BF_SOLID | BF_OPAQUE,                           // deepslate_bricks
    BF_SOLID | BF_OPAQUE,                           // deepslate_tiles
    BF_SOLID | BF_OPAQUE,                           // deepslate_coal_ore
    BF_SOLID | BF_OPAQUE,                           // deepslate_iron_ore
    BF_SOLID | BF_OPAQUE,                           // deepslate_gold_ore
    BF_SOLID | BF_OPAQUE,                           // deepslate_diamond_ore
    BF_SOLID | BF_OPAQUE,                           // deepslate_redstone_ore
    BF_SOLID | BF_OPAQUE,                           // deepslate_lapis_ore
    BF_SOLID | BF_OPAQUE,                           // deepslate_emerald_ore
    BF_SOLID | BF_OPAQUE,                           // deepslate_copper_ore
    BF_SOLID | BF_OPAQUE,                           // copper_ore
    BF_SOLID | BF_OPAQUE,                           // emerald_ore
    BF_SOLID | BF_OPAQUE,                           // coal_block
    BF_SOLID | BF_OPAQUE,                           // iron_block
    BF_SOLID | BF_OPAQUE,                           // gold_block
    BF_SOLID | BF_OPAQUE,                           // diamond_block
    BF_SOLID | BF_OPAQUE,                           // emerald_block
    BF_SOLID | BF_OPAQUE,                           // redstone_block
    BF_SOLID | BF_OPAQUE,                           // lapis_block
    BF_SOLID | BF_OPAQUE,                           // copper_block
    BF_SOLID | BF_OPAQUE,                           // netherite_block
    BF_SOLID | BF_OPAQUE,                           // raw_iron_block
    BF_SOLID | BF_OPAQUE,                           // raw_gold_block
    BF_SOLID | BF_OPAQUE,                           // raw_copper_block
    BF_SOLID | BF_OPAQUE,                           // netherrack
    BF_SOLID | BF_OPAQUE,                           // soul_sand
    BF_SOLID | BF_OPAQUE,                           // glowstone
    BF_SOLID | BF_OPAQUE,                           // magma
    BF_SOLID | BF_OPAQUE,                           // basalt
    BF_SOLID | BF_OPAQUE,                           // polished_basalt
    BF_SOLID | BF_OPAQUE,                           // smooth_basalt
    BF_SOLID | BF_OPAQUE,                           // blackstone
    BF_SOLID | BF_OPAQUE,                           // polished_blackstone
    BF_SOLID | BF_OPAQUE,                           // nether_bricks
    BF_SOLID | BF_OPAQUE,                           // red_nether_bricks
    BF_SOLID | BF_OPAQUE,                           // cracked_nether_bricks
    BF_SOLID | BF_OPAQUE,                           // quartz_block
    BF_SOLID | BF_OPAQUE,                           // terracotta
    BF_SOLID | BF_OPAQUE,                           // orange_wool
    BF_SOLID | BF_OPAQUE,                           // magenta_wool
    BF_SOLID | BF_OPAQUE,                           // light_blue_wool
    BF_SOLID | BF_OPAQUE,                           // green_wool
    BF_SOLID | BF_OPAQUE,                           // brown_wool
    BF_SOLID | BF_OPAQUE,                           // cyan_wool
    BF_SOLID | BF_OPAQUE,                           // purple_wool
    BF_SOLID | BF_OPAQUE,                           // pink_wool
    BF_SOLID | BF_OPAQUE,                           // gray_wool
    BF_SOLID | BF_OPAQUE,                           // light_gray_wool
    BF_SOLID | BF_OPAQUE,                           // amethyst_block
    BF_SOLID | BF_OPAQUE,                           // dripstone_block
    BF_SOLID | BF_OPAQUE,                           // moss_block
    BF_SOLID | BF_OPAQUE,                           // purpur_block
    BF_SOLID | BF_OPAQUE,                           // mossy_stone_bricks
    BF_SOLID | BF_OPAQUE,                           // cracked_stone_bricks
    BF_SOLID | BF_OPAQUE,                           // chiseled_stone_bricks
    BF_SOLID | BF_OPAQUE,                           // red_sandstone
    BF_SOLID | BF_OPAQUE,                           // red_sand
    BF_SOLID | BF_OPAQUE,                           // prismarine
    BF_SOLID | BF_OPAQUE,                           // prismarine_bricks
    BF_SOLID | BF_OPAQUE,                           // dark_prismarine
    BF_SOLID | BF_OPAQUE,                           // sea_lantern
    BF_SOLID | BF_OPAQUE,                           // end_stone
    BF_SOLID | BF_OPAQUE,                           // end_stone_bricks
    BF_SOLID | BF_OPAQUE,                           // purpur_pillar
    BF_SOLID | BF_OPAQUE,                           // nether_wart_block
    BF_SOLID | BF_OPAQUE,                           // warped_wart_block
    BF_SOLID | BF_OPAQUE,                           // shroomlight
    BF_SOLID | BF_OPAQUE,                           // crying_obsidian
    BF_SOLID | BF_OPAQUE,                           // ancient_debris
    BF_SOLID | BF_OPAQUE,                           // bone_block
    BF_SOLID | BF_OPAQUE,                           // hay_block
    BF_SOLID | BF_OPAQUE,                           // melon
    BF_LIQUID,                                      // lava
    BF_SOLID | BF_OPAQUE,                           // nether_quartz_ore
    BF_SOLID | BF_OPAQUE,                           // nether_gold_ore
    BF_CUTOUT,                                      // nether_portal
    BF_SOLID | BF_OPAQUE,                           // soul_soil
    BF_SOLID | BF_OPAQUE,                           // crimson_stem
    BF_SOLID | BF_OPAQUE,                           // warped_stem
    BF_SOLID | BF_OPAQUE,                           // crimson_planks
    BF_SOLID | BF_OPAQUE                            // warped_planks
    , BF_SOLID | BF_OPAQUE                          // cherry_log
    , BF_SOLID | BF_CUTOUT                          // cherry_leaves
    , BF_SOLID | BF_OPAQUE                          // cherry_planks
    , BF_SOLID | BF_OPAQUE                          // chest
};

static const ToolDef TOOLS[] = {
    {"wooden_pickaxe", ToolKind::Pickaxe, 0, 59, 2, 2, 1.2f},
    {"wooden_axe", ToolKind::Axe, 0, 59, 2, 7, 0.8f},
    {"wooden_shovel", ToolKind::Shovel, 0, 59, 2, 2.5f, 1},
    {"wooden_sword", ToolKind::Sword, 0, 59, 1, 4, 1.6f},
    {"stone_pickaxe", ToolKind::Pickaxe, 1, 131, 4, 3, 1.2f},
    {"stone_axe", ToolKind::Axe, 1, 131, 4, 9, 0.8f},
    {"stone_shovel", ToolKind::Shovel, 1, 131, 4, 3.5f, 1},
    {"stone_sword", ToolKind::Sword, 1, 131, 1, 5, 1.6f},
    {"iron_pickaxe", ToolKind::Pickaxe, 2, 250, 6, 4, 1.2f},
    {"iron_axe", ToolKind::Axe, 2, 250, 6, 9, 0.9f},
    {"iron_shovel", ToolKind::Shovel, 2, 250, 6, 4.5f, 1},
    {"iron_sword", ToolKind::Sword, 2, 250, 1, 6, 1.6f},
    {"diamond_pickaxe", ToolKind::Pickaxe, 3, 1561, 8, 5, 1.2f},
    {"diamond_axe", ToolKind::Axe, 3, 1561, 8, 9, 1},
    {"diamond_shovel", ToolKind::Shovel, 3, 1561, 8, 5.5f, 1},
    {"diamond_sword", ToolKind::Sword, 3, 1561, 1, 7, 1.6f},
    {"golden_pickaxe", ToolKind::Pickaxe, 0, 32, 12, 2, 1.2f},
    {"golden_axe", ToolKind::Axe, 0, 32, 12, 7, 1},
    {"golden_shovel", ToolKind::Shovel, 0, 32, 12, 2.5f, 1},
    {"golden_sword", ToolKind::Sword, 0, 32, 1, 4, 1.6f},
    {"netherite_pickaxe", ToolKind::Pickaxe, 4, 2031, 9, 6, 1.2f},
    {"netherite_axe", ToolKind::Axe, 4, 2031, 9, 10, 1},
    {"netherite_shovel", ToolKind::Shovel, 4, 2031, 9, 6.5f, 1},
    {"netherite_sword", ToolKind::Sword, 4, 2031, 1, 8, 1.6f}
};

const ToolDef* FindTool(uint16_t id) {
    return id >= ITEM_WOOD_PICKAXE && id <= ITEM_NETHERITE_SWORD ? &TOOLS[id - ITEM_WOOD_PICKAXE] : nullptr;
}

// Java armor values: defense points, toughness and durability per piece.
static const ArmorDef ARMOR[] = {
    {"leather_helmet", 0, 1, 0, 55}, {"leather_chestplate", 1, 3, 0, 80},
    {"leather_leggings", 2, 2, 0, 75}, {"leather_boots", 3, 1, 0, 65},
    {"chainmail_helmet", 0, 2, 0, 165}, {"chainmail_chestplate", 1, 5, 0, 240},
    {"chainmail_leggings", 2, 4, 0, 225}, {"chainmail_boots", 3, 1, 0, 195},
    {"iron_helmet", 0, 2, 0, 165}, {"iron_chestplate", 1, 6, 0, 240},
    {"iron_leggings", 2, 5, 0, 225}, {"iron_boots", 3, 2, 0, 195},
    {"golden_helmet", 0, 2, 0, 77}, {"golden_chestplate", 1, 5, 0, 112},
    {"golden_leggings", 2, 3, 0, 105}, {"golden_boots", 3, 1, 0, 91},
    {"diamond_helmet", 0, 3, 2, 363}, {"diamond_chestplate", 1, 8, 2, 528},
    {"diamond_leggings", 2, 6, 2, 495}, {"diamond_boots", 3, 3, 2, 429},
    {"netherite_helmet", 0, 3, 3, 407}, {"netherite_chestplate", 1, 8, 3, 592},
    {"netherite_leggings", 2, 6, 3, 555}, {"netherite_boots", 3, 3, 3, 481}
};

const ArmorDef* FindArmor(uint16_t id) {
    if (id == ITEM_ELYTRA) {
        static const ArmorDef elytra{"elytra", 1, 0, 0, 432};
        return &elytra;
    }
    return id >= ITEM_LEATHER_HELMET && id <= ITEM_NETHERITE_BOOTS ? &ARMOR[id - ITEM_LEATHER_HELMET] : nullptr;
}

bool IsValidItem(int id) {
    if (id < 0 || id > 0xFFFF) return false;
    if (id < BLOCK_COUNT) return true;
    if (id == ITEM_STICK ||
        (id >= ITEM_WOOD_PICKAXE && id <= ITEM_NETHERITE_SWORD) ||
        (id >= ITEM_COAL && id <= ITEM_RAW_COPPER) ||
        (id >= ITEM_LEATHER_HELMET && id <= ITEM_NETHERITE_BOOTS) ||
        (id >= ITEM_ELYTRA && id <= ITEM_COPPER_INGOT))
        return true;
    // Jar registry tail: names and ids loaded by registry.cpp at boot.
    return id >= int(REGISTRY_ID_BASE) && id < int(BLOCK_ID_LIMIT) &&
        RegistryRuntimeHasId(uint16_t(id));
}

// Filled once at boot by SetBlockItemTexture(): the atlas key each block uses
// for its inventory icon. Empty means "use the built-in BLOCK_DEFS side face".
// Sized to the whole runtime id range so jar blocks share the same path.
static std::string gBlockItemTexture[BLOCK_TILE_SLOTS];

// Icon tile per id in the shared atlas (-1 = no icon). 32K ints = 128 KB.
static int gItemIconTile[BLOCK_TILE_SLOTS];
static bool gItemIconTileReady = false;

int ItemIconTile(uint16_t id) {
    if (!gItemIconTileReady) {
        std::fill(gItemIconTile, gItemIconTile + BLOCK_TILE_SLOTS, -1);
        gItemIconTileReady = true;
    }
    return id < BLOCK_ID_LIMIT ? gItemIconTile[id] : -1;
}

void SetItemIconTile(uint16_t id, int tile) {
    if (id >= BLOCK_ID_LIMIT) return;
    if (!gItemIconTileReady) {
        std::fill(gItemIconTile, gItemIconTile + BLOCK_TILE_SLOTS, -1);
        gItemIconTileReady = true;
    }
    gItemIconTile[id] = tile;
}

const char* ItemName(uint16_t id) {
    if (id < BLOCK_COUNT) return BLOCK_DEFS[id].name;
    if (const char* runtime = RegistryRuntimeName(id)) return runtime;
    if (id == ITEM_STICK) return "stick";
    if (const ToolDef* tool = FindTool(id)) return tool->name;
    if (const ArmorDef* armor = FindArmor(id)) return armor->name;
    static const char* materials[] = {"coal", "raw_iron", "raw_gold", "diamond", "redstone", "lapis_lazuli",
                                      "iron_ingot", "gold_ingot", "netherite_ingot", "emerald", "raw_copper"};
    if (id >= ITEM_COAL && id <= ITEM_RAW_COPPER) return materials[id - ITEM_COAL];
    switch (id) {
        case ITEM_FLINT_AND_STEEL: return "flint_and_steel";
        case ITEM_QUARTZ: return "quartz";
        case ITEM_FIREWORK_ROCKET: return "firework_rocket";
        case ITEM_COPPER_INGOT: return "copper_ingot";
        default: break;
    }
    return "unknown";
}

std::string ItemTexture(uint16_t id) {
    if (id < BLOCK_TILE_SLOTS) {
        // Model-resolved side texture when the jar provided one: a chest icon
        // then matches the block in the world instead of the retired table.
        const std::string& key = gBlockItemTexture[id];
        if (!key.empty()) return std::string("minecraft/textures/") + key + ".png";
    }
    if (id < BLOCK_COUNT)
        return std::string("minecraft/textures/block/") + BLOCK_DEFS[id].side + ".png";
    return std::string("minecraft/textures/item/") + ItemName(id) + ".png";
}

bool LoadItemRGBA(AssetStore& store, uint16_t id, uint8_t* out16) {
    std::vector<uint8_t> bytes;
    if (!store.ReadFile(ItemTexture(id), bytes)) return false;
    return DecodePNG16(bytes, out16);
}

int ItemStackLimit(uint16_t id) {
    if (id == ITEM_FLINT_AND_STEEL) return 1;
    return FindTool(id) || FindArmor(id) ? 1 : 64;
}

// Vanilla grass/foliage/water textures are grayscale and biome-tinted by
// Java at runtime. Bake the exact plains colors sampled from the 26.3
// colormaps (grass #8EB971, foliage #71A74D, water #3F76E4) so no shader
// tint path is needed.
static void TintTile(uint8_t* px, int r, int g, int b, int rows, uint8_t alpha) {
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < ATLAS_TILE; ++x) {
            uint8_t* p = px + (y * ATLAS_TILE + x) * 4;
            p[0] = uint8_t(p[0] * r / 255);
            p[1] = uint8_t(p[1] * g / 255);
            p[2] = uint8_t(p[2] * b / 255);
            if (alpha) p[3] = uint8_t(p[3] * alpha / 255);
        }
    }
}

static void ApplyBiomeTint(const std::string& name, uint8_t* px) {
    if (name.find("grass_block_top") != std::string::npos) {
        TintTile(px, 0x8E, 0xB9, 0x71, ATLAS_TILE, 0);
    } else if (name.find("grass_block_side") != std::string::npos) {
        TintTile(px, 0x8E, 0xB9, 0x71, 4, 0); // overlay strip only; rest is dirt
    } else if (name.find("oak_leaves") != std::string::npos ||
               name.find("birch_leaves") != std::string::npos) {
        TintTile(px, 0x71, 0xA7, 0x4D, ATLAS_TILE, 0);
    } else if (name.find("spruce_leaves") != std::string::npos) {
        TintTile(px, 0x61, 0x99, 0x61, ATLAS_TILE, 0);
    } else if (name.find("water_still") != std::string::npos) {
        // alpha 0 preserves Java's baked 180/255 water opacity; tint RGB only.
        // Scaling it down pushed water under the opaque pass alpha clip.
        TintTile(px, 0x3F, 0x76, 0xE4, ATLAS_TILE, 0);
    } else if (name.find("/ice.png") != std::string::npos) {
        for (int i = 0; i < ATLAS_TILE * ATLAS_TILE; ++i) px[i * 4 + 3] = 255;
    }
}

// ---------------------------------------------------------------------------
// JAR-driven block textures (M1 step 1.2)
// ---------------------------------------------------------------------------
//
// Vanilla keeps every block -> texture binding inside the client jar, not in
// engine code:
//
//   blockstates/<block>.json   -> variants/multipart pick a model
//   models/block/<model>.json  -> "parent" chain + a textures map
//   textures/block/<name>.png  -> the pixels
//
// Reading that chain means a renamed texture, a resource pack or a block whose
// model lives under textures/entity/ (chest) can never paint the magenta
// "missing" tile again. The built-in BLOCK_DEFS table stays only as a fallback
// for a jar-less run.

namespace {

bool LoadJsonEntry(AssetStore& store, const std::string& path, nlohmann::json& out) {
    std::vector<uint8_t> bytes;
    if (!store.ReadFile(path, bytes) || bytes.empty()) return false;
    try {
        out = nlohmann::json::parse(bytes.begin(), bytes.end());
    } catch (const std::exception&) {
        return false;
    }
    return out.is_object();
}

// "minecraft:block/cube_all" / "block/cube_all" -> "minecraft/models/block/cube_all.json"
std::string BlockModelPath(const std::string& ref) {
    std::string s = ref;
    const size_t colon = s.find(':');
    if (colon != std::string::npos) s = s.substr(colon + 1);
    if (s.compare(0, 7, "models/") == 0) s = s.substr(7);
    return "minecraft/models/" + s + ".json";
}

// "minecraft:block/oak_planks" -> "block/oak_planks" (atlas key form).
std::string TextureAtlasKey(const std::string& ref) {
    std::string s = ref;
    const size_t colon = s.find(':');
    if (colon != std::string::npos) s = s.substr(colon + 1);
    if (s.size() >= 9 && s.compare(0, 9, "textures/") == 0) s = s.substr(9);
    return s;
}

std::string ModelFromApply(const nlohmann::json& apply) {
    const nlohmann::json* node = &apply;
    if (apply.is_array()) {
        if (apply.empty()) return {};
        node = &apply[0];
    }
    if (!node->is_object()) return {};
    if (node->contains("model") && (*node)["model"].is_string())
        return (*node)["model"].get<std::string>();
    return {};
}

// First declared model of a blockstate. Vanilla's single "" variant is the
// canonical default; anything else (facing=..., multipart) is data-driven too,
// so the first entry keeps the mesher deterministic.
std::string FirstStateModel(const nlohmann::json& state) {
    if (state.contains("variants") && state["variants"].is_object()) {
        const auto& variants = state["variants"];
        if (variants.contains("")) return ModelFromApply(variants[""]);
        for (auto it = variants.begin(); it != variants.end(); ++it) {
            const std::string model = ModelFromApply(it.value());
            if (!model.empty()) return model;
        }
    }
    if (state.contains("multipart") && state["multipart"].is_array()) {
        for (const auto& part : state["multipart"]) {
            if (!part.is_object() || !part.contains("apply")) continue;
            const std::string model = ModelFromApply(part["apply"]);
            if (!model.empty()) return model;
        }
    }
    return {};
}

// A vanilla texture value is either a string or an object such as
// {"force_translucent": true, "sprite": "minecraft:block/glass"}.
std::string TextureRefValue(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_object()) {
        if (value.contains("sprite") && value["sprite"].is_string())
            return value["sprite"].get<std::string>();
        if (value.contains("texture") && value["texture"].is_string())
            return value["texture"].get<std::string>();
    }
    return {};
}

// Facts a parent chain contributes. Textures merge parent-first; elements are
// overridden by the deepest model that declares them, exactly like vanilla.
struct ModelWalk {
    nlohmann::json textures = nlohmann::json::object();
    bool hasElements = false;
    bool fullCube = false;
    bool forceTranslucent = false;
    bool cutoutRender = false;
};

bool IsFullCubeElement(const nlohmann::json& elements) {
    if (!elements.is_array() || elements.size() != 1) return false;
    const nlohmann::json& e = elements[0];
    if (!e.is_object() || !e.contains("from") || !e.contains("to")) return false;
    auto axisBox = [&](const char* key, float expected) {
        const nlohmann::json& v = e[key];
        if (!v.is_array() || v.size() != 3) return false;
        for (const auto& axis : v) {
            if (!axis.is_number()) return false;
            if (std::fabs(axis.get<float>() - expected) > 0.001f) return false;
        }
        return true;
    };
    if (!axisBox("from", 0.0f) || !axisBox("to", 16.0f)) return false;
    if (!e.contains("faces") || !e["faces"].is_object()) return false;
    for (const char* face : {"down", "up", "north", "south", "east", "west"})
        if (!e["faces"].contains(face)) return false;
    return true;
}

void WalkModel(AssetStore& store, const std::string& modelRef, ModelWalk& walk, int depth) {
    if (depth > 8 || modelRef.empty()) return;
    nlohmann::json model;
    if (!LoadJsonEntry(store, BlockModelPath(modelRef), model)) return;
    if (model.contains("parent") && model["parent"].is_string())
        WalkModel(store, model["parent"].get<std::string>(), walk, depth + 1);
    if (model.contains("textures") && model["textures"].is_object()) {
        for (auto it = model["textures"].begin(); it != model["textures"].end(); ++it) {
            const std::string ref = TextureRefValue(it.value());
            if (!ref.empty()) walk.textures[it.key()] = ref;
            if (it.value().is_object() && it.value().value("force_translucent", false))
                walk.forceTranslucent = true;
        }
    }
    if (model.contains("elements") && model["elements"].is_array()) {
        walk.hasElements = true;
        walk.fullCube = IsFullCubeElement(model["elements"]);
    }
    if (model.contains("render_type") && model["render_type"].is_string()) {
        const std::string type = model["render_type"].get<std::string>();
        walk.cutoutRender = type.find("cutout") != std::string::npos ||
                            type.find("translucent") != std::string::npos;
    }
}

// "#side" -> "minecraft:block/oak_planks" -> "block/oak_planks".
std::string LookupTextureKey(const nlohmann::json& textures, const char* name, int depth) {
    if (depth > 8 || !name || !*name) return {};
    const auto it = textures.find(name);
    if (it == textures.end()) return {};
    const std::string value = TextureRefValue(*it);
    if (value.empty()) return {};
    if (value[0] == '#') return LookupTextureKey(textures, value.c_str() + 1, depth + 1);
    return TextureAtlasKey(value);
}

// Face alias order taken from vanilla's cube/cube_all/cube_bottom_top/leaves and
// grass_block models. "particle" is the last resort: block-entity models such
// as block/chest declare only that, and vanilla itself uses it for break
// particles, so a cube drawn from it is at least the correct material.
std::string PickFace(const nlohmann::json& textures, const char* const* keys, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const std::string key = LookupTextureKey(textures, keys[i], 0);
        if (!key.empty()) return key;
    }
    return {};
}

const char* const kTopKeys[] = {"up", "top", "end", "all", "particle"};
const char* const kSideKeys[] = {"north", "side", "front", "all", "particle"};
const char* const kBottomKeys[] = {"down", "bottom", "end", "all", "particle"};

} // namespace

BlockModelInfo ResolveBlockModel(AssetStore& store, const std::string& blockName) {
    BlockModelInfo out;
    if (blockName.empty() || blockName == "air") return out;
    nlohmann::json state;
    if (!LoadJsonEntry(store, "minecraft/blockstates/" + blockName + ".json", state)) return out;
    const std::string model = FirstStateModel(state);
    if (model.empty()) return out;
    ModelWalk walk;
    WalkModel(store, model, walk, 0);
    if (walk.textures.empty()) return out;
    out.hasModel = true;
    // Vanilla's own full-cube models (block/cube, block/cube_all, block/cube_column,
    // leaves) all reduce to one [0,0,0]-[16,16,16] element with six faces, so the
    // element test alone is both necessary and sufficient.
    out.fullCube = walk.fullCube;
    // cutout: the model asks for an alpha-tested/translucent render layer, or the
    // sprite is a force_translucent overlay (glass, ice).
    out.cutout = walk.cutoutRender || walk.forceTranslucent;
    out.textures.top = PickFace(walk.textures, kTopKeys, sizeof(kTopKeys) / sizeof(kTopKeys[0]));
    out.textures.side = PickFace(walk.textures, kSideKeys, sizeof(kSideKeys) / sizeof(kSideKeys[0]));
    out.textures.bottom = PickFace(walk.textures, kBottomKeys, sizeof(kBottomKeys) / sizeof(kBottomKeys[0]));
    // leaves and ice are cutout by material, not by render_type: the jar declares
    // no render_type on them. Names come from the jar, so this stays data-driven.
    const std::string& side = out.textures.side;
    if (side.find("leaves") != std::string::npos || side.find("_ice") != std::string::npos ||
        side.find("glass") != std::string::npos || blockName.find("leaves") != std::string::npos)
        out.cutout = true;
    return out;
}

BlockTextureSet ResolveBlockTextures(AssetStore& store, const std::string& blockName) {
    return ResolveBlockModel(store, blockName).textures;
}

// Runtime registry tail: full cubes the jar appended. Filled by registry.cpp.
uint8_t REGISTRY_BLOCK_FLAGS[BLOCK_TILE_SLOTS];

// Registry-resident name lookup for the appended id range (registry.cpp owns the
// table; this is the one accessor assets.cpp needs).
void SetBlockItemTexture(uint16_t id, const std::string& atlasKey) {
    if (id < BLOCK_TILE_SLOTS) gBlockItemTexture[id] = atlasKey;
}

std::vector<std::string> MissingBlockTextures(AssetStore& store) {
    const std::vector<std::string> names = store.ListPrefix("minecraft/textures/block/");
    std::unordered_map<std::string, bool> present;
    present.reserve(names.size() * 2);
    for (const std::string& name : names) present.emplace(name, true);
    std::vector<std::string> missing;
    for (int b = 1; b < BLOCK_COUNT; ++b) {
        const char* const faces[3] = {BLOCK_DEFS[b].top, BLOCK_DEFS[b].side, BLOCK_DEFS[b].bottom};
        for (const char* face : faces) {
            if (!face || !*face) continue;
            const std::string path = std::string("minecraft/textures/block/") + face + ".png";
            if (!present.count(path)) missing.push_back(path);
        }
    }
    std::sort(missing.begin(), missing.end());
    missing.erase(std::unique(missing.begin(), missing.end()), missing.end());
    return missing;
}

int BuildBlockAtlas(AssetStore& store, Atlas& atlas) {
    atlas.AddCheckerTile("__missing"); // tile 0 is always the fallback
    int loaded = 0;
    uint8_t px[ATLAS_TILE * ATLAS_TILE * 4];
    // One atlas for blocks and items: 1,406 block + 832 item PNGs still leave
    // half of the 1024x1024 sheet free, and every icon then costs one UV rect
    // instead of its own 16 KB GPU texture.
    const char* const prefixes[] = {"minecraft/textures/block/", "minecraft/textures/item/"};
    for (const char* prefix : prefixes) {
        const std::vector<std::string> names = store.ListPrefix(prefix);
        for (const std::string& name : names) {
            if (name.size() < 4 || name.compare(name.size() - 4, 4, ".png") != 0) continue;
            std::vector<uint8_t> bytes;
            if (!store.ReadFile(name, bytes)) continue;
            if (!DecodePNG16(bytes, px)) continue;
            ApplyBiomeTint(name, px);
            if (atlas.AddTile(name, px) >= 0) ++loaded;
        }
    }
    return loaded;
}

int BindItemIconTiles(Atlas& atlas) {
    int bound = 0;
    for (int i = 1; i < BLOCK_TILE_SLOTS; ++i) {
        const uint16_t id = uint16_t(i);
        if (!IsValidItem(id)) continue;
        const int tile = atlas.FindTile(ItemTexture(id));
        if (tile < 0) continue;
        SetItemIconTile(id, tile);
        ++bound;
    }
    return bound;
}

} // namespace potato