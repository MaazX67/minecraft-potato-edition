#pragma once
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
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
    int mouseX, mouseY;

    InputState()
        : keys{}, prevKeys{}, mouseDX(0), mouseDY(0), wheel(0),
          leftClick(false), rightClick(false), mouseX(0), mouseY(0) {}

    bool KeyPressed(int vk) const { return keys[vk & 0xFF] && !prevKeys[vk & 0xFF]; }

    void EndFrame() {
        memcpy(prevKeys, keys, sizeof(keys));
        mouseDX = mouseDY = 0;
        wheel = 0;
        leftClick = rightClick = false;
    }
};

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
    bool onGround = false, flying = true, sprinting = false, sneaking = false;
    bool inWater = false;
    static constexpr float HALF = 0.3f, HEIGHT = 1.8f, EYE = 1.62f;

    void Update(const InputState& in, float dt, FlyCamera& cam,
                const std::function<uint8_t(int, int, int)>& getBlock);
};

// Survival meters: 20 hp / 20 hunger, fall damage, slow exhaustion, regen
// when fed, starvation down to 1 hp. Drives the vanilla heart/hunger HUD.
struct SurvivalState {
    int health = 20, hunger = 20;
    float exhaustion = 0, regenTimer = 0, starveTimer = 0;
    bool dead = false;

    void OnLand(float fallDistance) {
        const int dmg = int(fallDistance) - 3;
        if (dmg > 0) health -= dmg;
        if (health < 0) health = 0;
    }
    void Update(float dt, bool moving, bool sprinting) {
        exhaustion += dt * (sprinting ? 4.0f : (moving ? 1.0f : 0.15f));
        if (exhaustion >= 40.0f) {
            exhaustion -= 40.0f;
            if (hunger > 0) --hunger;
        }
        if (hunger >= 18 && health > 0 && health < 20) {
            regenTimer += dt;
            if (regenTimer >= 4.0f) { regenTimer = 0; ++health; exhaustion += 6.0f; }
        } else {
            regenTimer = 0;
        }
        if (hunger == 0 && health > 1) {
            starveTimer += dt;
            if (starveTimer >= 4.0f) { starveTimer = 0; --health; }
        } else {
            starveTimer = 0;
        }
        dead = health <= 0;
    }
};

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
    };

    void SpawnCrit(float x, float y, float z);
    void Update(float dt);
    const std::vector<Particle>& List() const { return mParticles; }

private:
    std::vector<Particle> mParticles;
    std::mt19937 mRng{1337};
};

} // namespace potato