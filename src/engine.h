#pragma once
#include <windows.h>
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace potato {

// Hard ceilings for the time-sliced anti-lag buffer. The render thread never
// exceeds these per frame, so a burst of worker completions cannot spike the
// frame time on the i3-M380.
constexpr int MAX_UPLOADS_PER_FRAME = 8;    // mesh GPU uploads
constexpr int MAX_EVICTS_PER_FRAME = 4;     // subchunk memory cleanup sweeps
constexpr int MAX_GPU_SWEEP_PER_FRAME = 32; // evicted-GPU-entry checks
constexpr int MAX_PARTICLES = 256;

struct Frustum {
    DirectX::XMFLOAT4 planes[6];
};
void ExtractFrustum(const DirectX::XMMATRIX& vp, Frustum& f);
bool AABBInFrustum(const Frustum& f, float minX, float minY, float minZ,
                   float maxX, float maxY, float maxZ);

// Raw QueryPerformanceCounter milliseconds. Combat timing uses this directly
// so a clamped render delta can never skew the 1.9+ cooldown math.
float SystemTimeMs();

// High-resolution delta-time clock. Clamped so debugger stalls and alt-tab
// cannot produce a physics explosion.
class DeltaClock {
public:
    DeltaClock() {
        QueryPerformanceFrequency(&mFreq);
        QueryPerformanceCounter(&mLast);
    }
    float Tick() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - mLast.QuadPart) / double(mFreq.QuadPart);
        mLast = now;
        if (dt > 0.1) dt = 0.1;
        if (dt < 0.0) dt = 0.0;
        return float(dt);
    }
private:
    LARGE_INTEGER mFreq;
    LARGE_INTEGER mLast;
};

class WorldClock {
public:
    void Advance(double seconds);
    void Set(uint64_t ticks);
    uint64_t Ticks() const { return mTicks; }
    double Fraction() const { return mFraction; }
private:
    uint64_t mTicks = 0;
    double mFraction = 0;
    double mCompensation = 0;
};

struct SkyState {
    DirectX::XMFLOAT3 skyColor{0.47f, 0.65f, 1.0f};
    DirectX::XMFLOAT3 fogColor{0.47f, 0.65f, 1.0f};
    DirectX::XMFLOAT3 lightColor{1.0f, 1.0f, 1.0f};
    float daylight = 1.0f, sunAngle = 0.0f, stars = 0.0f;
    DirectX::XMFLOAT4 twilight{0, 0, 0, 0};
    int moonPhase = 0;
};

// Loads caller-supplied day.json from the installed client, never bundled tables.
class SkyTimeline {
public:
    bool LoadDay(const std::string& json);
    bool Loaded() const { return mLoaded; }
    SkyState Sample(uint64_t ticks, double fraction = 0) const;
private:
    struct Keyframe {
        uint32_t ticks;
        DirectX::XMFLOAT4 value;
    };
    struct Track {
        std::vector<Keyframe> keyframes;
        bool eased = false, angleWrap = false;
        double bezier[4] = {0, 0, 1, 1};
        DirectX::XMFLOAT4 Sample(double time) const;
    };
    enum { SkyColor, FogColor, LightColor, Daylight, Stars, Twilight, SunAngle, TrackCount };
    Track mTracks[TrackCount];
    bool mLoaded = false;
};

// Rigid request ring buffer: fixed capacity, no allocation at push/pop time.
// TryPush fails when full (backpressure), TryPop fails when empty. Mutex
// guarded; critical sections are a single element move, never blocking I/O.
template <typename T, size_t CAPACITY>
class RingQueue {
public:
    bool TryPush(const T& v) {
        std::lock_guard<std::mutex> lk(mMutex);
        if (mCount == CAPACITY) return false;
        mSlots[mHead] = v;
        mHead = (mHead + 1) % CAPACITY;
        ++mCount;
        return true;
    }
    bool TryPush(T&& v) {
        std::lock_guard<std::mutex> lk(mMutex);
        if (mCount == CAPACITY) return false;
        mSlots[mHead] = std::move(v); // moved only after space is guaranteed
        mHead = (mHead + 1) % CAPACITY;
        ++mCount;
        return true;
    }
    bool TryPop(T& out) {
        std::lock_guard<std::mutex> lk(mMutex);
        if (mCount == 0) return false;
        out = std::move(mSlots[mTail]);
        mTail = (mTail + 1) % CAPACITY;
        --mCount;
        return true;
    }
    size_t Size() const {
        std::lock_guard<std::mutex> lk(mMutex);
        return mCount;
    }
private:
    T mSlots[CAPACITY];
    size_t mHead = 0, mTail = 0, mCount = 0;
    mutable std::mutex mMutex;
};

struct InputState {
    bool keys[256];
    bool prevKeys[256];
    float mouseDX, mouseDY;
    float wheel;
    bool leftClick, rightClick;
    bool leftDown;
    int mouseX, mouseY;
    std::string typed; // WM_CHAR text for menu text fields, cleared each frame

