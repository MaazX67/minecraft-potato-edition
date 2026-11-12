#include "engine.h"
#include "assets.h"
#include <algorithm>

namespace potato {

void ExtractFrustum(const DirectX::XMMATRIX& vp, Frustum& f) {
    using namespace DirectX;
    const XMMATRIX columns = XMMatrixTranspose(vp);
    const XMVECTOR c0 = columns.r[0], c1 = columns.r[1], c2 = columns.r[2], c3 = columns.r[3];
    const XMVECTOR p[6] = {
        XMVectorAdd(c3, c0),       // left
        XMVectorSubtract(c3, c0),  // right
        XMVectorAdd(c3, c1),       // bottom
        XMVectorSubtract(c3, c1),  // top
        c2,                       // near: z >= 0
        XMVectorSubtract(c3, c2),  // far: w - z >= 0
    };
    for (int i = 0; i < 6; ++i) {
        XMStoreFloat4(&f.planes[i], XMPlaneNormalize(p[i]));
    }
}

bool AABBInFrustum(const Frustum& f, float minX, float minY, float minZ,
                   float maxX, float maxY, float maxZ) {
    for (int i = 0; i < 6; ++i) {
        const DirectX::XMFLOAT4& pl = f.planes[i];
        const float px = pl.x >= 0 ? maxX : minX;
        const float py = pl.y >= 0 ? maxY : minY;
        const float pz = pl.z >= 0 ? maxZ : minZ;
        if (pl.x * px + pl.y * py + pl.z * pz + pl.w < 0) return false;
    }
    return true;
}

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

namespace {
struct Recipe {
    int width, height;
    uint8_t cells[9];
    ItemSlot result;
};

const Recipe RECIPES[] = {
    {1, 1, {BLOCK_LOG}, {BLOCK_PLANKS, 4}},
    {1, 1, {BLOCK_SPRUCE_LOG}, {BLOCK_SPRUCE_PLANKS, 4}},
    {1, 1, {BLOCK_BIRCH_LOG}, {BLOCK_BIRCH_PLANKS, 4}},
    {2, 2, {BLOCK_PLANKS, BLOCK_PLANKS, BLOCK_PLANKS, BLOCK_PLANKS}, {BLOCK_CRAFTING_TABLE, 1}},
    {2, 2, {BLOCK_STONE, BLOCK_STONE, BLOCK_STONE, BLOCK_STONE}, {BLOCK_STONE_BRICKS, 4}},
    {2, 2, {BLOCK_SAND, BLOCK_SAND, BLOCK_SAND, BLOCK_SAND}, {BLOCK_SANDSTONE, 1}},
    {3, 3, {BLOCK_COBBLE, BLOCK_COBBLE, BLOCK_COBBLE, BLOCK_COBBLE, 0,
            BLOCK_COBBLE, BLOCK_COBBLE, BLOCK_COBBLE, BLOCK_COBBLE}, {BLOCK_FURNACE, 1}},
};

bool IsCraftCell(int index, bool table) {
    return table || (index % 3 < 2 && index / 3 < 2);
}

int InsertStack(ItemSlot* slots, int begin, int end, uint8_t id, int count) {
    for (int pass = 0; pass < 2 && count > 0; ++pass) {
        for (int i = begin; i < end && count > 0; ++i) {
            ItemSlot& slot = slots[i];
            if (pass == 0 ? (!slot.Empty() && slot.id == id) : slot.Empty()) {
                const int moved = std::min(64 - (slot.Empty() ? 0 : slot.count), count);
                slot = {id, (slot.Empty() ? 0 : slot.count) + moved};
                count -= moved;
            }
        }
    }
    return count;
}
}

int InventoryState::Capacity(uint8_t id) const {
    if (id == BLOCK_AIR) return 0;
    int available = 0;
    for (const ItemSlot& slot : slots) {
        if (slot.Empty()) available += 64;
        else if (slot.id == id) available += 64 - slot.count;
    }
    return available;
}

int InventoryState::Add(uint8_t id, int count) {
    if (id == BLOCK_AIR || count <= 0) return count;
    // Merge throughout the inventory before filling empty hotbar slots.
    for (ItemSlot& slot : slots) {
        if (!slot.Empty() && slot.id == id) {
            const int moved = std::min(64 - slot.count, count);
            slot.count += moved;
            count -= moved;
        }
    }
    count = InsertStack(slots, 27, 36, id, count);
    return InsertStack(slots, 0, 27, id, count);
}

ItemSlot InventoryState::Result(bool table) const {
    int minRow = 3, maxRow = -1, minCol = 3, maxCol = -1;
    for (int i = 0; i < 9; ++i) {
        if (craft[i].Empty()) continue;
        if (!IsCraftCell(i, table)) return {};
        minRow = std::min(minRow, i / 3); maxRow = std::max(maxRow, i / 3);
        minCol = std::min(minCol, i % 3); maxCol = std::max(maxCol, i % 3);
    }
    if (maxRow < 0) return {};
    const int width = maxCol - minCol + 1, height = maxRow - minRow + 1;
    for (const Recipe& recipe : RECIPES) {
        if (width != recipe.width || height != recipe.height) continue;
        bool matches = true;
        for (int r = 0; r < height; ++r) {
            for (int c = 0; c < width; ++c) {
                const ItemSlot& slot = craft[(minRow + r) * 3 + minCol + c];
                const uint8_t actual = slot.Empty() ? uint8_t(BLOCK_AIR) : slot.id;
                const uint8_t expected = recipe.cells[r * width + c];
                const bool plankTag = recipe.result.id == BLOCK_CRAFTING_TABLE &&
                    (actual == BLOCK_PLANKS || actual == BLOCK_SPRUCE_PLANKS || actual == BLOCK_BIRCH_PLANKS);
                if (actual != expected && !plankTag) matches = false;
            }
        }
        if (matches) return recipe.result;
    }
    return {};
}

bool InventoryState::Click(int index, bool right, bool table, bool shift) {
    if (index < 0 || index > 45) return false;
    if (index >= 36 && index < 45 && !IsCraftCell(index - 36, table)) return false;
    if (index == 45) {
        bool crafted = false;
        const ItemSlot initial = Result(table);
        for (;;) {
            const ItemSlot result = Result(table);
            if (result.Empty() || result.id != initial.id) break;
            if (shift) {
                if (Capacity(result.id) < result.count) break;
                Add(result.id, result.count);
            } else {
                if (!cursor.Empty() && (cursor.id != result.id || cursor.count + result.count > 64)) break;
                cursor = {result.id, (cursor.Empty() ? 0 : cursor.count) + result.count};
            }
            for (ItemSlot& slot : craft) {
                if (!slot.Empty() && --slot.count == 0) slot = {};
            }
            crafted = true;
            if (!shift) break;
        }
        return crafted;
    }
    ItemSlot& slot = index < 36 ? slots[index] : craft[index - 36];
    if (shift) {
        if (slot.Empty()) return false;
        const int before = slot.count;
        if (index >= 36) slot.count = Add(slot.id, slot.count);
        else slot.count = InsertStack(slots, index < 27 ? 27 : 0, index < 27 ? 36 : 27, slot.id, slot.count);
        const bool changed = slot.count != before;
        if (slot.count == 0) slot = {};
        return changed;
    }
    if (cursor.Empty()) {
        if (slot.Empty()) return false;
        const int moved = right ? (slot.count + 1) / 2 : slot.count;
        cursor = {slot.id, moved};
        slot.count -= moved;
        if (slot.count == 0) slot = {};
    } else if (slot.Empty() || slot.id == cursor.id) {
        const int moved = std::min(right ? 1 : cursor.count, 64 - (slot.Empty() ? 0 : slot.count));
        if (moved == 0) return false;
        slot = {cursor.id, (slot.Empty() ? 0 : slot.count) + moved};
        cursor.count -= moved;
        if (cursor.count == 0) cursor = {};
    } else {
        std::swap(slot, cursor);
    }
    return true;
}

bool InventoryState::Close() {
    InventoryState returned = *this;
    if (!returned.cursor.Empty() && returned.Add(returned.cursor.id, returned.cursor.count) != 0) return false;
    for (const ItemSlot& slot : returned.craft) {
        if (!slot.Empty() && returned.Add(slot.id, slot.count) != 0) return false;
    }
    returned.cursor = {};
    for (ItemSlot& slot : returned.craft) slot = {};
    *this = returned;
    return true;
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