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
// Slime block id (set at boot from the registry) for the bounce behaviour.
void SetSlimeBlockId(uint16_t id);
uint16_t SlimeBlockId();

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
                const std::function<uint16_t(int, int, int)>& getBlock);
};

struct ItemSlot {
    uint16_t id = 0;
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

    int Capacity(uint16_t id) const;
    int Add(uint16_t id, int count, uint16_t damage = 0);
    ItemSlot Result(bool table) const;
    bool Click(int index, bool right, bool table, bool shift = false);
    // Java double-click pickup: gather every matching stack into the cursor.
    bool DoubleClickCollect(int index, bool table);
    bool Close();
    int Defense() const;
    float Toughness() const;
};

float BlockHardness(uint16_t block);
float MiningSeconds(uint16_t block, uint16_t item, bool onGround = true, bool underwater = false);
bool CanHarvest(uint16_t block, uint16_t item);
ItemSlot BlockDrops(uint16_t block, uint16_t item);
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
int ItemCreativeTab(uint16_t id);
const char* CreativeTabName(int tab);
uint16_t CreativeTabIcon(int tab);
bool ItemMatchesSearch(uint16_t id, const std::string& query);

// Java experience curve: points needed to advance from `level` to level+1.
inline int XpToNextLevel(int level) {
    if (level >= 31) return 9 * level - 158;
    if (level >= 16) return 5 * level - 38;
    return 2 * level + 7;
}

struct SurvivalState {
    int health = 20, hunger = 20;
    float exhaustion = 0, saturation = 5, regenTimer = 0, starveTimer = 0;
    bool dead = false;
    // Tutorial system (Java: tutorial hints/toasts)
    int tutorialStep = 0; // 0 = not started, 1+ = current step
    float tutorialTimer = 0; // time since last hint
    bool tutorialComplete = false;
    float xpProgress = 0; int xpLevel = 0;
    int airTicks = 300; float drownTimer = 0, voidTimer = 0;

    void AddXp(int amount) {
        xpProgress += float(amount);
        while (xpLevel < 255 && xpProgress >= float(XpToNextLevel(xpLevel))) {
            xpProgress -= float(XpToNextLevel(xpLevel));
            ++xpLevel;
        }
    }

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
    Zombie, Pig, Cow, Sheep, Chicken, Creeper, Skeleton, Spider, Enderman,
    Slime, Allay, Axolotl, Bat, Bee, Blaze, Camel, Cat, Dolphin, Fox, Frog,
    Ghast, Goat, Horse, IronGolem, Llama, Panda, Parrot, Phantom, Piglin,
    Rabbit, Shulker, Silverfish, Sniffer, SnowGolem, Squid, Strider, Turtle,
    Villager, WanderingTrader, Warden, Witch, Wolf, ZombieVillager, Endermite,
    EnderDragon, Wither, ElderGuardian, Count
};

// Java HumanoidModel.createMesh: left_arm = texOffs(40,16).mirror(),
// left_leg = texOffs(0,16).mirror() (ZombieModel/SkeletonModel inherit this).
// Mirrored left limbs reuse the right-limb texture region; only PlayerModel
// overrides them to (32,48)/(16,48) for the separate 64x64 player-skin limbs.
struct MobLimbUV { float u, v; bool mirror; };
inline MobLimbUV HumanoidLeftArmUV() { return {40.0f, 16.0f, true}; }
inline MobLimbUV HumanoidLeftLegUV() { return {0.0f, 16.0f, true}; }

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
    void Update(float dt, const PlayerBody& player, const std::function<uint16_t(int, int, int)>& getBlock);
    float Hit(float damage, float knockX, float knockZ);
};
using Zombie = Mob;
bool RayHitsBody(const FlyCamera& camera, const PlayerBody& body, float reach, float& distance);
// Per-entity AABB ray test. Java entity pick bounds are the mob's own
// dimensions (EntityDimensions width/height), not the player box; a chicken
// and an enderman must not share one 0.6x1.95 hitbox.
bool RayHitsBox(const FlyCamera& camera, float cx, float feetY, float cz,
                float halfWidth, float height, float reach, float& distance);

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

// Java LivingEntity.swing(): a 6-tick arm animation that is independent of the
// attack cooldown. Progress runs 0..1 across 300 ms (6 ticks at 20 TPS) and
// snaps back to 0 when the swing finishes, exactly like getAttackAnim().
class ArmSwing {
public:
    static constexpr float DurationMs = 300.0f;

    void Trigger(float nowMs) { mStartMs = nowMs; }
    bool Active(float nowMs) const { return nowMs - mStartMs < DurationMs; }
    float Progress(float nowMs) const {
        const float t = (nowMs - mStartMs) / DurationMs;
        return (t < 0.0f || t >= 1.0f) ? 0.0f : t;
    }

private:
    float mStartMs = -1000.0f;
};

// Java 26.3 FirstPersonHandsAndItemsRenderer.swingArm + applyItemArmAttackTransform,
// read from the client JAR with tools/parity/classdump.ps1. side is +1 for the right
// hand, -1 for the left. At progress 0 (and 1) every translation is 0 and the two
// yaw terms cancel, so the pose is the identity and the rest position is unchanged.
struct SwingPose {
    float tx, ty, tz;
    float yaw1Deg, rollDeg, pitchDeg, yaw2Deg;
};
inline SwingPose ComputeSwingPose(float progress, float side) {
    const float p = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
    const float root = sqrtf(p);
    const float sinRoot = sinf(root * 3.14159265f);
    const float sinSquare = sinf(p * p * 3.14159265f);
    SwingPose s;
    s.tx = side * (-0.4f * sinRoot);
    s.ty = 0.2f * sinf(root * 6.28318531f);
    s.tz = -0.2f * sinf(p * 3.14159265f);
    s.yaw1Deg = side * (45.0f - 20.0f * sinSquare);
    s.rollDeg = side * (-20.0f * sinRoot);
    s.pitchDeg = -80.0f * sinRoot;
    s.yaw2Deg = side * -45.0f;
    return s;
}

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