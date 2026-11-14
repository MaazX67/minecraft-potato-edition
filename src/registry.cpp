#include "registry.h"
#include "json.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace potato {
namespace {

RegistryStats gStats;
std::vector<CraftingRecipe> gRecipes;
std::unordered_map<std::string, uint8_t> gNameToId;
std::unordered_map<uint8_t, LootEntry> gLoot;

bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string NormalizeName(const std::string& name) {
    const size_t colon = name.find(':');
    if (colon == std::string::npos) return name;
    return name.substr(colon + 1);
}

std::string ItemNameSafe(uint8_t id) {
    const char* n = ItemName(id);
    return n ? n : "";
}

void BuildNameMap() {
    if (!gNameToId.empty()) return;
    for (int i = 1; i < BLOCK_COUNT; ++i) gNameToId.emplace(BLOCK_DEFS[i].name, uint8_t(i));
    // Java 1.13+ renamed the block; our palette kept the short name.
    gNameToId["grass_block"] = BLOCK_GRASS;
    for (int i = BLOCK_COUNT; i < 256; ++i) {
        if (!IsValidItem(i)) continue;
        gNameToId.emplace(ItemNameSafe(uint8_t(i)), uint8_t(i));
    }
}

bool IsTagId(uint16_t cell) { return cell >= 0x100; }

// Vanilla ingredient tags that this palette can satisfy. Tags outside this
// list (saplings, dyes, banners, ...) are skipped and their recipe ignored.
uint16_t TagFromName(const std::string& tag) {
    if (tag == "planks" || EndsWith(tag, "_planks")) return TAG_PLANKS;
    if (tag == "logs" || EndsWith(tag, "_logs") || EndsWith(tag, "_log") ||
        tag == "logs_that_burn" || EndsWith(tag, "_stems") || EndsWith(tag, "_stem"))
        return TAG_LOGS;
    if (tag == "wool" || EndsWith(tag, "_wool")) return TAG_WOOL;
    if (tag == "coals") return TAG_COALS;
    if (tag == "stone_crafting_materials") return TAG_STONE_CRAFTING;
    if (tag == "coal_ores") return TAG_COAL_ORES;
    if (tag == "iron_ores") return TAG_IRON_ORES;
    if (tag == "gold_ores") return TAG_GOLD_ORES;
    if (tag == "copper_ores") return TAG_COPPER_ORES;
    if (tag == "diamond_ores") return TAG_DIAMOND_ORES;
    if (tag == "redstone_ores") return TAG_REDSTONE_ORES;
    if (tag == "lapis_ores") return TAG_LAPIS_ORES;
    if (tag == "emerald_ores") return TAG_EMERALD_ORES;
    if (tag == "quartz_ores") return TAG_QUARTZ_ORES;
    return TAG_NONE;
}

bool ParseIngredient(const nlohmann::json& value, uint16_t& cell);

bool ParseIngredientName(const std::string& raw, uint16_t& cell) {
    if (raw.empty()) return false;
    if (raw[0] == '#') {
        const uint16_t tag = TagFromName(NormalizeName(raw.substr(1)));
        if (tag == TAG_NONE) return false;
        cell = tag;
        return true;
    }
    const uint8_t id = ItemIdFromResourceName(raw);
    if (id == BLOCK_AIR) return false;
    cell = id;
    return true;
}

bool ParseIngredient(const nlohmann::json& value, uint16_t& cell) {
    if (value.is_string()) return ParseIngredientName(value.get<std::string>(), cell);
    if (value.is_object()) {
        if (value.contains("item") && value["item"].is_string())
            return ParseIngredientName(value["item"].get<std::string>(), cell);
        if (value.contains("id") && value["id"].is_string())
            return ParseIngredientName(value["id"].get<std::string>(), cell);
        if (value.contains("tag") && value["tag"].is_string())
            return ParseIngredientName("#" + value["tag"].get<std::string>(), cell);
        return false;
    }
    if (value.is_array()) {
        for (const auto& alternative : value) {
            if (ParseIngredient(alternative, cell)) return true;
        }
    }
    return false;
}

bool ParseResult(const nlohmann::json& doc, CraftingRecipe& recipe) {
    if (!doc.contains("result") || !doc["result"].is_object()) return false;
    const nlohmann::json& result = doc["result"];
    std::string name;
    if (result.contains("id") && result["id"].is_string()) name = result["id"].get<std::string>();
    else if (result.contains("item") && result["item"].is_string()) name = result["item"].get<std::string>();
    else return false;
    const uint8_t id = ItemIdFromResourceName(name);
    if (id == BLOCK_AIR) return false;
    int count = 1;
    if (result.contains("count") && result["count"].is_number_integer())
        count = result["count"].get<int>();
    if (count < 1 || count > 64) return false;
    recipe.result = {id, count};
    return true;
}

// True when a loot condition only fires with silk touch; the engine has no
// enchantments, so those alternatives are skipped. Java nests the enchantment
// list under match_tool's predicate/predicates, so every shape is walked.
bool HasSilkTouchName(const nlohmann::json& enchantments) {
    if (!enchantments.is_array()) return false;
    for (const auto& ench : enchantments) {
        if (!ench.is_object()) continue;
        std::string name;
        if (ench.contains("enchantment") && ench["enchantment"].is_string())
            name = ench["enchantment"].get<std::string>();
        else if (ench.contains("enchantments") && ench["enchantments"].is_string())
            name = ench["enchantments"].get<std::string>();
        if (name == "minecraft:silk_touch" || name == "silk_touch") return true;
    }
    return false;
}

bool RequiresSilkTouch(const nlohmann::json& conditions) {
    if (conditions.is_array()) {
        for (const auto& condition : conditions)
            if (RequiresSilkTouch(condition)) return true;
        return false;
    }
    if (!conditions.is_object()) return false;
    if (conditions.contains("enchantments") && HasSilkTouchName(conditions["enchantments"]))
        return true;
    if (conditions.contains("minecraft:enchantments") &&
        HasSilkTouchName(conditions["minecraft:enchantments"]))
        return true;
    if (conditions.contains("predicate") && RequiresSilkTouch(conditions["predicate"])) return true;
    if (conditions.contains("predicates") && RequiresSilkTouch(conditions["predicates"])) return true;
    if (conditions.contains("terms") && RequiresSilkTouch(conditions["terms"])) return true;
    return false;
}

bool ParseLootEntry(const nlohmann::json& entry, LootEntry& out) {
    if (!entry.is_object()) return false;
    const std::string type = entry.value("type", "");
    if (type != "minecraft:item") return false;
    const uint8_t id = ItemIdFromResourceName(entry.value("name", ""));
    if (id == BLOCK_AIR) return false;
    int count = 1;
    if (entry.contains("functions") && entry["functions"].is_array()) {
        for (const auto& fn : entry["functions"]) {
            if (!fn.is_object()) continue;
            if (fn.value("function", "") != "minecraft:set_count") continue;
            if (!fn.contains("count")) continue;
            const nlohmann::json& c = fn["count"];
            if (c.is_number_integer()) count = c.get<int>();
            else if (c.is_object()) count = c.value("min", c.value("max", 1));
        }
    }
    out.item = id;
    out.count = std::max(1, std::min(64, count));
    return true;
}

} // namespace

