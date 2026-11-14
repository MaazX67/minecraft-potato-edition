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

// The asset index only carries sounds/misc; every texture, font and GUI
// sprite lives inside the client jar (assets/minecraft/...). This finds the
// newest installed version jar under Migurinth/meta/versions.
std::string DefaultClientJarPath();

// Minimal read-only ZIP (jar) archive: central directory parsed at open,
// entries inflated on demand with stb's raw-deflate decoder. No dependency.
class ZipArchive {
public:
    bool Open(const std::string& path);
    bool Read(const std::string& name, std::vector<uint8_t>& out) const;
    std::vector<std::string> List() const;
    size_t EntryCount() const { return mEntries.size(); }

private:
    struct Entry {
        uint32_t localOffset = 0, compSize = 0, uncompSize = 0;
        uint16_t method = 0;
    };
    std::string mPath;
    std::unordered_map<std::string, Entry> mEntries;
};

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
    bool LoadJar(const std::string& jarPath) { return mJar.Open(jarPath); }
    bool HasJar() const { return mJar.EntryCount() > 0; }
    bool ReadFile(const std::string& name, std::vector<uint8_t>& out) const;
    std::vector<std::string> ListPrefix(const std::string& prefix) const;
    // Raw archive access for data/ entries (recipes, loot tables), which are
    // not under assets/ and therefore invisible to ReadFile/ListPrefix.
    bool ReadJarEntry(const std::string& path, std::vector<uint8_t>& out) const { return mJar.Read(path, out); }
    std::vector<std::string> ListJarPrefix(const std::string& prefix) const;
    size_t EntryCount() const { return mObjects.size() + mJar.EntryCount(); }

private:
    std::unordered_map<std::string, std::string> mObjects; // logical name -> sha1
    std::string mObjectsDir;
    ZipArchive mJar; // textures/GUI/font, names prefixed with "assets/"
};

