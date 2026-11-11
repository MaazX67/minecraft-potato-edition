#include "engine.h"
#include "assets.h"
#include <algorithm>

namespace potato {

float SystemTimeMs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return float(double(now.QuadPart) * 1000.0 / double(freq.QuadPart));
}

void FlyCamera::Look(float dx, float dy) {
    yaw += dx * sensitivity;
    pitch -= dy * sensitivity;
    const float lim = 1.5533431f; // 89 degrees
    if (pitch > lim) pitch = lim;
    if (pitch < -lim) pitch = -lim;
}

void FlyCamera::Update(const InputState& in, float dt) {
    const float sy = sinf(yaw), cy = cosf(yaw);
    float mx = 0, my = 0, mz = 0;
    if (in.keys['W']) { mx += sy; mz -= cy; }
    if (in.keys['S']) { mx -= sy; mz += cy; }
    if (in.keys['D']) { mx += cy; mz += sy; }
    if (in.keys['A']) { mx -= cy; mz -= sy; }
    if (in.keys[VK_SPACE]) my += 1.0f;
    if (in.keys[VK_SHIFT]) my -= 1.0f;
    const float len = sqrtf(mx * mx + my * my + mz * mz);
    if (len > 0.0f) {
        const float s = speed * dt / len;
        x += mx * s; y += my * s; z += mz * s;
        vy = my / len * speed;
    } else {
        vy = 0.0f;
    }
    const float cp = cosf(pitch);
    fwdX = sy * cp; fwdY = sinf(pitch); fwdZ = -cy * cp;
    rightX = cy; rightZ = sy;
}

static bool BodyHits(const std::function<uint8_t(int, int, int)>& getBlock,
                     float x, float y, float z) {
    const int x0 = int(floorf(x - PlayerBody::HALF)), x1 = int(floorf(x + PlayerBody::HALF - 1e-4f));
    const int y0 = int(floorf(y)), y1 = int(floorf(y + PlayerBody::HEIGHT - 1e-4f));
    const int z0 = int(floorf(z - PlayerBody::HALF)), z1 = int(floorf(z + PlayerBody::HALF - 1e-4f));
    for (int bx = x0; bx <= x1; ++bx)
        for (int by = y0; by <= y1; ++by)
            for (int bz = z0; bz <= z1; ++bz)
                if (BLOCK_FLAGS[getBlock(bx, by, bz)] & BF_SOLID) return true;
    return false;
}