uint8_t ItemIdFromResourceName(const std::string& name) {
    if (name.empty()) return BLOCK_AIR;
    BuildNameMap();
    const auto it = gNameToId.find(NormalizeName(name));
    return it == gNameToId.end() ? uint8_t(BLOCK_AIR) : it->second;
}

std::string ResourceNameForItem(uint8_t id) {
    if (id == BLOCK_GRASS) return "minecraft:grass_block";
    return "minecraft:" + ItemNameSafe(id);
}

bool RecipeCellMatches(uint16_t cell, uint8_t id) {
    if (id == BLOCK_AIR) return false;
    if (!IsTagId(cell)) return uint8_t(cell) == id;
    const std::string name = ItemNameSafe(id);
    switch (cell) {
        case TAG_PLANKS: return EndsWith(name, "planks");
        case TAG_LOGS: return EndsWith(name, "_log") || EndsWith(name, "_stem");
        case TAG_WOOL: return EndsWith(name, "_wool");
        case TAG_COALS: return id == ITEM_COAL;
        case TAG_STONE_CRAFTING:
            return id == BLOCK_COBBLE || id == BLOCK_COBBLED_DEEPSLATE || id == BLOCK_BLACKSTONE;
        case TAG_COAL_ORES: return id == BLOCK_COAL_ORE || id == BLOCK_DEEPSLATE_COAL_ORE;
        case TAG_IRON_ORES:
            return id == BLOCK_IRON_ORE || id == BLOCK_DEEPSLATE_IRON_ORE || id == ITEM_RAW_IRON;
        case TAG_GOLD_ORES:
            return id == BLOCK_GOLD_ORE || id == BLOCK_DEEPSLATE_GOLD_ORE ||
                   id == BLOCK_NETHER_GOLD_ORE || id == ITEM_RAW_GOLD;
        case TAG_COPPER_ORES:
            return id == BLOCK_COPPER_ORE || id == BLOCK_DEEPSLATE_COPPER_ORE || id == ITEM_RAW_COPPER;
        case TAG_DIAMOND_ORES:
            return id == BLOCK_DIAMOND_ORE || id == BLOCK_DEEPSLATE_DIAMOND_ORE;
        case TAG_REDSTONE_ORES:
            return id == BLOCK_REDSTONE_ORE || id == BLOCK_DEEPSLATE_REDSTONE_ORE;
        case TAG_LAPIS_ORES:
            return id == BLOCK_LAPIS_ORE || id == BLOCK_DEEPSLATE_LAPIS_ORE;
        case TAG_EMERALD_ORES:
            return id == BLOCK_EMERALD_ORE || id == BLOCK_DEEPSLATE_EMERALD_ORE;
        case TAG_QUARTZ_ORES: return id == BLOCK_NETHER_QUARTZ_ORE;
        default: return false;
    }
}