enum BlockId : uint16_t {
    BLOCK_AIR = 0, BLOCK_STONE, BLOCK_DIRT, BLOCK_GRASS, BLOCK_COBBLE,
    BLOCK_PLANKS, BLOCK_LOG, BLOCK_SAND, BLOCK_GRAVEL, BLOCK_BRICK,
    BLOCK_BEDROCK, BLOCK_COAL_ORE, BLOCK_IRON_ORE, BLOCK_GOLD_ORE,
    BLOCK_DIAMOND_ORE, BLOCK_REDSTONE_ORE, BLOCK_LAPIS_ORE,
    BLOCK_GRANITE, BLOCK_DIORITE, BLOCK_ANDESITE,
    BLOCK_POLISHED_GRANITE, BLOCK_POLISHED_DIORITE, BLOCK_POLISHED_ANDESITE,
    BLOCK_SPRUCE_PLANKS, BLOCK_BIRCH_PLANKS, BLOCK_SPRUCE_LOG, BLOCK_BIRCH_LOG,
    BLOCK_OAK_LEAVES, BLOCK_SPRUCE_LEAVES, BLOCK_BIRCH_LEAVES,
    BLOCK_GLASS, BLOCK_SANDSTONE, BLOCK_SMOOTH_STONE, BLOCK_STONE_BRICKS,
    BLOCK_MOSSY_COBBLE, BLOCK_OBSIDIAN, BLOCK_TNT, BLOCK_CRAFTING_TABLE,
    BLOCK_FURNACE, BLOCK_BOOKSHELF, BLOCK_WHITE_WOOL, BLOCK_RED_WOOL,
    BLOCK_BLUE_WOOL, BLOCK_LIME_WOOL, BLOCK_YELLOW_WOOL, BLOCK_BLACK_WOOL,
    BLOCK_WATER, BLOCK_ICE, BLOCK_CLAY, BLOCK_SNOW,
    // Deepslate family and deepslate ores (Java deepslate layer).
    BLOCK_DEEPSLATE, BLOCK_COBBLED_DEEPSLATE, BLOCK_POLISHED_DEEPSLATE,
    BLOCK_DEEPSLATE_BRICKS, BLOCK_DEEPSLATE_TILES,
    BLOCK_DEEPSLATE_COAL_ORE, BLOCK_DEEPSLATE_IRON_ORE, BLOCK_DEEPSLATE_GOLD_ORE,
    BLOCK_DEEPSLATE_DIAMOND_ORE, BLOCK_DEEPSLATE_REDSTONE_ORE, BLOCK_DEEPSLATE_LAPIS_ORE,
    BLOCK_DEEPSLATE_EMERALD_ORE, BLOCK_DEEPSLATE_COPPER_ORE,
    BLOCK_COPPER_ORE, BLOCK_EMERALD_ORE,
    // Mineral storage blocks.
    BLOCK_COAL_BLOCK, BLOCK_IRON_BLOCK, BLOCK_GOLD_BLOCK, BLOCK_DIAMOND_BLOCK,
    BLOCK_EMERALD_BLOCK, BLOCK_REDSTONE_BLOCK, BLOCK_LAPIS_BLOCK, BLOCK_COPPER_BLOCK,
    BLOCK_NETHERITE_BLOCK, BLOCK_RAW_IRON_BLOCK, BLOCK_RAW_GOLD_BLOCK, BLOCK_RAW_COPPER_BLOCK,
    // Nether / basalt / blackstone / quartz.
    BLOCK_NETHERRACK, BLOCK_SOUL_SAND, BLOCK_GLOWSTONE, BLOCK_MAGMA,
    BLOCK_BASALT, BLOCK_POLISHED_BASALT, BLOCK_SMOOTH_BASALT,
    BLOCK_BLACKSTONE, BLOCK_POLISHED_BLACKSTONE,
    BLOCK_NETHER_BRICKS, BLOCK_RED_NETHER_BRICKS, BLOCK_CRACKED_NETHER_BRICKS, BLOCK_QUARTZ_BLOCK,
    // Building palette: terracotta, remaining wool colors, misc full cubes.
    BLOCK_TERRACOTTA,
    BLOCK_ORANGE_WOOL, BLOCK_MAGENTA_WOOL, BLOCK_LIGHT_BLUE_WOOL, BLOCK_GREEN_WOOL,
    BLOCK_BROWN_WOOL, BLOCK_CYAN_WOOL, BLOCK_PURPLE_WOOL, BLOCK_PINK_WOOL,
    BLOCK_GRAY_WOOL, BLOCK_LIGHT_GRAY_WOOL,
    BLOCK_AMETHYST_BLOCK, BLOCK_DRIPSTONE_BLOCK, BLOCK_MOSS_BLOCK, BLOCK_PURPUR_BLOCK,
    // Stone-brick variants, red sandstone, prismarine, End and Nether palette.
    BLOCK_MOSSY_STONE_BRICKS, BLOCK_CRACKED_STONE_BRICKS, BLOCK_CHISELED_STONE_BRICKS,
    BLOCK_RED_SANDSTONE, BLOCK_RED_SAND,
    BLOCK_PRISMARINE, BLOCK_PRISMARINE_BRICKS, BLOCK_DARK_PRISMARINE, BLOCK_SEA_LANTERN,
    BLOCK_END_STONE, BLOCK_END_STONE_BRICKS, BLOCK_PURPUR_PILLAR,
    BLOCK_NETHER_WART_BLOCK, BLOCK_WARPED_WART_BLOCK, BLOCK_SHROOMLIGHT,
    BLOCK_CRYING_OBSIDIAN, BLOCK_ANCIENT_DEBRIS,
    BLOCK_BONE_BLOCK, BLOCK_HAY_BLOCK, BLOCK_MELON,
    // Nether dimension fluids, ores, portal and wood (Java 1.16+ palette).
    BLOCK_LAVA, BLOCK_NETHER_QUARTZ_ORE, BLOCK_NETHER_GOLD_ORE, BLOCK_PORTAL,
    BLOCK_SOUL_SOIL, BLOCK_CRIMSON_STEM, BLOCK_WARPED_STEM,
    BLOCK_CRIMSON_PLANKS, BLOCK_WARPED_PLANKS,
    // Cherry grove (Java 1.20) and storage.
    BLOCK_CHERRY_LOG, BLOCK_CHERRY_LEAVES, BLOCK_CHERRY_PLANKS, BLOCK_CHEST,
    BLOCK_COUNT
};

