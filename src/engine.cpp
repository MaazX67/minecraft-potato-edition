#include "engine.h"

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