bool ParseRecipeJson(const std::string& text, CraftingRecipe& out) {
    const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return false;
    const std::string type = doc.value("type", "");
    if (type == "minecraft:crafting_shaped") {
        if (!doc.contains("pattern") || !doc["pattern"].is_array()) return false;
        const nlohmann::json& pattern = doc["pattern"];
        if (pattern.empty() || pattern.size() > 3) return false;
        int height = int(pattern.size()), width = 0;
        for (const auto& row : pattern) {
            if (!row.is_string()) return false;
            width = std::max(width, int(row.get_ref<const std::string&>().size()));
        }
        if (width <= 0 || width > 3) return false;
        if (!doc.contains("key") || !doc["key"].is_object()) return false;
        const nlohmann::json& key = doc["key"];
        CraftingRecipe recipe;
        recipe.width = width;
        recipe.height = height;
        for (int r = 0; r < height; ++r) {
            const std::string row = pattern[r].get<std::string>();
            for (int c = 0; c < width; ++c) {
                const char symbol = c < int(row.size()) ? row[c] : ' ';
                if (symbol == ' ') continue;
                const std::string k(1, symbol);
                uint16_t cell = 0;
                if (!key.contains(k) || !ParseIngredient(key.at(k), cell)) return false;
                recipe.cells[r * 3 + c] = cell;
            }
        }
        if (!ParseResult(doc, recipe)) return false;
        out = recipe;
        return true;
    }
    if (type == "minecraft:crafting_shapeless") {
        if (!doc.contains("ingredients") || !doc["ingredients"].is_array()) return false;
        const nlohmann::json& ingredients = doc["ingredients"];
        if (ingredients.empty() || ingredients.size() > 9) return false;
        CraftingRecipe recipe;
        recipe.shapeless = true;
        for (size_t i = 0; i < ingredients.size(); ++i) {
            uint16_t cell = 0;
            if (!ParseIngredient(ingredients[i], cell)) return false;
            recipe.cells[i] = cell;
        }
        if (!ParseResult(doc, recipe)) return false;
        out = recipe;
        return true;
    }
    return false;
}

bool ParseLootTableJson(const std::string& text, LootEntry& out, bool& hasItem) {
    hasItem = false;
    const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return false;
    if (!doc.contains("pools") || !doc["pools"].is_array()) return true;
    for (const auto& pool : doc["pools"]) {
        if (!pool.is_object() || !pool.contains("entries") || !pool["entries"].is_array()) continue;
        if (pool.contains("conditions") && RequiresSilkTouch(pool["conditions"])) continue;
        for (const auto& entry : pool["entries"]) {
            if (!entry.is_object()) continue;
            if (entry.contains("conditions") && RequiresSilkTouch(entry["conditions"])) continue;
            LootEntry parsed;
            if (!ParseLootEntry(entry, parsed)) continue;
            out = parsed;
            hasItem = true;
            return true;
        }
    }
    return true;
}