// Items live above the block range so BLOCK_COUNT can grow without colliding.
// The 160 base leaves headroom over BLOCK_COUNT (138); the whole layout is the
// original 128 base shifted +32. Legacy saves (format 1/2) stored items at
// 64..104; LoadWorldSave remaps them with +96.
enum ItemId : uint16_t {
    ITEM_STICK = 160,
    ITEM_WOOD_PICKAXE, ITEM_WOOD_AXE, ITEM_WOOD_SHOVEL, ITEM_WOOD_SWORD,
    ITEM_STONE_PICKAXE, ITEM_STONE_AXE, ITEM_STONE_SHOVEL, ITEM_STONE_SWORD,
    ITEM_IRON_PICKAXE, ITEM_IRON_AXE, ITEM_IRON_SHOVEL, ITEM_IRON_SWORD,
    ITEM_DIAMOND_PICKAXE, ITEM_DIAMOND_AXE, ITEM_DIAMOND_SHOVEL, ITEM_DIAMOND_SWORD,
    ITEM_GOLD_PICKAXE, ITEM_GOLD_AXE, ITEM_GOLD_SHOVEL, ITEM_GOLD_SWORD,
    ITEM_NETHERITE_PICKAXE, ITEM_NETHERITE_AXE, ITEM_NETHERITE_SHOVEL, ITEM_NETHERITE_SWORD,
    ITEM_COAL = 192, ITEM_RAW_IRON, ITEM_RAW_GOLD, ITEM_DIAMOND,
    ITEM_REDSTONE, ITEM_LAPIS, ITEM_IRON_INGOT, ITEM_GOLD_INGOT, ITEM_NETHERITE_INGOT,
    ITEM_EMERALD, ITEM_RAW_COPPER,
    ITEM_LEATHER_HELMET = 203, ITEM_LEATHER_CHESTPLATE, ITEM_LEATHER_LEGGINGS, ITEM_LEATHER_BOOTS,
    ITEM_CHAINMAIL_HELMET, ITEM_CHAINMAIL_CHESTPLATE, ITEM_CHAINMAIL_LEGGINGS, ITEM_CHAINMAIL_BOOTS,
    ITEM_IRON_HELMET, ITEM_IRON_CHESTPLATE, ITEM_IRON_LEGGINGS, ITEM_IRON_BOOTS,
    ITEM_GOLD_HELMET, ITEM_GOLD_CHESTPLATE, ITEM_GOLD_LEGGINGS, ITEM_GOLD_BOOTS,
    ITEM_DIAMOND_HELMET, ITEM_DIAMOND_CHESTPLATE, ITEM_DIAMOND_LEGGINGS, ITEM_DIAMOND_BOOTS,
    ITEM_NETHERITE_HELMET, ITEM_NETHERITE_CHESTPLATE, ITEM_NETHERITE_LEGGINGS, ITEM_NETHERITE_BOOTS,
    ITEM_ELYTRA = 227, ITEM_FLINT_AND_STEEL, ITEM_QUARTZ, ITEM_FIREWORK_ROCKET, ITEM_COPPER_INGOT
};
static_assert(int(ITEM_STICK) >= int(BLOCK_COUNT), "item IDs must stay above the block range");

enum class ToolKind { None, Pickaxe, Axe, Shovel, Sword };
struct ToolDef {
    const char* name;
    ToolKind kind;
    int tier, durability;
    float speed, damage, attackSpeed;
};
const ToolDef* FindTool(uint16_t id);
struct ArmorDef {
    const char* name;
    int slot; // 0 helmet, 1 chestplate, 2 leggings, 3 boots
    int defense;
    float toughness;
    int durability;
};
const ArmorDef* FindArmor(uint16_t id);
bool IsValidItem(int id);
const char* ItemName(uint16_t id);
std::string ItemTexture(uint16_t id);
int ItemStackLimit(uint16_t id);

// SOLID: collides with the player. OPAQUE: culls neighbor faces. CUTOUT:
// alpha-tested (leaves/glass). LIQUID: rendered in the transparent pass.
enum BlockFlags : uint8_t {
    BF_SOLID = 1, BF_OPAQUE = 2, BF_CUTOUT = 4, BF_LIQUID = 8
};
extern const uint8_t BLOCK_FLAGS[BLOCK_COUNT];

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

// Slices one 16x16 frame out of a decoded animated texture (vertical strips
// such as water_still.png 16x512). Static 16x16 input copies through; other
// sizes are nearest-neighbor resampled. out must hold 16*16*4 bytes.
void SliceAnimatedFrame(const uint8_t* rgba, int w, int h, int frame, uint8_t* out16);

// Loads an item/block's 16x16 RGBA pixels (frame 0 for animated strips) from
// the asset store. False if the texture is absent. out must hold 16*16*4 bytes.
bool LoadItemRGBA(AssetStore& store, uint16_t id, uint8_t* out16);

// Decodes a PNG at its native size (GUI sprites, font). out is w*h*4 RGBA.
bool DecodePNGAny(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& out, int& w, int& h);

// Loads every minecraft/textures/block/*.png into the atlas.
// Tile 0 is always the missing-texture checker. Returns block tiles loaded.
int BuildBlockAtlas(AssetStore& store, Atlas& atlas);

} // namespace potato