    InputState()
        : keys{}, prevKeys{}, mouseDX(0), mouseDY(0), wheel(0),
          leftClick(false), rightClick(false), leftDown(false), mouseX(0), mouseY(0) {}

    bool KeyPressed(int vk) const { return keys[vk & 0xFF] && !prevKeys[vk & 0xFF]; }

    void EndFrame() {
        memcpy(prevKeys, keys, sizeof(keys));
        mouseDX = mouseDY = 0;
        wheel = 0;
        leftClick = rightClick = false;
        typed.clear();
    }
};

struct KeyBinds {
    int forward = 'W', back = 'S', left = 'A', right = 'D';
    int jump = VK_SPACE, sneak = VK_SHIFT, sprint = VK_CONTROL;
};
extern KeyBinds gBinds;

// Fly camera: yaw/pitch mouse look, WASD + Space/Shift, dt-corrected.
// Conventions: yaw 0 faces -Z, +X is right, +Y is up.
class FlyCamera {
public:
    float x = 0.0f, y = 90.0f, z = 0.0f;
    float yaw = 0.0f, pitch = 0.0f;
    float sensitivity = 0.0025f;
    float speed = 12.0f;
    float fovDeg = 70.0f;
    float vy = 0.0f;                 // vertical velocity, used for crit detection
    float fwdX = 0, fwdY = 0, fwdZ = -1;
    float rightX = 1, rightZ = 0;

    void Look(float dx, float dy);
    void Update(const InputState& in, float dt);
};

// Walk-mode player body: 0.6 x 1.8 AABB, axis-separated voxel collision with
// substepping (no tunneling at terminal velocity). Vanilla speeds: walk
// 4.317, sprint 5.612, sneak 1.31 m/s; gravity 32, jump ~1.25 blocks.
// Flying (creative) keeps the old noclip-style free movement but still
// collides. The camera follows at eye height and inherits vy for crits.
struct PlayerBody {
    float x = 8.5f, y = 90.0f, z = 8.5f; // feet position
    float vx = 0, vy = 0, vz = 0;
    float walkSpeed = 4.317f, impulseX = 0, impulseZ = 0;
    bool sprintLatch = false;
    bool onGround = false, flying = true, sprinting = false, sneaking = false;
    bool inWater = false;
    bool elytraEquipped = false, gliding = false;
    static constexpr float HALF = 0.3f, HEIGHT = 1.8f, EYE = 1.62f;

    void Update(const InputState& in, float dt, FlyCamera& cam,
                const std::function<uint8_t(int, int, int)>& getBlock);
};

struct ItemSlot {
    uint8_t id = 0;
    int count = 0;
    uint16_t damage = 0;
    bool Empty() const { return id == 0 || count <= 0; }
};

// One crafting recipe. cells hold an item id, 0 for an empty cell, or a
// RecipeTag value (>= 0x100) for ingredient tags such as #minecraft:planks.
// Built-in recipes are used until the jar registry loads Java's data files.
struct CraftingRecipe {
    int width = 0;
    int height = 0;
    uint16_t cells[9] = {};
    ItemSlot result{};
    bool shapeless = false;
};

struct InventoryState {
    ItemSlot slots[36];
    ItemSlot craft[9];
    ItemSlot armor[4]; // helmet, chestplate, leggings, boots
    ItemSlot offhand;
    ItemSlot cursor;

    int Capacity(uint8_t id) const;
    int Add(uint8_t id, int count, uint16_t damage = 0);
    ItemSlot Result(bool table) const;
    bool Click(int index, bool right, bool table, bool shift = false);
    // Java double-click pickup: gather every matching stack into the cursor.
    bool DoubleClickCollect(int index, bool table);
    bool Close();
    int Defense() const;
    float Toughness() const;
};

float BlockHardness(uint8_t block);
float MiningSeconds(uint8_t block, uint8_t item, bool onGround = true, bool underwater = false);
bool CanHarvest(uint8_t block, uint8_t item);
ItemSlot BlockDrops(uint8_t block, uint8_t item);
bool WearTool(ItemSlot& slot, int amount);

struct FurnaceState {
    ItemSlot input, fuel, output;
    float burn = 0, burnTotal = 0, cook = 0;
    int x = 0, y = 0, z = 0;
};

struct ChestState {
    ItemSlot slots[27];
};

// Java 26.3 creative tabs, in menu order. Search is always last.
enum CreativeTab {
    CTAB_BUILDING, CTAB_COLORED, CTAB_NATURAL, CTAB_FUNCTIONAL, CTAB_REDSTONE,
    CTAB_TOOLS, CTAB_COMBAT, CTAB_FOOD, CTAB_INGREDIENTS, CTAB_SEARCH, CTAB_COUNT
};
int ItemCreativeTab(uint8_t id);
const char* CreativeTabName(int tab);
uint8_t CreativeTabIcon(int tab);
bool ItemMatchesSearch(uint8_t id, const std::string& query);

struct SurvivalState {
    int health = 20, hunger = 20;
    float exhaustion = 0, saturation = 5, regenTimer = 0, starveTimer = 0;
    bool dead = false;