bool LoadRegistry(AssetStore& store) {
    gStats = {};
    gRecipes.clear();
    gLoot.clear();
    BuildNameMap();
    if (!store.HasJar()) return false;
    gStats.jarLoaded = true;

    // Name coverage: how much of Java's block/item namespace this palette maps.
    const char kBlockstates[] = "assets/minecraft/blockstates/";
    for (const std::string& path : store.ListJarPrefix(kBlockstates)) {
        if (!EndsWith(path, ".json")) continue;
        const std::string name = path.substr(strlen(kBlockstates));
        if (ItemIdFromResourceName(name.substr(0, name.size() - 5)) != BLOCK_AIR) ++gStats.mappedBlockNames;
    }
    const char kItemModels[] = "assets/minecraft/models/item/";
    for (const std::string& path : store.ListJarPrefix(kItemModels)) {
        if (!EndsWith(path, ".json")) continue;
        const std::string name = path.substr(strlen(kItemModels));
        if (ItemIdFromResourceName(name.substr(0, name.size() - 5)) != BLOCK_AIR) ++gStats.mappedItemNames;
    }

    std::vector<std::string> recipePaths = store.ListJarPrefix("data/minecraft/recipe/");
    if (recipePaths.empty()) recipePaths = store.ListJarPrefix("data/minecraft/recipes/");
    std::vector<uint8_t> bytes;
    for (const std::string& path : recipePaths) {
        if (!EndsWith(path, ".json")) continue;
        if (!store.ReadJarEntry(path, bytes) || bytes.empty()) continue;
        CraftingRecipe recipe;
        if (ParseRecipeJson(std::string(bytes.begin(), bytes.end()), recipe)) gRecipes.push_back(recipe);
    }
    gStats.recipes = int(gRecipes.size());
    gStats.recipesFromJar = !gRecipes.empty();

    std::vector<std::string> lootPaths = store.ListJarPrefix("data/minecraft/loot_table/blocks/");
    if (lootPaths.empty()) lootPaths = store.ListJarPrefix("data/minecraft/loot_tables/blocks/");
    for (const std::string& path : lootPaths) {
        if (!EndsWith(path, ".json")) continue;
        const size_t slash = path.find_last_of('/');
        const std::string base = path.substr(slash == std::string::npos ? 0 : slash + 1);
        const uint8_t block = ItemIdFromResourceName(base.substr(0, base.size() - 5));
        if (block == BLOCK_AIR || block >= BLOCK_COUNT) continue;
        if (!store.ReadJarEntry(path, bytes) || bytes.empty()) continue;
        LootEntry entry;
        bool hasItem = false;
        if (!ParseLootTableJson(std::string(bytes.begin(), bytes.end()), entry, hasItem)) continue;
        ++gStats.lootTables;
        gLoot[block] = hasItem ? entry : LootEntry{};
    }
    gStats.lootFromJar = !gLoot.empty();
    return gStats.recipesFromJar || gStats.lootFromJar;
}

const RegistryStats& RegistryInfo() { return gStats; }

const std::vector<CraftingRecipe>& RegistryRecipes() { return gRecipes; }

bool LootDropForBlock(uint8_t block, LootEntry& out) {
    const auto it = gLoot.find(block);
    if (it == gLoot.end()) return false;
    out = it->second;
    return true;
}