void PlayerBody::Update(const InputState& in, float dt, FlyCamera& cam,
                        const std::function<uint8_t(int, int, int)>& getBlock) {
    const float sy = sinf(cam.yaw), cy = cosf(cam.yaw);
    float mx = 0, mz = 0;
    if (in.keys['W']) { mx += sy; mz -= cy; }
    if (in.keys['S']) { mx -= sy; mz += cy; }
    if (in.keys['D']) { mx += cy; mz += sy; }
    if (in.keys['A']) { mx -= cy; mz -= sy; }
    const float len = sqrtf(mx * mx + mz * mz);
    if (len > 0.0f) { mx /= len; mz /= len; }
    sneaking = !flying && in.keys[VK_SHIFT];
    sprinting = !flying && !sneaking && in.keys[VK_CONTROL] && in.keys['W'];
    const float speed = flying ? 12.0f : (sneaking ? 1.31f : (sprinting ? 5.612f : 4.317f));
    vx = mx * speed;
    vz = mz * speed;

    const uint8_t feet = getBlock(int(floorf(x)), int(floorf(y + 0.2f)), int(floorf(z)));
    inWater = (BLOCK_FLAGS[feet] & BF_LIQUID) != 0;

    if (flying) {
        vy = ((in.keys[VK_SPACE] ? 1.0f : 0.0f) - (in.keys[VK_SHIFT] ? 1.0f : 0.0f)) * speed;
    } else if (inWater) {
        vy -= 8.0f * dt;
        if (vy < -4.0f) vy = -4.0f;
        if (in.keys[VK_SPACE]) vy = 4.0f;
    } else {
        vy -= 32.0f * dt;
        if (vy < -50.0f) vy = -50.0f;
        if (onGround && in.keys[VK_SPACE]) { vy = 8.94f; onGround = false; }
    }

    // Substep the integration so terminal-velocity falls cannot tunnel.
    const float maxV = std::max({fabsf(vx), fabsf(vy), fabsf(vz)});
    int steps = 1 + int(maxV * dt / 0.4f);
    if (steps > 16) steps = 16;
    const float sdt = dt / float(steps);
    for (int s = 0; s < steps; ++s) {
        const float nx = x + vx * sdt;
        if (!BodyHits(getBlock, nx, y, z)) {
            x = nx;
        } else {
            const float cx = vx > 0 ? floorf(nx + HALF) - HALF - 0.001f
                                    : floorf(nx - HALF) + 1.0f + HALF + 0.001f;
            if (!BodyHits(getBlock, cx, y, z)) x = cx;
            vx = 0;
        }
        const float nz = z + vz * sdt;
        if (!BodyHits(getBlock, x, y, nz)) {
            z = nz;
        } else {
            const float cz = vz > 0 ? floorf(nz + HALF) - HALF - 0.001f
                                    : floorf(nz - HALF) + 1.0f + HALF + 0.001f;
            if (!BodyHits(getBlock, x, y, cz)) z = cz;
            vz = 0;
        }
        const float ny = y + vy * sdt;
        if (!BodyHits(getBlock, x, ny, z)) {
            y = ny;
            onGround = false;
        } else {
            if (vy < 0) {
                const float ly = floorf(ny) + 1.0f;
                if (!BodyHits(getBlock, x, ly, z)) y = ly;
                onGround = true;
            } else if (vy > 0) {
                const float ly = floorf(ny + HEIGHT) - HEIGHT - 0.001f;
                if (!BodyHits(getBlock, x, ly, z)) y = ly;
            }
            vy = 0;
        }
    }
    if (y < -10.0f) { y = -10.0f; vy = 0; }
    cam.x = x;
    cam.y = y + (sneaking ? EYE - 0.12f : EYE);
    cam.z = z;
    cam.vy = vy;
}

float AttackCooldown::Strength(float nowMs) const {
    const float periodMs = 1000.0f / attackSpeed;
    float s = (nowMs - mLastSwingMs + 25.0f) / periodMs; // +0.5 tick
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    return s;
}

float AttackCooldown::Swing(float nowMs) {
    const float s = Strength(nowMs);
    mLastSwingMs = nowMs;
    return baseDamage * (0.2f + 0.8f * s * s);
}

void ParticleSystem::SpawnCrit(float x, float y, float z) {
    std::uniform_real_distribution<float> angle(0.0f, 6.2831853f);
    std::uniform_real_distribution<float> speed(1.5f, 4.5f);
    std::uniform_real_distribution<float> up(2.0f, 5.0f);
    std::uniform_real_distribution<float> life(0.4f, 0.8f);
    for (int i = 0; i < 16; ++i) {
        if (mParticles.size() >= MAX_PARTICLES) return;
        const float a = angle(mRng), sp = speed(mRng);
        Particle p;
        p.x = x; p.y = y; p.z = z;
        p.vx = cosf(a) * sp;
        p.vy = up(mRng);
        p.vz = sinf(a) * sp;
        p.maxLife = p.life = life(mRng);
        mParticles.push_back(p);
    }
}

void ParticleSystem::Update(float dt) {
    size_t w = 0;
    for (size_t i = 0; i < mParticles.size(); ++i) {
        Particle& p = mParticles[i];
        p.life -= dt;
        if (p.life <= 0.0f) continue;
        p.vy -= 9.8f * dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.z += p.vz * dt;
        mParticles[w++] = p;
    }
    mParticles.resize(w);
}

} // namespace potato