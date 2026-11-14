#pragma once
#include "assets.h"
#include "engine.h"
#include <string>
#include <vector>

namespace potato {

// Ingredient tags referenced by Java recipe JSON. Values live above the item
// id range so one uint16 cell can hold either.
enum RecipeTag : uint16_t {
    TAG_NONE = 0,
    TAG_PLANKS = 0x8000,
    TAG_LOGS,
    TAG_WOOL,
    TAG_COALS,
    TAG_STONE_CRAFTING,
    TAG_COAL_ORES,
    TAG_IRON_ORES,
    TAG_GOLD_ORES,
    TAG_COPPER_ORES,
    TAG_DIAMOND_ORES,
    TAG_REDSTONE_ORES,
    TAG_LAPIS_ORES,
    TAG_EMERALD_ORES,
    TAG_QUARTZ_ORES
};

struct RegistryStats {
    bool jarLoaded = false;
    bool recipesFromJar = false;
    bool lootFromJar = false;
    int mappedBlockNames = 0; // JAR block names that map to an internal id
    int mappedItemNames = 0;  // JAR item names that map to an internal id
    int recipes = 0;           // JAR crafting recipes usable by this engine
    int lootTables = 0;        // JAR block loot tables parsed
};

// Parses Java's data/minecraft/recipe and data/minecraft/loot_table/blocks
// entries out of the loaded asset store. Safe to call without a jar: returns
// false and leaves the built-in fallback tables in place.
bool LoadRegistry(AssetStore& store);

const RegistryStats& RegistryInfo();

// One machine-readable parity line for the loaded registry:
//   PARITY-REGISTRY: jar=yes blocks=136 items=66 recipes=64 loot=... names=202 dup=0 hash=1a2b3c4d
// `names` counts distinct resource names over the whole id range and `hash` is
// FNV-1a over the sorted name->id table, so any change to the mapping is visible
// as a different hash. Safe without a jar (reports jar=no).
std::string RegistryParityLine();

// Recipes read from the jar. Empty until LoadRegistry succeeds; the crafting
// engine then keeps using its built-in Java-parity table.
const std::vector<CraftingRecipe>& RegistryRecipes();

// Name mapping ("minecraft:oak_planks" or "oak_planks" -> BLOCK_PLANKS).
uint16_t ItemIdFromResourceName(const std::string& name);
std::string ResourceNameForItem(uint16_t id);

// True when a recipe cell (item id or tag) accepts the given item id.
bool RecipeCellMatches(uint16_t cell, uint16_t id);

// Single-item block loot table result. Mirrors the simple pools used by the
// vanilla block tables; fortune/silk-touch variants fall back to base counts.
struct LootEntry {
    uint16_t item = 0;
    int count = 1;
};
// False when the jar has no usable table for this block.
bool LootDropForBlock(uint16_t block, LootEntry& out);

// Testing seams: parse one Java 26.3 resource file. Both never throw.
bool ParseRecipeJson(const std::string& text, CraftingRecipe& out);
bool ParseLootTableJson(const std::string& text, LootEntry& out, bool& hasItem);

} // namespace potato
