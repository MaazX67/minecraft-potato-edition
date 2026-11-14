#include "world.h"
#include "assets.h"
#include <algorithm>
#include <cctype>
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

PerlinNoise::PerlinNoise(uint64_t seed) {
    uint8_t base[256];
    std::iota(base, base + 256, 0);
    std::mt19937_64 rng(MixSeed64(seed));
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

const char* GeneratorName(WorldGenerator generator) {
    switch (generator) {
        case WorldGenerator::Legacy: return "potato-perlin-v1";
        case WorldGenerator::Climate: return "potato-climate-v2";
        case WorldGenerator::Flat: return "potato-flat-v1";
        case WorldGenerator::Nether: return "potato-nether-v1";
        case WorldGenerator::LargeBiomes: return "potato-large-biomes-v1";
        case WorldGenerator::Amplified: return "potato-amplified-v1";
    }
    return "unknown";
}

bool ParseGenerator(const std::string& name, WorldGenerator& generator) {
    if (name == "potato-perlin-v1" || name == "legacy") generator = WorldGenerator::Legacy;
    else if (name == "potato-climate-v2" || name == "climate") generator = WorldGenerator::Climate;
    else if (name == "potato-flat-v1" || name == "flat") generator = WorldGenerator::Flat;
    else if (name == "potato-nether-v1" || name == "nether") generator = WorldGenerator::Nether;
    else if (name == "potato-large-biomes-v1" || name == "large_biomes") generator = WorldGenerator::LargeBiomes;
    else if (name == "potato-amplified-v1" || name == "amplified") generator = WorldGenerator::Amplified;
    else return false;
    return true;
}

const char* BiomeName(Biome biome) {
    switch (biome) {
        case Biome::Plains: return "Plains";
        case Biome::Forest: return "Forest";
        case Biome::BirchForest: return "Birch Forest";
        case Biome::Taiga: return "Taiga";
        case Biome::Desert: return "Desert";
        case Biome::Snowy: return "Snowy";
        case Biome::Mountains: return "Mountains";
        case Biome::River: return "River";
        case Biome::Ocean: return "Ocean";
        case Biome::CherryGrove: return "Cherry Grove";
    }
    return "Unknown";
}

namespace {

constexpr int CLIMATE_SEA = 63;

float SmoothRange(float lo, float hi, float value) {
    const float t = std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

int FloorCell8(int value) {
    return value / 8 - (value % 8 < 0 ? 1 : 0);
}

struct ClimateVein {
    uint8_t block = BLOCK_STONE;
    int maxY = 0;
    float lateral = 0, centerY = 0, invRadiusY = 1;
};

ClimateVein SampleClimateVein(int x, int cellY, int z, uint64_t seed) {
    const int cellX = FloorCell8(x), cellZ = FloorCell8(z);
    const uint32_t hash = Hash3(uint32_t(cellX), uint32_t(cellY), uint32_t(cellZ),
                                SeedHash32(seed) ^ 0xA341316Cu);
    const uint32_t roll = hash % 100;
    const int bottom = cellY * 8;
    ClimateVein vein;
    if (bottom < 16 && roll < 3) { vein.block = BLOCK_DIAMOND_ORE; vein.maxY = 16; }
    else if (bottom < 24 && roll < 7) { vein.block = BLOCK_REDSTONE_ORE; vein.maxY = 24; }
    else if (bottom < 32 && roll < 11) { vein.block = BLOCK_LAPIS_ORE; vein.maxY = 32; }
    else if (bottom < 40 && roll < 15) { vein.block = BLOCK_GOLD_ORE; vein.maxY = 40; }
    else if (bottom < 80 && roll < 31) { vein.block = BLOCK_IRON_ORE; vein.maxY = 80; }
    else if (bottom < 144 && roll < 53) { vein.block = BLOCK_COAL_ORE; vein.maxY = 144; }
    const float dx = float(x - cellX * 8 - 2 - int((hash >> 8) & 3u));
    const float dz = float(z - cellZ * 8 - 2 - int((hash >> 10) & 3u));
    const float rx = 1.8f + float((hash >> 16) & 3u) * 0.35f;
    const float rz = 1.8f + float((hash >> 18) & 3u) * 0.35f;
    vein.lateral = dx * dx / (rx * rx) + dz * dz / (rz * rz);
    vein.centerY = float(bottom + 2 + int((hash >> 12) & 3u));
    vein.invRadiusY = 1.0f / (1.5f + float((hash >> 20) & 3u) * 0.35f);
    return vein;
}

bool IsTreeLeaf(uint8_t block) {
    return block == BLOCK_OAK_LEAVES || block == BLOCK_BIRCH_LEAVES || block == BLOCK_SPRUCE_LEAVES ||
           block == BLOCK_CHERRY_LEAVES;
}

uint8_t DeepslateOre(uint8_t ore) {
    switch (ore) {
        case BLOCK_COAL_ORE: return BLOCK_DEEPSLATE_COAL_ORE;
        case BLOCK_IRON_ORE: return BLOCK_DEEPSLATE_IRON_ORE;
        case BLOCK_GOLD_ORE: return BLOCK_DEEPSLATE_GOLD_ORE;
        case BLOCK_DIAMOND_ORE: return BLOCK_DEEPSLATE_DIAMOND_ORE;
        case BLOCK_REDSTONE_ORE: return BLOCK_DEEPSLATE_REDSTONE_ORE;
        case BLOCK_LAPIS_ORE: return BLOCK_DEEPSLATE_LAPIS_ORE;
        default: return ore;
    }
}

} // namespace

TerrainColumn SampleTerrain(const PerlinNoise& noise, int x, int z,
                            bool largeBiomes, bool amplified) {
    const float freq = largeBiomes ? 0.35f : 1.0f;
    const float wx = float(x) * freq, wz = float(z) * freq;
    const float continental = noise.Fbm2(wx * 0.0012f + 37.1f, wz * 0.0012f - 19.4f, 3);
    const float erosion = noise.Fbm2(wx * 0.0027f - 83.2f, wz * 0.0027f + 41.8f, 3);
    const float ridge = 1.0f - std::clamp(fabsf(noise.Fbm2(wx * 0.007f + 53.8f, wz * 0.007f - 31.4f, 2)), 0.0f, 1.0f);
    const float inland = SmoothRange(-0.12f, 0.20f, continental);
    const float mountains = 105.0f * ridge * ridge * ridge
        * (1.0f - SmoothRange(-0.38f, 0.20f, erosion)) * SmoothRange(-0.08f, 0.18f, continental);
    const float hills = noise.Fbm2(wx * 0.012f - 15.3f, wz * 0.012f + 67.9f, 2);
    float elevation = float(CLIMATE_SEA) + continental * 95.0f + inland * (8.0f + hills * 13.0f) + mountains;
    if (amplified) elevation = float(CLIMATE_SEA) + (elevation - float(CLIMATE_SEA)) * 1.8f +
        fabsf(mountains) * 0.6f;
    const float riverNoise = noise.Noise2(wx * 0.004f + 91.7f + erosion * 0.8f,
                                         wz * 0.004f - 72.5f + continental * 0.6f);
    const float river = 1.0f - SmoothRange(0.016f, 0.085f, fabsf(riverNoise));
    const float riverBed = float(CLIMATE_SEA - 5) + hills * 1.5f;
    elevation = LerpN(river, elevation, std::min(elevation, riverBed));
    const int height = int(floorf(std::clamp(elevation, 5.0f, 210.0f)));
    const float temperature = std::clamp(0.5f + 0.7f * noise.Fbm2(wx * 0.0016f - 137.6f, wz * 0.0016f + 89.2f, 3)
        - float(std::max(0, height - 85)) * 0.0045f, 0.0f, 1.0f);
    const float humidity = std::clamp(0.5f + 0.7f * noise.Fbm2(wx * 0.0018f + 173.3f, wz * 0.0018f - 121.7f, 3), 0.0f, 1.0f);
    Biome biome = Biome::Plains;
    if (river > 0.82f && continental > -0.12f && height <= CLIMATE_SEA + 1) biome = Biome::River;
    else if (height < CLIMATE_SEA) biome = Biome::Ocean;
    else if (height >= 118 || (mountains > 27.0f && height > 95)) biome = Biome::Mountains;
    else if (temperature < 0.24f) biome = Biome::Snowy;
    else if (temperature < 0.40f && humidity > 0.42f) biome = Biome::Taiga;
    else if (temperature > 0.64f && humidity < 0.40f) biome = Biome::Desert;
    else if (temperature >= 0.45f && temperature < 0.66f && humidity > 0.60f &&
             noise.Fbm2(wx * 0.005f + 311.7f, wz * 0.005f - 208.3f, 2) > 0.30f) biome = Biome::CherryGrove;
    else if (temperature >= 0.43f && temperature < 0.67f && humidity > 0.62f) biome = Biome::BirchForest;
    else if (humidity > 0.49f) biome = Biome::Forest;
    return {height, biome, temperature, humidity};
}

// ---------------------------------------------------------------------------
// Seeded structures: one candidate per region cell, chosen and jittered from
// the 64-bit world seed so a seed alone defines every structure in the world.
// ---------------------------------------------------------------------------

namespace {

constexpr int kOverworldStructureCell = 512;
constexpr int kNetherStructureCell = 256;

int FloorDivGrid(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

uint64_t StructureCellHash(uint64_t seed, int cellX, int cellZ, bool nether) {
    return MixSeed64(seed ^ (uint64_t(uint32_t(cellX)) * 0x9E3779B97F4A7C15ull)
                          ^ (uint64_t(uint32_t(cellZ)) * 0xC2B2AE3D27D4EB4Full)
                          ^ (nether ? 0x4E45544845524Bull : 0x4F56455257ull));
}

struct StructureCandidate {
    bool present = false;
    StructureType type = StructureType::Village;
    int x = 0;
    int z = 0;
};

StructureCandidate SampleStructureCell(uint64_t seed, int cellX, int cellZ, bool nether) {
    StructureCandidate out;
    const uint64_t h = StructureCellHash(seed, cellX, cellZ, nether);
    const uint32_t roll = uint32_t(h % 1000u);
    if (nether) {
        if (roll < 90) out.type = StructureType::NetherFortress;
        else if (roll < 150) out.type = StructureType::Bastion;
        else return out;
    } else if (roll < 55) {
        out.type = StructureType::Village;
    } else if (roll < 80) {
        out.type = StructureType::PillagerOutpost;
    } else if (roll < 125) {
        out.type = StructureType::RuinedPortal;
    } else if (roll < 155) {
        out.type = StructureType::DesertPyramid;
    } else if (roll < 175) {
        out.type = StructureType::JungleTemple;
    } else if (roll < 395) {
        out.type = StructureType::Mineshaft;
    } else if (roll < 455) {
        out.type = StructureType::Stronghold;
    } else {
        return out;
    }
    out.present = true;
    const int cell = nether ? kNetherStructureCell : kOverworldStructureCell;
    const int span = cell - 96;
    out.x = cellX * cell + 48 + int((h >> 16) % uint64_t(span));
    out.z = cellZ * cell + 48 + int((h >> 34) % uint64_t(span));
    return out;
}

struct StructureStamper {
    uint8_t* blocks;
    int originX, originZ;
    void Put(int wx, int y, int wz, uint8_t block) const {
        const int x = wx - originX, z = wz - originZ;
        if (x < 0 || x >= CHUNK_X || z < 0 || z >= CHUNK_Z || y < 1 || y >= CHUNK_Y) return;
        blocks[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y] = block;
    }
    void Box(int x0, int y0, int z0, int x1, int y1, int z1, uint8_t block) const {
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x) Put(x, y, z, block);
    }
};

void VillageHut(const StructureStamper& s, int x, int gy, int z) {
    s.Box(x - 2, gy, z - 2, x + 2, gy, z + 2, BLOCK_COBBLE);
    for (int dy = 1; dy <= 3; ++dy)
        for (int d = -2; d <= 2; ++d) {
            const uint8_t wall = (d == -2 || d == 2) ? BLOCK_LOG : BLOCK_PLANKS;
            s.Put(x + d, gy + dy, z - 2, wall);
            s.Put(x + d, gy + dy, z + 2, wall);
            s.Put(x - 2, gy + dy, z + d, wall);
            s.Put(x + 2, gy + dy, z + d, wall);
        }
    s.Box(x - 2, gy + 4, z - 2, x + 2, gy + 4, z + 2, BLOCK_PLANKS);
    s.Put(x, gy + 1, z - 2, BLOCK_AIR);
    s.Put(x, gy + 2, z - 2, BLOCK_AIR);
    s.Put(x - 1, gy + 2, z + 2, BLOCK_GLASS);
    s.Put(x + 1, gy + 2, z + 2, BLOCK_GLASS);
    s.Put(x + 1, gy + 1, z + 1, BLOCK_CHEST);
}

void BuildVillage(const StructureStamper& s, int x, int gy, int z) {
    s.Box(x - 6, gy, z - 6, x + 6, gy, z + 6, BLOCK_COBBLE);
    s.Box(x - 1, gy + 1, z - 1, x + 1, gy + 1, z + 1, BLOCK_COBBLE);
    s.Put(x, gy + 1, z, BLOCK_WATER);
    for (int dx = -1; dx <= 1; dx += 2)
        for (int dz = -1; dz <= 1; dz += 2) s.Put(x + dx, gy + 2, z + dz, BLOCK_LOG);
    s.Box(x - 2, gy + 3, z - 2, x + 2, gy + 3, z + 2, BLOCK_PLANKS);
    VillageHut(s, x - 10, gy, z);
    VillageHut(s, x + 10, gy, z);
    VillageHut(s, x, gy, z - 10);
    VillageHut(s, x, gy, z + 10);
}

void BuildDesertPyramid(const StructureStamper& s, int x, int gy, int z) {
    for (int step = 0; step <= 6; ++step) {
        const int r = 7 - step;
        const int y = gy + step;
        s.Box(x - r, y, z - r, x + r, y, z + r, BLOCK_SANDSTONE);
        if (step == 2)
            for (int d = -r + 1; d <= r - 1; ++d) {
                s.Put(x + d, y, z - r, BLOCK_TERRACOTTA);
                s.Put(x + d, y, z + r, BLOCK_TERRACOTTA);
                s.Put(x - r, y, z + d, BLOCK_TERRACOTTA);
                s.Put(x + r, y, z + d, BLOCK_TERRACOTTA);
            }
    }
    s.Put(x, gy + 7, z, BLOCK_TERRACOTTA);
    // Entrance tunnel on the south face plus the treasure room.
    s.Box(x - 1, gy + 1, z + 1, x + 1, gy + 2, z + 7, BLOCK_AIR);
    s.Box(x - 5, gy + 1, z - 5, x + 5, gy + 3, z + 5, BLOCK_AIR);
    s.Put(x - 4, gy + 1, z - 4, BLOCK_CHEST);
    s.Put(x + 4, gy + 1, z - 4, BLOCK_CHEST);
    s.Put(x, gy + 1, z, BLOCK_TERRACOTTA);
}

void BuildJungleTemple(const StructureStamper& s, int x, int gy, int z) {
    for (int step = 0; step <= 2; ++step) {
        const int r = 5 - step * 2;
        s.Box(x - r, gy + step * 2, z - r, x + r, gy + step * 2 + 1, z + r, BLOCK_MOSSY_COBBLE);
    }
    s.Put(x, gy + 6, z, BLOCK_CHEST);
    s.Box(x - 1, gy + 1, z + 4, x + 1, gy + 2, z + 5, BLOCK_AIR);
    s.Put(x - 2, gy + 1, z - 2, BLOCK_CRAFTING_TABLE);
    s.Put(x + 2, gy + 3, z + 2, BLOCK_MOSSY_COBBLE);
}

void BuildRuinedPortal(const StructureStamper& s, int x, int gy, int z) {
    s.Box(x - 3, gy, z - 2, x + 3, gy, z + 2, BLOCK_NETHERRACK);
    s.Box(x - 2, gy + 1, z - 2, x + 2, gy + 1, z + 2, BLOCK_NETHERRACK);
    for (int dy = 1; dy <= 5; ++dy) {
        s.Put(x - 2, gy + dy, z, BLOCK_OBSIDIAN);
        s.Put(x + 2, gy + dy, z, BLOCK_OBSIDIAN);
    }
    for (int dx = -1; dx <= 1; ++dx) s.Put(x + dx, gy + 6, z, BLOCK_OBSIDIAN);
    s.Put(x + 2, gy + 3, z, BLOCK_AIR);
    s.Put(x - 2, gy + 2, z, BLOCK_CRYING_OBSIDIAN);
    s.Put(x - 3, gy + 1, z - 1, BLOCK_MAGMA);
    s.Put(x + 3, gy + 1, z + 1, BLOCK_CHEST);
}

void BuildPillagerOutpost(const StructureStamper& s, int x, int gy, int z) {
    s.Box(x - 5, gy, z - 5, x + 5, gy, z + 5, BLOCK_COBBLE);
    for (int dx = -3; dx <= 3; dx += 3)
        for (int dz = -3; dz <= 3; dz += 3)
            for (int dy = 1; dy <= 4; ++dy) s.Put(x + dx, gy + dy, z + dz, BLOCK_LOG);
    s.Box(x - 4, gy + 5, z - 4, x + 4, gy + 5, z + 4, BLOCK_PLANKS);
    for (int dy = 6; dy <= 7; ++dy)
        for (int d = -3; d <= 3; ++d) {
            s.Put(x + d, gy + dy, z - 3, BLOCK_PLANKS);
            s.Put(x + d, gy + dy, z + 3, BLOCK_PLANKS);
            s.Put(x - 3, gy + dy, z + d, BLOCK_PLANKS);
            s.Put(x + 3, gy + dy, z + d, BLOCK_PLANKS);
        }
    s.Put(x, gy + 6, z - 3, BLOCK_AIR);
    s.Put(x, gy + 7, z - 3, BLOCK_AIR);
    s.Box(x - 4, gy + 8, z - 4, x + 4, gy + 8, z + 4, BLOCK_PLANKS);
    s.Put(x + 2, gy + 6, z + 2, BLOCK_CHEST);
}

void BuildMineshaft(const StructureStamper& s, int x, int y, int z) {
    s.Box(x - 10, y, z - 1, x + 10, y, z + 1, BLOCK_PLANKS);
    s.Box(x - 10, y + 1, z - 1, x + 10, y + 3, z + 1, BLOCK_AIR);
    for (int d = -8; d <= 8; d += 4)
        for (int dz = -1; dz <= 1; dz += 2)
            for (int dy = 1; dy <= 3; ++dy) s.Put(x + d, y + dy, z + dz, BLOCK_LOG);
    s.Box(x - 10, y + 4, z - 2, x + 10, y + 4, z + 2, BLOCK_PLANKS);
    s.Box(x - 1, y, z - 8, x + 1, y, z + 8, BLOCK_PLANKS);
    s.Box(x - 1, y + 1, z - 8, x + 1, y + 3, z + 8, BLOCK_AIR);
    s.Put(x + 6, y + 1, z + 2, BLOCK_CHEST);
    s.Put(x - 7, y, z + 1, BLOCK_COBBLE);
}

void BuildStronghold(const StructureStamper& s, int x, int y, int z) {
    s.Box(x - 6, y, z - 6, x + 6, y + 5, z + 6, BLOCK_STONE_BRICKS);
    s.Box(x - 5, y + 1, z - 5, x + 5, y + 4, z + 5, BLOCK_AIR);
    s.Put(x - 6, y + 3, z - 6, BLOCK_MOSSY_STONE_BRICKS);
    s.Put(x + 6, y + 2, z + 3, BLOCK_MOSSY_STONE_BRICKS);
    s.Put(x - 3, y + 5, z + 1, BLOCK_MOSSY_STONE_BRICKS);
    s.Box(x + 6, y + 1, z - 1, x + 6, y + 3, z + 1, BLOCK_AIR);
    s.Box(x + 7, y, z - 1, x + 10, y + 3, z + 1, BLOCK_STONE_BRICKS);
    s.Box(x + 7, y + 1, z - 1, x + 10, y + 2, z + 1, BLOCK_AIR);
    s.Put(x + 4, y + 1, z + 4, BLOCK_CHEST);
    s.Box(x - 1, y + 1, z - 1, x + 1, y + 4, z + 1, BLOCK_STONE_BRICKS);
}

void BuildNetherFortress(const StructureStamper& s, int x, int y, int z) {
    s.Box(x - 12, y - 1, z - 2, x + 12, y - 1, z + 2, BLOCK_NETHER_BRICKS);
    for (int dx = -12; dx <= 12; ++dx)
        for (int dz = -2; dz <= 2; dz += 4) {
            s.Put(x + dx, y, z + dz, BLOCK_NETHER_BRICKS);
            s.Put(x + dx, y + 1, z + dz, BLOCK_NETHER_BRICKS);
        }
    for (int dx = -12; dx <= 12; dx += 6)
        for (int dz = -2; dz <= 2; dz += 4)
            for (int dy = 1; dy <= 4; ++dy) s.Put(x + dx, y + dy, z + dz, BLOCK_NETHER_BRICKS);
    s.Box(x - 12, y + 5, z - 2, x + 12, y + 5, z + 2, BLOCK_NETHER_BRICKS);
    s.Put(x + 9, y + 1, z + 1, BLOCK_CHEST);
}

void BuildBastion(const StructureStamper& s, int x, int y, int z) {
    s.Box(x - 5, y, z - 5, x + 5, y, z + 5, BLOCK_BLACKSTONE);
    for (int dy = 1; dy <= 6; ++dy)
        for (int d = -5; d <= 5; ++d) {
            const uint8_t wall = (d == -5 || d == 5) ? BLOCK_POLISHED_BLACKSTONE : BLOCK_BLACKSTONE;
            s.Put(x + d, y + dy, z - 5, wall);
            s.Put(x + d, y + dy, z + 5, wall);
            s.Put(x - 5, y + dy, z + d, wall);
            s.Put(x + 5, y + dy, z + d, wall);
        }
    s.Box(x - 6, y + 7, z - 6, x + 6, y + 7, z + 6, BLOCK_POLISHED_BLACKSTONE);
    s.Box(x - 1, y + 1, z + 5, x + 1, y + 3, z + 5, BLOCK_AIR);
    s.Put(x, y + 2, z + 3, BLOCK_GOLD_BLOCK);
    s.Put(x - 3, y + 1, z - 3, BLOCK_CHEST);
}

int StructureGroundY(const PerlinNoise& noise, const StructureCandidate& c) {
    return SampleTerrain(noise, c.x, c.z).height;
}

} // namespace

const char* StructureId(StructureType type) {
    switch (type) {
        case StructureType::Village: return "village";
        case StructureType::DesertPyramid: return "desert_pyramid";
        case StructureType::JungleTemple: return "jungle_temple";
        case StructureType::RuinedPortal: return "ruined_portal";
        case StructureType::PillagerOutpost: return "pillager_outpost";
        case StructureType::Mineshaft: return "mineshaft";
        case StructureType::Stronghold: return "stronghold";
        case StructureType::NetherFortress: return "nether_fortress";
        case StructureType::Bastion: return "bastion";
        default: return "";
    }
}

const char* StructureName(StructureType type) {
    switch (type) {
        case StructureType::Village: return "Village";
        case StructureType::DesertPyramid: return "Desert Pyramid";
        case StructureType::JungleTemple: return "Jungle Temple";
        case StructureType::RuinedPortal: return "Ruined Portal";
        case StructureType::PillagerOutpost: return "Pillager Outpost";
        case StructureType::Mineshaft: return "Mineshaft";
        case StructureType::Stronghold: return "Stronghold";
        case StructureType::NetherFortress: return "Nether Fortress";
        case StructureType::Bastion: return "Bastion Remnant";
        default: return "";
    }
}

bool IsNetherStructure(StructureType type) {
    return type == StructureType::NetherFortress || type == StructureType::Bastion;
}

bool ParseStructure(const std::string& name, StructureType& type) {
    std::string key;
    for (char ch : name) {
        if (ch == ' ' || ch == '-') key.push_back('_');
        else key.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
    }
    const std::string prefix = "minecraft:";
    if (key.compare(0, prefix.size(), prefix) == 0) key.erase(0, prefix.size());
    if (key == "desert_temple" || key == "pyramid") key = "desert_pyramid";
    if (key == "jungle_pyramid") key = "jungle_temple";
    if (key == "fortress") key = "nether_fortress";
    if (key == "bastion_remnant") key = "bastion";
    for (int i = 0; i < int(StructureType::Count); ++i) {
        const StructureType candidate = StructureType(i);
        if (key == StructureId(candidate)) { type = candidate; return true; }
    }
    return false;
}

bool FindStructure(uint64_t seed, int fromX, int fromZ, StructureType type,
                   int& outX, int& outZ, int& distance) {
    if (type == StructureType::Count) return false;
    const bool nether = IsNetherStructure(type);
    const int cell = nether ? kNetherStructureCell : kOverworldStructureCell;
    const int baseCX = FloorDivGrid(fromX, cell);
    const int baseCZ = FloorDivGrid(fromZ, cell);
    int64_t bestSq = -1;
    int bestX = 0, bestZ = 0;
    for (int ring = 0; ring <= 32; ++ring) {
        for (int gx = baseCX - ring; gx <= baseCX + ring; ++gx) {
            for (int gz = baseCZ - ring; gz <= baseCZ + ring; ++gz) {
                if (ring > 0 && abs(gx - baseCX) != ring && abs(gz - baseCZ) != ring) continue;
                const StructureCandidate c = SampleStructureCell(seed, gx, gz, nether);
                if (!c.present || c.type != type) continue;
                const int64_t dx = int64_t(c.x) - fromX;
                const int64_t dz = int64_t(c.z) - fromZ;
                const int64_t sq = dx * dx + dz * dz;
                if (bestSq < 0 || sq < bestSq) {
                    bestSq = sq;
                    bestX = c.x;
                    bestZ = c.z;
                }
            }
        }
        if (bestSq >= 0 && double(ring) * double(cell) > std::sqrt(double(bestSq)) + double(cell)) break;
    }
    if (bestSq < 0) return false;
    outX = bestX;
    outZ = bestZ;
    distance = int(std::llround(std::sqrt(double(bestSq))));
    return true;
}

void ApplyStructures(uint8_t* blocks, const PerlinNoise& noise, uint64_t seed,
                     int cx, int cz, bool nether) {
    const int cell = nether ? kNetherStructureCell : kOverworldStructureCell;
    const int originX = cx * CHUNK_X;
    const int originZ = cz * CHUNK_Z;
    const int minCX = FloorDivGrid(originX - 16, cell);
    const int maxCX = FloorDivGrid(originX + CHUNK_X + 16, cell);
    const int minCZ = FloorDivGrid(originZ - 16, cell);
    const int maxCZ = FloorDivGrid(originZ + CHUNK_Z + 16, cell);
    const StructureStamper stamper{blocks, originX, originZ};
    for (int gx = minCX; gx <= maxCX; ++gx) {
        for (int gz = minCZ; gz <= maxCZ; ++gz) {
            const StructureCandidate c = SampleStructureCell(seed, gx, gz, nether);
            if (!c.present || IsNetherStructure(c.type) != nether) continue;
            const int gy = StructureGroundY(noise, c);
            switch (c.type) {
                case StructureType::Village: BuildVillage(stamper, c.x, gy, c.z); break;
                case StructureType::DesertPyramid: BuildDesertPyramid(stamper, c.x, gy, c.z); break;
                case StructureType::JungleTemple: BuildJungleTemple(stamper, c.x, gy, c.z); break;
                case StructureType::RuinedPortal: BuildRuinedPortal(stamper, c.x, gy, c.z); break;
                case StructureType::PillagerOutpost: BuildPillagerOutpost(stamper, c.x, gy, c.z); break;
                case StructureType::Mineshaft:
                    BuildMineshaft(stamper, c.x, std::clamp(gy - 26, 14, 44), c.z);
                    break;
                case StructureType::Stronghold:
                    BuildStronghold(stamper, c.x, std::clamp(gy - 22, 12, 40), c.z);
                    break;
                case StructureType::NetherFortress: BuildNetherFortress(stamper, c.x, 72, c.z); break;
                case StructureType::Bastion: BuildBastion(stamper, c.x, 68, c.z); break;
                default: break;
            }
        }
    }
}

void GenerateNetherChunk(uint8_t* blocks, const PerlinNoise& noise, uint64_t seed, int cx, int cz) {
    const uint32_t seed32 = SeedHash32(seed);
    const int ox = cx * CHUNK_X, oz = cz * CHUNK_Z;
    for (int x = 0; x < CHUNK_X; ++x) {
        for (int z = 0; z < CHUNK_Z; ++z) {
            const float wx = float(ox + x), wz = float(oz + z);
            const float ground = 64.0f + noise.Fbm2(wx * 0.01f, wz * 0.01f, 3) * 22.0f;
            for (int y = 0; y < CHUNK_Y; ++y) {
                const uint32_t r = Hash3(uint32_t(wx), uint32_t(y), uint32_t(wz), seed32) % 1000;
                uint8_t b = BLOCK_AIR;
                if (y == 0 || (y < 4 && r < 600) || y >= 252 || (y > 248 && r < 600)) {
                    b = BLOCK_BEDROCK;
                } else {
                    const float cave = noise.Fbm3(wx * 0.045f, float(y) * 0.05f, wz * 0.045f, 3);
                    bool solid = cave < 0.28f;
                    if (float(y) < ground - 6.0f) solid = true;
                    if (y > 128 && y < 250 && cave < 0.45f) solid = true;
                    if (solid) {
                        b = BLOCK_NETHERRACK;
                        if (y > 120 && r < 35) b = BLOCK_GLOWSTONE;
                        else if (y < 120 && r < 90) b = BLOCK_NETHER_QUARTZ_ORE;
                        else if (r < 130) b = BLOCK_NETHER_GOLD_ORE;
                        else if (y < 24 && r > 985) b = BLOCK_ANCIENT_DEBRIS;
                        else if (y > 100 && r > 940) b = BLOCK_MAGMA;
                        else if (float(y) > ground - 4.0f && r > 880 && r <= 920) b = BLOCK_SOUL_SAND;
                        else if (r > 920 && r <= 935) b = BLOCK_SOUL_SOIL;
                        else if (r > 960 && r <= 975) b = BLOCK_BLACKSTONE;
                    } else if (y <= 31) {
                        b = BLOCK_LAVA;
                    }
                }
                blocks[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y] = b;
            }
        }
    }
    if (structures) ApplyStructures(blocks, noise, seed, cx, cz, true);
}

void GenerateClimateChunk(uint8_t* blocks, const PerlinNoise& noise, uint64_t seed, int cx, int cz,
                          bool largeBiomes = false, bool amplified = false, bool structures = true) {
    memset(blocks, BLOCK_AIR, CHUNK_VOL * sizeof(uint8_t));
    const int originX = cx * CHUNK_X, originZ = cz * CHUNK_Z;
    TerrainColumn columns[CHUNK_X + 2][CHUNK_Z + 2];
    for (int x = -1; x <= CHUNK_X; ++x) {
        for (int z = -1; z <= CHUNK_Z; ++z) {
            columns[x + 1][z + 1] = SampleTerrain(noise, originX + x, originZ + z, largeBiomes, amplified);
        }
    }
    for (int x = 0; x < CHUNK_X; ++x) {
        for (int z = 0; z < CHUNK_Z; ++z) {
            const int wx = originX + x, wz = originZ + z;
            const TerrainColumn column = columns[x + 1][z + 1];
            const int h = column.height;
            const bool river = column.biome == Biome::River;
            const bool sandy = column.biome == Biome::Desert || (!river && h <= CLIMATE_SEA + 2);
            const bool rocky = column.biome == Biome::Mountains && h > 104;
            const bool snowy = !sandy && !river && (column.temperature < 0.24f || h >= 155);
            const bool submerged = h <= CLIMATE_SEA + 1 || river || column.biome == Biome::Ocean;
            const int neighborHeights[4] = {columns[x][z + 1].height, columns[x + 2][z + 1].height,
                                           columns[x + 1][z].height, columns[x + 1][z + 2].height};
            int waterFloor = CLIMATE_SEA;
            for (int floor : neighborHeights) waterFloor = std::min(waterFloor, floor);
            int veinCell = -1;
            ClimateVein vein;
            for (int y = 0; y <= h; ++y) {
                uint8_t block = y < 8 ? BLOCK_DEEPSLATE : BLOCK_STONE;
                if (y == 0) block = BLOCK_BEDROCK;
                else if (river && y >= h - 2) block = BLOCK_GRAVEL;
                else if (sandy && y >= h - 3) block = BLOCK_SAND;
                else if (sandy && y >= h - 6) block = BLOCK_SANDSTONE;
                else if (y == h) block = snowy ? BLOCK_SNOW : (rocky ? BLOCK_STONE : BLOCK_GRASS);
                else if (!rocky && y >= h - 3) block = BLOCK_DIRT;
                if (block == BLOCK_STONE || block == BLOCK_DEEPSLATE) {
                    const int cellY = y / 8;
                    if (cellY != veinCell) {
                        vein = SampleClimateVein(wx, cellY, wz, seed);
                        veinCell = cellY;
                    }
                    const float dy = (float(y) - vein.centerY) * vein.invRadiusY;
                    if (y < h - 3 && y < vein.maxY && vein.lateral + dy * dy < 1.0f && vein.block != BLOCK_STONE)
                        block = y < 8 ? DeepslateOre(vein.block) : vein.block;
                }
                // Keep a solid roof and seal cave faces beside water, including across chunk edges.
                if (!submerged && y > 6 && y < h - 5 && (y > CLIMATE_SEA || y <= waterFloor)) {
                    const float a = noise.Noise3(float(wx) * 0.036f, float(y) * 0.055f, float(wz) * 0.036f);
                    const float b = noise.Noise3(float(wx) * 0.038f + 47.2f, float(y) * 0.057f - 19.8f, float(wz) * 0.038f + 83.1f);
                    if ((fabsf(a) < 0.055f && fabsf(b) < 0.055f) || (y < 32 && a > 0.62f && b > 0.25f)) block = BLOCK_AIR;
                }
                blocks[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y] = block;
            }
            for (int y = h + 1; y <= CLIMATE_SEA; ++y) {
                blocks[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y] =
                    y == CLIMATE_SEA && column.temperature < 0.27f ? BLOCK_ICE : BLOCK_WATER;
            }
        }
    }

    auto place = [&](int wx, int y, int wz, uint8_t block, bool trunk) {
        const int x = wx - originX, z = wz - originZ;
        if (x < 0 || x >= CHUNK_X || z < 0 || z >= CHUNK_Z || y < 0 || y >= CHUNK_Y) return;
        uint8_t& target = blocks[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y];
        if (target == BLOCK_AIR || (trunk && IsTreeLeaf(target))) target = block;
    };
    // A four-block halo and shared candidate order keep tree overlap deterministic at chunk seams.
    for (int wx = originX - 4; wx < originX + CHUNK_X + 4; ++wx) {
        for (int wz = originZ - 4; wz < originZ + CHUNK_Z + 4; ++wz) {
            const uint32_t hash = Hash3(uint32_t(wx), 0, uint32_t(wz), SeedHash32(seed) ^ 0x51ED270Bu);
            if ((hash & 255u) >= 16u) continue;
            const TerrainColumn column = SampleTerrain(noise, wx, wz, largeBiomes, amplified);
            if (column.height <= CLIMATE_SEA + 2 || column.height > 145) continue;
            uint32_t density = 0;
            uint8_t log = BLOCK_LOG, leaves = BLOCK_OAK_LEAVES;
            switch (column.biome) {
                case Biome::Plains: density = 2; break;
                case Biome::Forest:
                    density = 13;
                    if ((hash >> 8) % 5 == 0) { log = BLOCK_BIRCH_LOG; leaves = BLOCK_BIRCH_LEAVES; }
                    break;
                case Biome::BirchForest: density = 16; log = BLOCK_BIRCH_LOG; leaves = BLOCK_BIRCH_LEAVES; break;
                case Biome::CherryGrove: density = 15; log = BLOCK_CHERRY_LOG; leaves = BLOCK_CHERRY_LEAVES; break;
                case Biome::Taiga: density = 14; log = BLOCK_SPRUCE_LOG; leaves = BLOCK_SPRUCE_LEAVES; break;
                case Biome::Snowy: density = 2; log = BLOCK_SPRUCE_LOG; leaves = BLOCK_SPRUCE_LEAVES; break;
                default: continue;
            }
            if ((hash & 255u) >= density) continue;
            const int base = column.height + 1;
            const bool spruce = log == BLOCK_SPRUCE_LOG;
            const int th = spruce ? 7 + int((hash >> 16) % 4) : (log == BLOCK_BIRCH_LOG ? 5 : 4) + int((hash >> 16) % 3);
            if (spruce) {
                for (int dy = 2; dy <= th + 1; ++dy) {
                    int radius = std::min(4, (th + 2 - dy) / 2);
                    if ((dy & 1) && radius > 1) --radius;
                    for (int dx = -radius; dx <= radius; ++dx) {
                        for (int dz = -radius; dz <= radius; ++dz) {
                            if (dx * dx + dz * dz > radius * radius + 1) continue;
                            place(wx + dx, base + dy, wz + dz, leaves, false);
                        }
                    }
                }
            } else {
                for (int dy = th - 2; dy <= th + 1; ++dy) {
                    const int radius = dy < th ? 2 : 1;
                    for (int dx = -radius; dx <= radius; ++dx) {
                        for (int dz = -radius; dz <= radius; ++dz) {
                            if (abs(dx) + abs(dz) > radius + 1 || (dy == th + 1 && abs(dx) + abs(dz) > 1)) continue;
                            place(wx + dx, base + dy, wz + dz, leaves, false);
                        }
                    }
                }
            }
            for (int dy = 0; dy < th; ++dy) place(wx, base + dy, wz, log, true);
        }
    }

    ApplyStructures(blocks, noise, seed, cx, cz, false);
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

    // Sky light: per-column height of the highest solid cell. A face whose
    // adjacent air cell sits above that height sees the full sky (15); faces
    // under an overhang are shadowed. This feeds the shader's 2D lightmap so
    // surface faces stay bright while cave interiors stay dark.
    int heights[CHUNK_X * CHUNK_Z];
    for (int z = 0; z < CHUNK_Z; ++z) {
        for (int x = 0; x < CHUNK_X; ++x) {
            int h = -1;
            for (int y = CHUNK_Y - 1; y >= 0; --y) {
                if (BLOCK_FLAGS[sample(x, y, z)] & BF_SOLID) { h = y; break; }
            }
            heights[x + CHUNK_X * z] = h;
        }
    }
    auto skyAt = [&](int nx, int ny, int nz) -> float {
        if (ny >= CHUNK_Y) return 15.0f;
        if (ny < 0) return 0.0f;
        // Quad corners on the outer boundary (16) resolve to the edge column.
        if (nx < 0) nx = 0; else if (nx >= CHUNK_X) nx = CHUNK_X - 1;
        if (nz < 0) nz = 0; else if (nz >= CHUNK_Z) nz = CHUNK_Z - 1;
        return ny > heights[nx + CHUNK_X * nz] ? 15.0f : 0.0f;
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
                        if (d == 0) {
                            vert.u = cell.sign > 0 ? float(h) - corners[c][1] : corners[c][1];
                            vert.v = float(w) - corners[c][0];
                        } else {
                            vert.u = cell.sign > 0 ? corners[c][0] : float(w) - corners[c][0];
                            vert.v = d == 2 ? float(h) - corners[c][1] : corners[c][1];
                        }
                        vert.light = float(cell.light);
                        vert.cr = vert.cg = vert.cb = 1.0f;
                        // Air cell on the face's normal side; p[d] is the
                        // plane coordinate (opaque cell at p[d]-1 when sign>0).
                        int ax = int(vert.x), ay = int(vert.y), az = int(vert.z);
                        const int airCell = int(p[d]) + (cell.sign > 0 ? 0 : -1);
                        if (d == 0) ax = airCell;
                        else if (d == 1) ay = airCell;
                        else az = airCell;
                        vert.sky = skyAt(ax, ay, az);
                        dst.vertices.push_back(vert);
                    }
                    dst.indices.push_back(base);
                    dst.indices.push_back(base + (cell.sign > 0 ? 1 : 2));
                    dst.indices.push_back(base + (cell.sign > 0 ? 2 : 1));
                    dst.indices.push_back(base);
                    dst.indices.push_back(base + (cell.sign > 0 ? 2 : 3));
                    dst.indices.push_back(base + (cell.sign > 0 ? 3 : 2));

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

void BuildExtrudedItemMesh(const uint8_t* rgba, std::vector<MeshVertex>& out) {
    const float s = 0.55f / 16.0f;
    const float t = s * 0.5f;
    auto opaque = [&](int x, int y) -> bool {
        if (x < 0 || y < 0 || x >= 16 || y >= 16) return false;
        return rgba[(y * 16 + x) * 4 + 3] > 0;
    };
    auto vert = [&](float x, float y, float z, float u, float v) {
        MeshVertex mv;
        mv.x = x; mv.y = y; mv.z = z;
        mv.tu = 0; mv.tv = 0; mv.u = u; mv.v = v;
        mv.light = 15.0f; mv.cr = mv.cg = mv.cb = 1.0f;
        out.push_back(mv);
    };
    auto quad = [&](float x0, float y0, float x1, float y1, float z0, float z1,
                    float u0, float v0, float u1, float v1) {
        vert(x0,y0,z0,u0,v0); vert(x1,y0,z0,u1,v0); vert(x1,y1,z0,u1,v1);
        vert(x0,y0,z0,u0,v0); vert(x1,y1,z0,u1,v1); vert(x0,y1,z0,u0,v1);
    };
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            if (!opaque(x, y)) continue;
            const float x0 = (x - 8) * s, x1 = (x + 1 - 8) * s;
            const float y0 = (8 - y) * s, y1 = (8 - y - 1) * s;
            const float u0 = x / 16.0f, u1 = (x + 1) / 16.0f;
            const float v0 = y / 16.0f, v1 = (y + 1) / 16.0f;
            quad(x0,y0,x1,y1, t,t, u0,v0,u1,v1);   // front
            quad(x0,y0,x1,y1,-t,-t, u0,v0,u1,v1);  // back
            if (!opaque(x-1,y)) quad(x0,y0,x0,y1,-t,t, u0,v0,u0,v1); // left
            if (!opaque(x+1,y)) quad(x1,y0,x1,y1,-t,t, u1,v0,u1,v1); // right
            if (!opaque(x,y-1)) quad(x0,y0,x1,y0,-t,t, u0,v0,u1,v0); // top
            if (!opaque(x,y+1)) quad(x0,y1,x1,y1,-t,t, u0,v1,u1,v1); // bottom
        }
    }
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

World::World(uint64_t seed, int renderDistance, WorldGenerator generator)
    : mSeed(seed), mGenerator(generator), mRd(renderDistance), mNoise(seed) {
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
    std::vector<uint8_t> raw(CHUNK_VOL, BLOCK_AIR);
    if (mGenerator == WorldGenerator::Climate || mGenerator == WorldGenerator::LargeBiomes ||
        mGenerator == WorldGenerator::Amplified) {
        GenerateClimateChunk(raw.data(), mNoise, mSeed, cx, cz,
                             mGenerator == WorldGenerator::LargeBiomes,
                             mGenerator == WorldGenerator::Amplified, mStructures);
        c.SetRaw(raw.data());
        return;
    }
    if (mGenerator == WorldGenerator::Nether) {
        GenerateNetherChunk(raw.data(), mNoise, mSeed, cx, cz);
        c.SetRaw(raw.data());
        return;
    }
    if (mGenerator == WorldGenerator::Flat) {
        constexpr int layer = CHUNK_X * CHUNK_Z;
        std::fill_n(raw.data(), layer, BLOCK_BEDROCK);
        std::fill_n(raw.data() + layer, layer * 2, BLOCK_DIRT);
        std::fill_n(raw.data() + layer * 3, layer, BLOCK_GRASS);
        c.SetRaw(raw.data());
        return;
    }
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
                    const uint32_t r = Hash3(uint32_t(wx), uint32_t(y), uint32_t(wz), SeedHash32(mSeed)) % 1000;
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
                raw[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y] = b;
            }
            for (int y = h + 1; y <= SEA; ++y) {
                raw[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y] = BLOCK_WATER;
            }
        }
    }

    // Oak trees. Trunks stay 2 blocks inside the chunk border so the radius-2
    // canopy never writes into a neighbor (no cross-chunk gen writes needed).
    for (int x = 2; x < CHUNK_X - 2; ++x) {
        for (int z = 2; z < CHUNK_Z - 2; ++z) {
            const int h = heights[x][z];
            if (h <= SEA + 1 || h + 9 >= CHUNK_Y) continue;
            if (raw[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * h] != BLOCK_GRASS) continue;
            const uint32_t r = Hash3(uint32_t(cx * CHUNK_X + x), 0, uint32_t(cz * CHUNK_Z + z), SeedHash32(mSeed) ^ 0x51EDu);
            if (r % 96 != 0) continue;
            const int base = h + 1, th = 4 + int(r % 3);
            auto setIfAir = [&](int lx, int y, int lz, uint8_t blk) {
                const size_t idx = size_t(lx) + CHUNK_X * size_t(lz) + CHUNK_X * CHUNK_Z * size_t(y);
                if (raw[idx] == BLOCK_AIR) raw[idx] = blk;
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
                raw[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * (base + i)] = BLOCK_LOG;
            }
        }
    }
    c.SetRaw(raw.data());
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
        const uint8_t b = c->GetIndex(lx + CHUNK_X * lz + CHUNK_X * CHUNK_Z * y);
        if (BLOCK_FLAGS[b] & BF_SOLID) return y;
    }
    return -1;
}