int ItemCreativeTab(uint8_t id) {
    if (id == BLOCK_AIR || !IsValidItem(id)) return -1;
    if (const ToolDef* tool = FindTool(id))
        return tool->kind == ToolKind::Axe || tool->kind == ToolKind::Sword ? CTAB_COMBAT : CTAB_TOOLS;
    if (FindArmor(id) || id == ITEM_ELYTRA) return CTAB_COMBAT;
    if (id == ITEM_FLINT_AND_STEEL || id == ITEM_FIREWORK_ROCKET) return CTAB_TOOLS;
    if (id >= ITEM_COAL && id <= ITEM_RAW_COPPER) return CTAB_INGREDIENTS;
    if (id == ITEM_STICK || id == ITEM_QUARTZ || id == ITEM_COPPER_INGOT) return CTAB_INGREDIENTS;
    const std::string name = ItemNameSafe(id);
    if (EndsWith(name, "_wool") || id == BLOCK_TERRACOTTA) return CTAB_COLORED;
    switch (id) {
        case BLOCK_TNT: case BLOCK_REDSTONE_BLOCK: return CTAB_REDSTONE;
        case BLOCK_CRAFTING_TABLE: case BLOCK_FURNACE: case BLOCK_CHEST:
        case BLOCK_BOOKSHELF: case BLOCK_MELON: case BLOCK_HAY_BLOCK:
        case BLOCK_BONE_BLOCK: case BLOCK_SEA_LANTERN: case BLOCK_SHROOMLIGHT:
            return CTAB_FUNCTIONAL;
        default: break;
    }
    if (EndsWith(name, "_ore") || EndsWith(name, "_leaves") ||
        id == BLOCK_DIRT || id == BLOCK_GRASS || id == BLOCK_SAND || id == BLOCK_RED_SAND ||
        id == BLOCK_GRAVEL || id == BLOCK_CLAY || id == BLOCK_SNOW || id == BLOCK_ICE ||
        id == BLOCK_MOSS_BLOCK || id == BLOCK_DEEPSLATE || id == BLOCK_NETHERRACK ||
        id == BLOCK_SOUL_SAND || id == BLOCK_SOUL_SOIL || id == BLOCK_BASALT ||
        id == BLOCK_BLACKSTONE || id == BLOCK_MAGMA || id == BLOCK_GLOWSTONE ||
        id == BLOCK_ANCIENT_DEBRIS || id == BLOCK_AMETHYST_BLOCK || id == BLOCK_DRIPSTONE_BLOCK ||
        id == BLOCK_NETHER_WART_BLOCK || id == BLOCK_WARPED_WART_BLOCK ||
        id == BLOCK_CRIMSON_STEM || id == BLOCK_WARPED_STEM ||
        id == BLOCK_WATER || id == BLOCK_LAVA)
        return CTAB_NATURAL;
    return CTAB_BUILDING;
}

const char* CreativeTabName(int tab) {
    switch (tab) {
        case CTAB_BUILDING: return "Building Blocks";
        case CTAB_COLORED: return "Colored Blocks";
        case CTAB_NATURAL: return "Natural Blocks";
        case CTAB_FUNCTIONAL: return "Functional Blocks";
        case CTAB_REDSTONE: return "Redstone";
        case CTAB_TOOLS: return "Tools & Utilities";
        case CTAB_COMBAT: return "Combat";
        case CTAB_FOOD: return "Food & Drinks";
        case CTAB_INGREDIENTS: return "Ingredients";
        case CTAB_SEARCH: return "Search Items";
        default: return "";
    }
}

uint8_t CreativeTabIcon(int tab) {
    switch (tab) {
        case CTAB_BUILDING: return BLOCK_BRICK;
        case CTAB_COLORED: return BLOCK_RED_WOOL;
        case CTAB_NATURAL: return BLOCK_GRASS;
        case CTAB_FUNCTIONAL: return BLOCK_CRAFTING_TABLE;
        case CTAB_REDSTONE: return BLOCK_REDSTONE_BLOCK;
        case CTAB_TOOLS: return ITEM_IRON_PICKAXE;
        case CTAB_COMBAT: return ITEM_IRON_SWORD;
        case CTAB_FOOD: return BLOCK_MELON;
        case CTAB_INGREDIENTS: return ITEM_IRON_INGOT;
        case CTAB_SEARCH: return ITEM_QUARTZ;
        default: return BLOCK_AIR;
    }
}

bool ItemMatchesSearch(uint8_t id, const std::string& query) {
    if (query.empty()) return true;
    std::string needle = query;
    for (char& c : needle) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (c == ' ') c = '_';
    }
    std::string haystack = ItemNameSafe(id);
    for (char& c : haystack) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return haystack.find(needle) != std::string::npos;
}

} // namespace potato