    void OnLand(float fallDistance) {
        const int dmg = int(fallDistance) - 3;
        if (dmg > 0) health -= dmg;
        if (health < 0) health = 0;
    }
    void Update(float dt, bool moving, bool sprinting, int difficulty) {
        if (difficulty == 0) {
            hunger = 20;
            exhaustion = starveTimer = 0;
            if (health > 0 && health < 20) {
                regenTimer += dt;
                if (regenTimer >= 1.0f) { regenTimer -= 1.0f; ++health; }
            } else regenTimer = 0;
            dead = health <= 0;
            return;
        }
        if (moving && sprinting) exhaustion += dt * 5.612f * 0.1f;
        if (exhaustion >= 4.0f) {
            exhaustion -= 4.0f;
            if (saturation > 0) saturation = saturation > 1 ? saturation - 1 : 0;
            else if (hunger > 0) --hunger;
        }
        if (hunger >= 18 && health > 0 && health < 20) {
            regenTimer += dt;
            if (regenTimer >= 4.0f) { regenTimer -= 4.0f; ++health; exhaustion += 6.0f; }
        } else {
            regenTimer = 0;
        }
        const int starvationFloor = difficulty == 1 ? 10 : (difficulty == 2 ? 1 : 0);
        if (hunger == 0 && health > starvationFloor) {
            starveTimer += dt;
            if (starveTimer >= 4.0f) { starveTimer -= 4.0f; --health; }
        } else {
            starveTimer = 0;
        }
        dead = health <= 0;
    }
};

struct HurtResistance {
    float remaining = 0, lastDamage = 0;
    void Advance(float dt) { remaining = std::max(0.0f, remaining - dt); }
    float Apply(float damage);
};

enum class MobType {
    Zombie, Pig, Cow, Sheep, Chicken, Creeper, Skeleton, Spider, Enderman, Count
};

// Java 26.3 base attributes. drop/drop2 are resource names resolved against the
// JAR registry at kill time (empty = no drop).
struct MobStats {
    const char* id;
    const char* name;
    float health;
    float speed;
    float height;
    float halfWidth;
    float attackDamage; // 0 for passive mobs
    bool hostile;
    const char* drop;
    int dropMin;
    int dropMax;
    const char* drop2;
    int drop2Min;
    int drop2Max;
};

const MobStats& GetMobStats(MobType type);
const char* MobId(MobType type);
const char* MobName(MobType type);
bool ParseMobId(const std::string& name, MobType& type);

struct Mob {
    MobType type = MobType::Zombie;
    PlayerBody body;
    HurtResistance resistance;
    float health = 20, yaw = 0, attackDelay = 0;
    float wanderTimer = 0, wanderYaw = 0;
    float fuse = -1; // creeper ignition countdown, < 0 when idle
    bool passive = false;
    Mob() { SetType(MobType::Zombie); }
    void SetType(MobType type);
    void Update(float dt, const PlayerBody& player, const std::function<uint8_t(int, int, int)>& getBlock);
    float Hit(float damage, float knockX, float knockZ);
};
using Zombie = Mob;
bool RayHitsBody(const FlyCamera& camera, const PlayerBody& body, float reach, float& distance);

// Minecraft 1.9+ attack cooldown: swing recharge gates damage.
// Strength follows vanilla getAttackStrengthScale(0.5f); damage scales by
// 0.2 + 0.8 * strength^2, so spam clicking deals heavily reduced damage.
// Vanilla critical hits additionally require strength > 0.9 while falling.
class AttackCooldown {
public:
    float attackSpeed = 4.0f;  // attacks per second (vanilla empty hand)
    float baseDamage = 1.0f;

    float Strength(float nowMs) const;
    float Swing(float nowMs);
    void Reset(float nowMs) { mLastSwingMs = nowMs; }

private:
    float mLastSwingMs = -1000000.0f;
};

// Per-player combat state attached to the input tracking loop. The unmanaged
// progress float is refreshed every frame from the QPC-derived timestamp and
// drives the HUD charge indicator under the crosshair.
struct PlayerState {
    float attackCooldownProgress = 1.0f;

    void UpdateCombat(const AttackCooldown& cooldown, float nowMs) {
        attackCooldownProgress = cooldown.Strength(nowMs);
    }
};

// Critical-hit particle sweep. CPU simulation only; main.cpp renders the
// live list as camera-facing quads. Spawned exclusively on true vanilla
// crits (strength > 0.9 AND falling) — short-cycled swings spawn nothing.
class ParticleSystem {
public:
    struct Particle {
        float x, y, z;
        float vx, vy, vz;
        float life, maxLife;
        bool bubble = false;
        bool rain = false;
        bool snow = false;
    };

    void SpawnCrit(float x, float y, float z);
    void SpawnBubbles(float x, float y, float z);
    // One weather drop; snow drifts slowly, rain falls in fast streaks.
    void SpawnRain(float x, float y, float z, bool snow);
    void Update(float dt);
    const std::vector<Particle>& List() const { return mParticles; }

private:
    std::vector<Particle> mParticles;
    std::mt19937 mRng{1337};
};

} // namespace potato