TerrainColumn World::LodColumn(int x, int z) const {
    if (mGenerator == WorldGenerator::Climate || mGenerator == WorldGenerator::LargeBiomes ||
        mGenerator == WorldGenerator::Amplified)
        return SampleTerrain(mNoise, x, z, mGenerator == WorldGenerator::LargeBiomes,
                             mGenerator == WorldGenerator::Amplified);
    TerrainColumn c{};
    if (mGenerator == WorldGenerator::Flat) {
        c.height = 3;
        c.biome = Biome::Plains;
        c.temperature = 0.6f;
        c.humidity = 0.5f;
    }
    return c;
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
            {
                std::lock_guard<std::mutex> lk(mEditsMutex);
                const auto edits = mEdits.find(ChunkKey(gj.cx, gj.cz));
                if (edits != mEdits.end())
                    for (const auto& e : edits->second) gj.chunk->SetIndex(e.first, e.second);
            }
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
                // Mesh building consumes raw arrays; decode the paletted sections
                // into scratch buffers for the chunk and its four neighbors.
                std::vector<uint8_t> selfRaw(CHUNK_VOL), negXraw, posXraw, negZraw, posZraw;
                auto decode = [](const std::shared_ptr<Chunk>& c, std::vector<uint8_t>& out) -> const uint8_t* {
                    if (!c) return nullptr;
                    out.resize(CHUNK_VOL);
                    c->GetRaw(out.data());
                    return out.data();
                };
                mj.chunk->GetRaw(selfRaw.data());
                BuildChunkMeshCPU(selfRaw.data(),
                                  decode(mj.negX, negXraw),
                                  decode(mj.posX, posXraw),
                                  decode(mj.negZ, negZraw),
                                  decode(mj.posZ, posZraw),
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
    return c->GetIndex((x - cx * CHUNK_X) + CHUNK_X * (z - cz * CHUNK_Z) + CHUNK_X * CHUNK_Z * y);
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
    const int index = lx + CHUNK_X * lz + CHUNK_X * CHUNK_Z * y;
    {
        std::lock_guard<std::mutex> lk(mEditsMutex);
        mEdits[ChunkKey(cx, cz)][uint16_t(index)] = block;
    }
    {
        std::unique_lock<std::shared_mutex> blk(mBlocksMutex);
        c->SetIndex(index, block);
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

EditMap World::SnapshotEdits() const {
    std::lock_guard<std::mutex> lk(mEditsMutex);
    return mEdits;
}

void World::SetEdits(EditMap edits) {
    std::lock_guard<std::mutex> lk(mEditsMutex);
    mEdits = std::move(edits);
}

static void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

static bool GetU32(const uint8_t* data, size_t size, size_t& pos, uint32_t& v) {
    if (size - pos < 4) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(data[pos + i]) << (8 * i);
    pos += 4;
    return true;
}

std::vector<uint8_t> EncodeEdits(const EditMap& edits) {
    std::vector<uint64_t> keys;
    for (const auto& kv : edits) if (!kv.second.empty()) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    std::vector<uint8_t> out = {'P', 'T', 'E', 'D'};
    PutU32(out, 1);
    PutU32(out, uint32_t(keys.size()));
    for (uint64_t key : keys) {
        const ChunkEdits& chunk = edits.at(key);
        std::vector<std::pair<uint16_t, uint8_t>> entries(chunk.begin(), chunk.end());
        std::sort(entries.begin(), entries.end());
        PutU32(out, uint32_t(KeyCX(key)));
        PutU32(out, uint32_t(KeyCZ(key)));
        PutU32(out, uint32_t(entries.size()));
        for (const auto& e : entries) {
            out.push_back(uint8_t(e.first));
            out.push_back(uint8_t(e.first >> 8));
            out.push_back(e.second);
        }
    }
    return out;
}

bool DecodeEdits(const uint8_t* data, size_t size, EditMap& out) {
    size_t pos = 4;
    uint32_t version = 0, chunks = 0;
    if (size < 4 || memcmp(data, "PTED", 4) != 0 || !GetU32(data, size, pos, version) ||
        version != 1 || !GetU32(data, size, pos, chunks) || chunks > (size - pos) / 12)
        return false;
    EditMap result;
    for (uint32_t c = 0; c < chunks; ++c) {
        uint32_t cx = 0, cz = 0, count = 0;
        if (!GetU32(data, size, pos, cx) || !GetU32(data, size, pos, cz) ||
            !GetU32(data, size, pos, count) || count > uint32_t(CHUNK_VOL) ||
            (size - pos) / 3 < count)
            return false;
        const uint64_t key = ChunkKey(int32_t(cx), int32_t(cz));
        if (result.count(key)) return false;
        ChunkEdits& chunk = result[key];
        for (uint32_t i = 0; i < count; ++i, pos += 3) {
            const uint8_t block = data[pos + 2];
            const uint16_t index = uint16_t(data[pos] | (data[pos + 1] << 8));
            if (block >= BLOCK_COUNT || chunk.count(index)) return false;
            chunk[index] = block;
        }
    }
    if (pos != size) return false;
    out = std::move(result);
    return true;
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