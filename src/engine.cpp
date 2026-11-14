#include "engine.h"
#include "assets.h"
#include "registry.h"
#include "json.hpp"
#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace potato {

KeyBinds gBinds;

void WorldClock::Set(uint64_t ticks) {
    mTicks = ticks;
    mFraction = mCompensation = 0;
}

void WorldClock::Advance(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) return;
    const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    const double elapsed = seconds * 20.0;
    if (elapsed >= double(maximum)) { Set(maximum); return; }
    double whole;
    const double part = std::modf(elapsed, &whole);
    const double adjusted = part - mCompensation;
    double sum = mFraction + adjusted;
    mCompensation = (sum - mFraction) - adjusted;
    // Normalize roundoff at a tick boundary, not small fractional advances.
    const double boundary = std::round(sum);
    if (boundary >= 1 && std::abs(sum - boundary) <= 4 * std::numeric_limits<double>::epsilon()) {
        sum = boundary;
        mCompensation = 0;
    }
    const double carry = std::floor(sum);
    const uint64_t integral = uint64_t(whole);
    const uint64_t extra = uint64_t(carry);
    if (integral > maximum - mTicks || extra > maximum - mTicks - integral) {
        Set(maximum);
        return;
    }
    mTicks += integral + extra;
    mFraction = sum - carry;
}

namespace {
constexpr uint32_t SKY_PERIOD = 24000;
constexpr size_t MAX_SKY_KEYFRAMES = 1024;

bool SkyNumber(const nlohmann::json& value, double& result) {
    if (!value.is_number()) return false;
    result = value.get<double>();
    return std::isfinite(result);
}

bool SkyHexColor(const nlohmann::json& value, DirectX::XMFLOAT4& result) {
    if (!value.is_string()) return false;
    const std::string& text = value.get_ref<const std::string&>();
    if ((text.size() != 7 && text.size() != 9) || text[0] != '#') return false;
    uint32_t packed = 0;
    for (size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        packed = (packed << 4) | uint32_t(digit);
    }
    result = DirectX::XMFLOAT4(float((packed >> 16) & 255) / 255.0f,
        float((packed >> 8) & 255) / 255.0f, float(packed & 255) / 255.0f,
        text.size() == 9 ? float(packed >> 24) / 255.0f : 1.0f);
    return true;
}

double SkyBezier(double t, double a, double b) {
    const double u = 1.0 - t;
    return 3.0 * u * u * t * a + 3.0 * u * t * t * b + t * t * t;
}
}

bool SkyTimeline::LoadDay(const std::string& json) {
    // Bound both parser allocation/recursion and retained untrusted jar data.
    if (json.empty() || json.size() > 1024 * 1024) return false;
    try {
        const auto root = nlohmann::json::parse(json,
            [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                if (depth > 32) throw std::length_error("sky timeline nesting");
                return true;
            });
        double period;
        if (!root.is_object() || !root.contains("clock") ||
            root["clock"] != "minecraft:overworld" || !root.contains("period_ticks") ||
            !SkyNumber(root["period_ticks"], period) || period != SKY_PERIOD ||
            !root.contains("tracks") || !root["tracks"].is_object()) return false;
        const auto& tracks = root["tracks"];
        const char* names[TrackCount] = {
            "minecraft:visual/sky_color", "minecraft:visual/fog_color",
            "minecraft:visual/sky_light_color", "minecraft:visual/sky_light_factor",
            "minecraft:visual/star_brightness", "minecraft:visual/sunrise_sunset_color",
            "minecraft:visual/sun_angle"
        };
        SkyTimeline candidate;
        for (int i = 0; i < TrackCount; ++i) {
            if (!tracks.contains(names[i])) return false;
            const auto& source = tracks[names[i]];
            if (!source.is_object() || !source.contains("keyframes")) return false;
            const auto& frames = source["keyframes"];
            if (!frames.is_array() || frames.empty() || frames.size() > MAX_SKY_KEYFRAMES)
                return false;
            Track& track = candidate.mTracks[i];
            if (source.contains("ease")) {
                const auto& ease = source["ease"];
                if (ease.is_object() && ease.contains("cubic_bezier")) {
                    const auto& curve = ease["cubic_bezier"];
                    if (!curve.is_array() || curve.size() != 4) return false;
                    for (int j = 0; j < 4; ++j) {
                        if (!SkyNumber(curve[j], track.bezier[j]) ||
                            track.bezier[j] < 0 || track.bezier[j] > 1) return false;
                    }
                    track.eased = true;
                } else if (ease != "linear") return false;
            }
            track.keyframes.reserve(frames.size());
            for (const auto& frame : frames) {
                double tick;
                if (!frame.is_object() || !frame.contains("ticks") || !frame.contains("value") ||
                    !SkyNumber(frame["ticks"], tick) || tick < 0 || tick >= SKY_PERIOD ||
                    std::floor(tick) != tick) return false;
                Keyframe key;
                key.ticks = uint32_t(tick);
                if (i == SkyColor || i == FogColor || i == LightColor || i == Twilight) {
                    if (!SkyHexColor(frame["value"], key.value)) return false;
                } else {
                    double value;
                    if (!SkyNumber(frame["value"], value) ||
                        std::abs(value) > (std::numeric_limits<float>::max)()) return false;
                    if ((i == Daylight && (value < 0 || value > 1)) ||
                        (i == Stars && (value < 0 || value > 0.5))) return false;
                    key.value = DirectX::XMFLOAT4(float(value), 0, 0, 0);
                }
                track.keyframes.push_back(key);
            }
            // Equal-time endpoints encode one full revolution, not a zero-length edge.
            track.angleWrap = i == SunAngle && track.keyframes.size() == 2 &&
                track.keyframes[0].ticks == 6000 && track.keyframes[1].ticks == 6000 &&
                frames[0]["value"] == 360.0 && frames[1]["value"] == 0.0;
            if (!track.angleWrap) {
                for (size_t j = 1; j < track.keyframes.size(); ++j)
                    if (track.keyframes[j].ticks <= track.keyframes[j - 1].ticks) return false;
            }
        }
        candidate.mLoaded = true;
        *this = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

DirectX::XMFLOAT4 SkyTimeline::Track::Sample(double time) const {
    if (keyframes.size() == 1) return keyframes.front().value;
    const Keyframe* a;
    const Keyframe* b;
    double progress;
    if (angleWrap) {
        a = &keyframes[1];
        b = &keyframes[0];
        progress = std::fmod(time - a->ticks + SKY_PERIOD, double(SKY_PERIOD)) / SKY_PERIOD;
    } else {
        const auto next = std::upper_bound(keyframes.begin(), keyframes.end(), time,
            [](double t, const Keyframe& key) { return t < key.ticks; });
        b = next == keyframes.end() ? &keyframes.front() : &*next;
        a = next == keyframes.begin() ? &keyframes.back() : &*(next - 1);
        double duration = double(b->ticks) - a->ticks;
        double elapsed = time - a->ticks;
        if (duration <= 0) duration += SKY_PERIOD;
        if (elapsed < 0) elapsed += SKY_PERIOD;
        progress = elapsed / duration;
    }
    if (eased && progress > 0 && progress < 1) {
        double lo = 0, hi = 1;
        for (int iteration = 0; iteration < 48; ++iteration) {
            const double mid = (lo + hi) * 0.5;
            if (SkyBezier(mid, bezier[0], bezier[2]) < progress) lo = mid;
            else hi = mid;
        }
        progress = SkyBezier((lo + hi) * 0.5, bezier[1], bezier[3]);
    }
    auto lerp = [progress](float from, float to) {
        return float(double(from) + (double(to) - from) * progress);
    };
    return DirectX::XMFLOAT4(lerp(a->value.x, b->value.x), lerp(a->value.y, b->value.y),
        lerp(a->value.z, b->value.z), lerp(a->value.w, b->value.w));
}

SkyState SkyTimeline::Sample(uint64_t ticks, double fraction) const {
    SkyState state;
    state.moonPhase = int((ticks / SKY_PERIOD) % 8);
    if (!mLoaded) return state;
    // Reduce integer time before converting to double, including beyond 2^53.
    if (!std::isfinite(fraction)) fraction = 0;
    double time = std::fmod(double(ticks % SKY_PERIOD) +
        std::fmod(fraction, double(SKY_PERIOD)), double(SKY_PERIOD));
    if (time < 0) time += SKY_PERIOD;
    const auto sky = mTracks[SkyColor].Sample(time);
    const auto fog = mTracks[FogColor].Sample(time);
    const auto light = mTracks[LightColor].Sample(time);
    state.skyColor = DirectX::XMFLOAT3(0.47f * sky.x, 0.65f * sky.y, sky.z);
    state.fogColor = DirectX::XMFLOAT3(0.47f * fog.x, 0.65f * fog.y, fog.z);
    state.lightColor = DirectX::XMFLOAT3(light.x, light.y, light.z);
    state.daylight = mTracks[Daylight].Sample(time).x;
    state.stars = mTracks[Stars].Sample(time).x;
    state.twilight = mTracks[Twilight].Sample(time);
    state.sunAngle = mTracks[SunAngle].Sample(time).x * (DirectX::XM_PI / 180.0f);
    return state;
}

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
    if (in.keys[gBinds.forward]) { mx += sy; mz -= cy; }
    if (in.keys[gBinds.back]) { mx -= sy; mz += cy; }
    if (in.keys[gBinds.right]) { mx += cy; mz += sy; }
    if (in.keys[gBinds.left]) { mx -= cy; mz -= sy; }
    if (in.keys[gBinds.jump]) my += 1.0f;
    if (in.keys[gBinds.sneak]) my -= 1.0f;
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
    if (in.keys[gBinds.forward]) { mx += sy; mz -= cy; }
    if (in.keys[gBinds.back]) { mx -= sy; mz += cy; }
    if (in.keys[gBinds.right]) { mx += cy; mz += sy; }
    if (in.keys[gBinds.left]) { mx -= cy; mz -= sy; }
    const float len = sqrtf(mx * mx + mz * mz);
    if (len > 0.0f) { mx /= len; mz /= len; }
    sneaking = !flying && in.keys[gBinds.sneak];
    sprinting = !flying && !sneaking && (in.keys[gBinds.sprint] || sprintLatch) &&
                in.keys[gBinds.forward];
    const float speed = flying ? 12.0f : (sneaking ? 1.31f : (sprinting ? 5.612f : walkSpeed));
    vx = mx * speed + impulseX;
    vz = mz * speed + impulseZ;
    const float drag = expf(-8.0f * dt);
    impulseX *= drag; impulseZ *= drag;

    const uint8_t feet = getBlock(int(floorf(x)), int(floorf(y + 0.2f)), int(floorf(z)));
    inWater = (BLOCK_FLAGS[feet] & BF_LIQUID) != 0;

    if (flying) {
        vy = ((in.keys[gBinds.jump] ? 1.0f : 0.0f) - (in.keys[gBinds.sneak] ? 1.0f : 0.0f)) * speed;
    } else if (inWater) {
        vy -= 8.0f * dt;
        if (vy < -4.0f) vy = -4.0f;
        if (in.keys[gBinds.jump]) vy = 4.0f;
    } else {
        vy -= 32.0f * dt;
        if (vy < -50.0f) vy = -50.0f;
        if (onGround && in.keys[gBinds.jump]) { vy = 8.94f; onGround = false; }
    }

    // Elytra: holding space while airborne steers along the look vector.
    gliding = false;
    if (elytraEquipped && !flying && !inWater && !onGround && in.keys[gBinds.jump] && vy < 1.0f) {
        gliding = true;
        const float cp = cosf(cam.pitch);
        const float gx = sy * cp, gy = sinf(cam.pitch), gz = -cy * cp;
        float spd = sqrtf(vx * vx + vy * vy + vz * vz) + 7.0f * dt;
        if (spd < 7.0f) spd = 7.0f;
        if (spd > 22.0f) spd = 22.0f;
        vx = gx * spd; vy = gy * spd; vz = gz * spd;
        if (vy < -30.0f) vy = -30.0f;
        if (vy > 8.0f) vy = 8.0f;
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

static ToolKind EffectiveTool(uint8_t b) {
    if (b == BLOCK_STONE || b == BLOCK_COBBLE || (b >= BLOCK_COAL_ORE && b <= BLOCK_POLISHED_ANDESITE) ||
        b == BLOCK_SANDSTONE || b == BLOCK_SMOOTH_STONE || b == BLOCK_STONE_BRICKS ||
        b == BLOCK_MOSSY_COBBLE || b == BLOCK_OBSIDIAN || b == BLOCK_FURNACE || b == BLOCK_BRICK || b == BLOCK_ICE)
        return ToolKind::Pickaxe;
    if ((b >= BLOCK_DEEPSLATE && b <= BLOCK_QUARTZ_BLOCK && b != BLOCK_SOUL_SAND && b != BLOCK_GLOWSTONE) ||
        b == BLOCK_TERRACOTTA || b == BLOCK_AMETHYST_BLOCK || b == BLOCK_DRIPSTONE_BLOCK || b == BLOCK_PURPUR_BLOCK)
        return ToolKind::Pickaxe;
    if ((b >= BLOCK_MOSSY_STONE_BRICKS && b <= BLOCK_CHISELED_STONE_BRICKS) || b == BLOCK_RED_SANDSTONE ||
        (b >= BLOCK_PRISMARINE && b <= BLOCK_DARK_PRISMARINE) || (b >= BLOCK_END_STONE && b <= BLOCK_PURPUR_PILLAR) ||
        b == BLOCK_CRYING_OBSIDIAN || b == BLOCK_ANCIENT_DEBRIS || b == BLOCK_BONE_BLOCK)
        return ToolKind::Pickaxe;
    if (b == BLOCK_PLANKS || b == BLOCK_LOG || (b >= BLOCK_SPRUCE_PLANKS && b <= BLOCK_BIRCH_LOG) ||
        b == BLOCK_CRAFTING_TABLE || b == BLOCK_BOOKSHELF) return ToolKind::Axe;
    if (b == BLOCK_DIRT || b == BLOCK_GRASS || b == BLOCK_SAND || b == BLOCK_GRAVEL ||
        b == BLOCK_CLAY || b == BLOCK_SNOW || b == BLOCK_SOUL_SAND || b == BLOCK_RED_SAND) return ToolKind::Shovel;
    return ToolKind::None;
}

float BlockHardness(uint8_t b) {
    if (b == BLOCK_BEDROCK || b == BLOCK_WATER || b == BLOCK_LAVA || b == BLOCK_PORTAL) return -1;
    if (b == BLOCK_AIR || b == BLOCK_TNT) return 0;
    if (b == BLOCK_OBSIDIAN) return 50;
    if (b >= BLOCK_COAL_ORE && b <= BLOCK_LAPIS_ORE) return 3;
    if (b == BLOCK_FURNACE) return 3.5f;
    if (b == BLOCK_CRAFTING_TABLE) return 2.5f;
    if (b == BLOCK_LOG || b == BLOCK_SPRUCE_LOG || b == BLOCK_BIRCH_LOG || b == BLOCK_COBBLE ||
        b == BLOCK_MOSSY_COBBLE || b == BLOCK_PLANKS || b == BLOCK_SPRUCE_PLANKS || b == BLOCK_BIRCH_PLANKS) return 2;
    if (b == BLOCK_STONE || (b >= BLOCK_GRANITE && b <= BLOCK_POLISHED_ANDESITE) ||
        b == BLOCK_BRICK || b == BLOCK_STONE_BRICKS || b == BLOCK_SMOOTH_STONE || b == BLOCK_BOOKSHELF) return 1.5f;
    if (b == BLOCK_SANDSTONE || (b >= BLOCK_WHITE_WOOL && b <= BLOCK_BLACK_WOOL)) return 0.8f;
    if (b == BLOCK_GRASS || b == BLOCK_GRAVEL || b == BLOCK_CLAY) return 0.6f;
    if (b == BLOCK_GLASS) return 0.3f;
    if (b >= BLOCK_OAK_LEAVES && b <= BLOCK_BIRCH_LEAVES) return 0.2f;
    if (b == BLOCK_SNOW) return 0.2f;
    if (b == BLOCK_DEEPSLATE) return 3;
    if (b >= BLOCK_COBBLED_DEEPSLATE && b <= BLOCK_DEEPSLATE_TILES) return 3.5f;
    if (b >= BLOCK_DEEPSLATE_COAL_ORE && b <= BLOCK_DEEPSLATE_COPPER_ORE) return 4.5f;
    if (b == BLOCK_COPPER_ORE || b == BLOCK_EMERALD_ORE) return 3;
    if (b == BLOCK_NETHERITE_BLOCK) return 50;
    if (b == BLOCK_GOLD_BLOCK || b == BLOCK_LAPIS_BLOCK || b == BLOCK_COPPER_BLOCK) return 3;
    if (b >= BLOCK_COAL_BLOCK && b <= BLOCK_RAW_COPPER_BLOCK) return 5;
    if (b == BLOCK_NETHERRACK) return 0.4f;
    if (b == BLOCK_SOUL_SAND || b == BLOCK_MAGMA) return 0.5f;
    if (b == BLOCK_GLOWSTONE) return 0.3f;
    if (b >= BLOCK_BASALT && b <= BLOCK_SMOOTH_BASALT) return 1.25f;
    if (b == BLOCK_BLACKSTONE) return 1.5f;
    if (b >= BLOCK_POLISHED_BLACKSTONE && b <= BLOCK_CRACKED_NETHER_BRICKS) return 2;
    if (b == BLOCK_QUARTZ_BLOCK) return 0.8f;
    if (b == BLOCK_TERRACOTTA) return 1.25f;
    if (b >= BLOCK_ORANGE_WOOL && b <= BLOCK_LIGHT_GRAY_WOOL) return 0.8f;
    if (b == BLOCK_MOSS_BLOCK) return 0.1f;
    if (b == BLOCK_AMETHYST_BLOCK || b == BLOCK_DRIPSTONE_BLOCK || b == BLOCK_PURPUR_BLOCK) return 1.5f;
    if (b >= BLOCK_MOSSY_STONE_BRICKS && b <= BLOCK_CHISELED_STONE_BRICKS) return 1.5f;
    if (b == BLOCK_RED_SANDSTONE) return 0.8f;
    if (b == BLOCK_RED_SAND) return 0.5f;
    if (b >= BLOCK_PRISMARINE && b <= BLOCK_DARK_PRISMARINE) return 1.5f;
    if (b == BLOCK_SEA_LANTERN) return 0.3f;
    if (b >= BLOCK_END_STONE && b <= BLOCK_END_STONE_BRICKS) return 3;
    if (b == BLOCK_PURPUR_PILLAR) return 1.5f;
    if (b >= BLOCK_NETHER_WART_BLOCK && b <= BLOCK_WARPED_WART_BLOCK) return 1;
    if (b == BLOCK_SHROOMLIGHT) return 1;
    if (b == BLOCK_CRYING_OBSIDIAN) return 50;
    if (b == BLOCK_ANCIENT_DEBRIS) return 30;
    if (b == BLOCK_BONE_BLOCK) return 2;
    if (b == BLOCK_HAY_BLOCK) return 0.5f;
    if (b == BLOCK_MELON) return 1;
    if (b == BLOCK_NETHER_QUARTZ_ORE || b == BLOCK_NETHER_GOLD_ORE) return 3;
    if (b == BLOCK_SOUL_SOIL) return 0.5f;
    if (b == BLOCK_CRIMSON_STEM || b == BLOCK_WARPED_STEM ||
        b == BLOCK_CRIMSON_PLANKS || b == BLOCK_WARPED_PLANKS) return 2;
    if (b == BLOCK_CHERRY_LOG) return 2;
    if (b == BLOCK_CHERRY_LEAVES) return 0.2f;
    if (b == BLOCK_CHERRY_PLANKS) return 2;
    if (b == BLOCK_CHEST) return 2.5f;
    return 0.5f;
}

bool CanHarvest(uint8_t b, uint8_t item) {
    if (BlockHardness(b) < 0) return false;
    if (EffectiveTool(b) != ToolKind::Pickaxe || b == BLOCK_ICE) return true;
    const ToolDef* tool = FindTool(item);
    const int required =
        (b == BLOCK_OBSIDIAN || b == BLOCK_NETHERITE_BLOCK || b == BLOCK_CRYING_OBSIDIAN ||
         b == BLOCK_ANCIENT_DEBRIS) ? 3 :
        (b == BLOCK_DIAMOND_ORE || b == BLOCK_GOLD_ORE || b == BLOCK_REDSTONE_ORE || b == BLOCK_EMERALD_ORE ||
         b == BLOCK_DEEPSLATE_GOLD_ORE || b == BLOCK_DEEPSLATE_DIAMOND_ORE || b == BLOCK_DEEPSLATE_REDSTONE_ORE ||
         b == BLOCK_DEEPSLATE_EMERALD_ORE || b == BLOCK_GOLD_BLOCK || b == BLOCK_DIAMOND_BLOCK ||
         b == BLOCK_EMERALD_BLOCK || b == BLOCK_REDSTONE_BLOCK || b == BLOCK_RAW_GOLD_BLOCK ||
         b == BLOCK_AMETHYST_BLOCK) ? 2 :
        (b == BLOCK_IRON_ORE || b == BLOCK_LAPIS_ORE || b == BLOCK_COPPER_ORE ||
         b == BLOCK_DEEPSLATE_IRON_ORE || b == BLOCK_DEEPSLATE_LAPIS_ORE || b == BLOCK_DEEPSLATE_COPPER_ORE ||
         b == BLOCK_IRON_BLOCK || b == BLOCK_LAPIS_BLOCK || b == BLOCK_COPPER_BLOCK ||
         b == BLOCK_RAW_IRON_BLOCK || b == BLOCK_RAW_COPPER_BLOCK) ? 1 : 0;
    return tool && tool->kind == ToolKind::Pickaxe && tool->tier >= required;
}

float MiningSeconds(uint8_t b, uint8_t item, bool onGround, bool underwater) {
    const float hardness = BlockHardness(b);
    if (hardness < 0) return std::numeric_limits<float>::infinity();
    if (hardness == 0) return 0;
    float speed = 1;
    if (const ToolDef* tool = FindTool(item)) {
        if (tool->kind == EffectiveTool(b)) speed = tool->speed;
        else if (tool->kind == ToolKind::Sword) speed = 1.5f;
    }
    if (!onGround) speed /= 5;
    if (underwater) speed /= 5;
    const float ticks = std::ceil(hardness * (CanHarvest(b, item) ? 30.0f : 100.0f) / speed);
    return ticks / 20.0f;
}

ItemSlot BlockDrops(uint8_t b, uint8_t item) {
    if (!CanHarvest(b, item)) return {};
    LootEntry loot;
    if (LootDropForBlock(b, loot))
        return loot.item == BLOCK_AIR ? ItemSlot{} : ItemSlot{loot.item, loot.count};
    switch (b) {
        case BLOCK_STONE: return {BLOCK_COBBLE, 1};
        case BLOCK_GRASS: return {BLOCK_DIRT, 1};
        case BLOCK_COAL_ORE: return {ITEM_COAL, 1};
        case BLOCK_IRON_ORE: return {ITEM_RAW_IRON, 1};
        case BLOCK_GOLD_ORE: return {ITEM_RAW_GOLD, 1};
        case BLOCK_DIAMOND_ORE: return {ITEM_DIAMOND, 1};
        case BLOCK_REDSTONE_ORE: return {ITEM_REDSTONE, 4};
        case BLOCK_LAPIS_ORE: return {ITEM_LAPIS, 4};
        case BLOCK_DEEPSLATE: return {BLOCK_COBBLED_DEEPSLATE, 1};
        case BLOCK_DEEPSLATE_COAL_ORE: return {ITEM_COAL, 1};
        case BLOCK_DEEPSLATE_IRON_ORE: return {ITEM_RAW_IRON, 1};
        case BLOCK_DEEPSLATE_GOLD_ORE: return {ITEM_RAW_GOLD, 1};
        case BLOCK_DEEPSLATE_DIAMOND_ORE: return {ITEM_DIAMOND, 1};
        case BLOCK_DEEPSLATE_REDSTONE_ORE: return {ITEM_REDSTONE, 4};
        case BLOCK_DEEPSLATE_LAPIS_ORE: return {ITEM_LAPIS, 4};
        case BLOCK_DEEPSLATE_EMERALD_ORE: case BLOCK_EMERALD_ORE: return {ITEM_EMERALD, 1};
        case BLOCK_DEEPSLATE_COPPER_ORE: case BLOCK_COPPER_ORE: return {ITEM_RAW_COPPER, 2};
        case BLOCK_OAK_LEAVES: case BLOCK_SPRUCE_LEAVES: case BLOCK_BIRCH_LEAVES:
        case BLOCK_GLASS: case BLOCK_ICE: case BLOCK_AIR: return {};
        default: return {b, 1};
    }
}

bool WearTool(ItemSlot& slot, int amount) {
    const ToolDef* tool = FindTool(slot.id);
    if (!tool || slot.Empty() || amount <= 0) return false;
    if (int(slot.damage) + amount >= tool->durability) { slot = {}; return true; }
    slot.damage = uint16_t(int(slot.damage) + amount);
    return false;
}

namespace {
bool IsPlank(uint8_t id) {
    return id == BLOCK_PLANKS || id == BLOCK_SPRUCE_PLANKS || id == BLOCK_BIRCH_PLANKS;
}

ItemSlot ToolRecipe(const ItemSlot* craft, int row, int col, int width, int height) {
    struct Shape { int width; const char* pattern; int kind; };
    const Shape shapes[] = {{3, "MMM S  S ", 0}, {2, "MMMS S", 1}, {1, "MSS", 2}, {1, "MMS", 3}};
    const uint8_t materials[] = {BLOCK_PLANKS, BLOCK_COBBLE, ITEM_IRON_INGOT, ITEM_DIAMOND, ITEM_GOLD_INGOT};
    if (height != 3) return {};
    for (const Shape& shape : shapes) {
        if (width != shape.width) continue;
        for (int material = 0; material < 5; ++material) {
            for (int mirrored = 0; mirrored < 2; ++mirrored) {
                bool matches = true;
                for (int r = 0; r < 3; ++r) for (int c = 0; c < width; ++c) {
                    const ItemSlot& slot = craft[(row + r) * 3 + col + c];
                    const uint8_t id = slot.Empty() ? uint8_t(0) : slot.id;
                    const char symbol = shape.pattern[r * width + (mirrored ? width - 1 - c : c)];
                    if (symbol == 'M' ? (material == 0 ? !IsPlank(id) : id != materials[material]) :
                        symbol == 'S' ? id != ITEM_STICK : id != 0) matches = false;
                }
                if (matches) return {uint8_t(ITEM_WOOD_PICKAXE + material * 4 + shape.kind), 1};
            }
        }
    }
    return {};
}

// Built-in fallback used until the jar registry supplies Java's data files.
const CraftingRecipe RECIPES[] = {
    {1, 1, {BLOCK_LOG}, {BLOCK_PLANKS, 4}},
    {1, 1, {BLOCK_SPRUCE_LOG}, {BLOCK_SPRUCE_PLANKS, 4}},
    {1, 1, {BLOCK_BIRCH_LOG}, {BLOCK_BIRCH_PLANKS, 4}},
    {1, 2, {BLOCK_PLANKS, BLOCK_PLANKS}, {ITEM_STICK, 4}},
    {2, 2, {BLOCK_PLANKS, BLOCK_PLANKS, BLOCK_PLANKS, BLOCK_PLANKS}, {BLOCK_CRAFTING_TABLE, 1}},
    {2, 2, {BLOCK_STONE, BLOCK_STONE, BLOCK_STONE, BLOCK_STONE}, {BLOCK_STONE_BRICKS, 4}},
    {2, 2, {BLOCK_SAND, BLOCK_SAND, BLOCK_SAND, BLOCK_SAND}, {BLOCK_SANDSTONE, 1}},
    {3, 3, {BLOCK_COBBLE, BLOCK_COBBLE, BLOCK_COBBLE, BLOCK_COBBLE, 0,
            BLOCK_COBBLE, BLOCK_COBBLE, BLOCK_COBBLE, BLOCK_COBBLE}, {BLOCK_FURNACE, 1}},
};

bool MatchesShaped(const ItemSlot* craft, const CraftingRecipe& recipe,
                   int minRow, int minCol, int width, int height) {
    if (recipe.width != width || recipe.height != height) return false;
    for (int r = 0; r < height; ++r) {
        for (int c = 0; c < width; ++c) {
            const ItemSlot& slot = craft[(minRow + r) * 3 + minCol + c];
            const uint8_t actual = slot.Empty() ? uint8_t(BLOCK_AIR) : slot.id;
            const uint16_t expected = recipe.cells[r * 3 + c];
            if (expected == 0 ? actual != BLOCK_AIR : !RecipeCellMatches(expected, actual)) return false;
        }
    }
    return true;
}

bool MatchesShapeless(const ItemSlot* craft, const CraftingRecipe& recipe, int used) {
    int wanted = 0;
    for (int i = 0; i < 9; ++i) if (recipe.cells[i]) ++wanted;
    if (wanted != used) return false;
    bool taken[9] = {};
    for (int i = 0; i < 9; ++i) {
        if (craft[i].Empty()) continue;
        bool matched = false;
        for (int j = 0; j < 9; ++j) {
            if (taken[j] || !recipe.cells[j]) continue;
            if (RecipeCellMatches(recipe.cells[j], craft[i].id)) {
                taken[j] = true;
                matched = true;
                break;
            }
        }
        if (!matched) return false;
    }
    return true;
}

bool IsCraftCell(int index, bool table) {
    return table || (index % 3 < 2 && index / 3 < 2);
}

int InsertStack(ItemSlot* slots, int begin, int end, uint8_t id, int count, uint16_t damage) {
    const int limit = ItemStackLimit(id);
    for (int pass = 0; pass < 2 && count > 0; ++pass) {
        for (int i = begin; i < end && count > 0; ++i) {
            ItemSlot& slot = slots[i];
            if (pass == 0 ? (!slot.Empty() && slot.id == id && slot.damage == damage) : slot.Empty()) {
                const int moved = std::min(limit - (slot.Empty() ? 0 : slot.count), count);
                slot = {id, (slot.Empty() ? 0 : slot.count) + moved, damage};
                count -= moved;
            }
        }
    }
    return count;
}
}

int InventoryState::Capacity(uint8_t id) const {
    if (id == BLOCK_AIR) return 0;
    const int limit = ItemStackLimit(id);
    int available = 0;
    for (const ItemSlot& slot : slots) {
        if (slot.Empty()) available += limit;
        else if (slot.id == id) available += limit - slot.count;
    }
    return available;
}

int InventoryState::Add(uint8_t id, int count, uint16_t damage) {
    if (id == BLOCK_AIR || count <= 0) return count;
    const int limit = ItemStackLimit(id);
    for (ItemSlot& slot : slots) {
        if (!slot.Empty() && slot.id == id && slot.damage == damage) {
            const int moved = std::min(limit - slot.count, count);
            slot.count += moved;
            count -= moved;
        }
    }
    count = InsertStack(slots, 27, 36, id, count, damage);
    return InsertStack(slots, 0, 27, id, count, damage);
}

ItemSlot InventoryState::Result(bool table) const {
    int minRow = 3, maxRow = -1, minCol = 3, maxCol = -1, used = 0;
    for (int i = 0; i < 9; ++i) {
        if (craft[i].Empty()) continue;
        if (!IsCraftCell(i, table)) return {};
        ++used;
        minRow = std::min(minRow, i / 3); maxRow = std::max(maxRow, i / 3);
        minCol = std::min(minCol, i % 3); maxCol = std::max(maxCol, i % 3);
    }
    if (maxRow < 0) return {};
    const int width = maxCol - minCol + 1, height = maxRow - minRow + 1;
    if (table) {
        const ItemSlot tool = ToolRecipe(craft, minRow, minCol, width, height);
        if (!tool.Empty()) return tool;
    }
    const std::vector<CraftingRecipe>& jar = RegistryRecipes();
    if (!jar.empty()) {
        for (const CraftingRecipe& recipe : jar) {
            if (recipe.shapeless ? MatchesShapeless(craft, recipe, used)
                                 : MatchesShaped(craft, recipe, minRow, minCol, width, height))
                return recipe.result;
        }
        return {};
    }
    for (const CraftingRecipe& recipe : RECIPES) {
        if (width != recipe.width || height != recipe.height) continue;
        bool matches = true;
        for (int r = 0; r < height; ++r) {
            for (int c = 0; c < width; ++c) {
                const ItemSlot& slot = craft[(minRow + r) * 3 + minCol + c];
                const uint8_t actual = slot.Empty() ? uint8_t(BLOCK_AIR) : slot.id;
                const uint8_t expected = uint8_t(recipe.cells[r * width + c]);
                const bool plankTag = (recipe.result.id == BLOCK_CRAFTING_TABLE || recipe.result.id == ITEM_STICK) && IsPlank(actual);
                if (actual != expected && !plankTag) matches = false;
            }
        }
        if (matches) return recipe.result;
    }
    return {};
}

bool InventoryState::Click(int index, bool right, bool table, bool shift) {
    if (index < 0 || index > 50) return false;
    if (index >= 46 && table) return false;
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
                if (!cursor.Empty() && (cursor.id != result.id || cursor.damage != 0 ||
                    cursor.count + result.count > ItemStackLimit(result.id))) break;
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
    if (index >= 46) {
        ItemSlot& slot = index == 50 ? offhand : armor[index - 46];
        if (shift) {
            if (slot.Empty()) return false;
            const int before = slot.count;
            slot.count = Add(slot.id, slot.count, slot.damage);
            if (slot.count == 0) slot = {};
            return slot.count != before;
        }
        if (cursor.Empty()) {
            if (slot.Empty()) return false;
            const int moved = right ? (slot.count + 1) / 2 : slot.count;
            cursor = {slot.id, moved, slot.damage};
            slot.count -= moved;
            if (slot.count == 0) slot = {};
            return true;
        }
        if (index < 50) {
            const ArmorDef* a = FindArmor(cursor.id);
            if (!a || a->slot != index - 46) return false;
        }
        if (slot.Empty() || (slot.id == cursor.id && slot.damage == cursor.damage)) {
            const int moved = std::min(right ? 1 : cursor.count, ItemStackLimit(cursor.id) - (slot.Empty() ? 0 : slot.count));
            if (moved == 0) return false;
            slot = {cursor.id, (slot.Empty() ? 0 : slot.count) + moved, cursor.damage};
            cursor.count -= moved;
            if (cursor.count == 0) cursor = {};
        } else {
            std::swap(slot, cursor);
        }
        return true;
    }
    ItemSlot& slot = index < 36 ? slots[index] : craft[index - 36];
    if (shift) {
        if (slot.Empty()) return false;
        if (index < 36) {
            if (const ArmorDef* a = FindArmor(slot.id)) {
                std::swap(armor[a->slot], slot);
                return true;
            }
        }
        const int before = slot.count;
        if (index >= 36) slot.count = Add(slot.id, slot.count, slot.damage);
        else slot.count = InsertStack(slots, index < 27 ? 27 : 0, index < 27 ? 36 : 27,
                                      slot.id, slot.count, slot.damage);
        const bool changed = slot.count != before;
        if (slot.count == 0) slot = {};
        return changed;
    }
    if (cursor.Empty()) {
        if (slot.Empty()) return false;
        const int moved = right ? (slot.count + 1) / 2 : slot.count;
        cursor = {slot.id, moved, slot.damage};
        slot.count -= moved;
        if (slot.count == 0) slot = {};
    } else if (slot.Empty() || (slot.id == cursor.id && slot.damage == cursor.damage)) {
        const int moved = std::min(right ? 1 : cursor.count, ItemStackLimit(cursor.id) - (slot.Empty() ? 0 : slot.count));
        if (moved == 0) return false;
        slot = {cursor.id, (slot.Empty() ? 0 : slot.count) + moved, cursor.damage};
        cursor.count -= moved;
        if (cursor.count == 0) cursor = {};
    } else {
        std::swap(slot, cursor);
    }
    return true;
}

bool InventoryState::Close() {
    InventoryState returned = *this;
    if (!returned.cursor.Empty() && returned.Add(returned.cursor.id, returned.cursor.count, returned.cursor.damage) != 0) return false;
    for (const ItemSlot& slot : returned.craft) {
        if (!slot.Empty() && returned.Add(slot.id, slot.count, slot.damage) != 0) return false;
    }
    returned.cursor = {};
    for (ItemSlot& slot : returned.craft) slot = {};
    *this = returned;
    return true;
}

int InventoryState::Defense() const {
    int total = 0;
    for (const ItemSlot& s : armor) if (const ArmorDef* a = FindArmor(s.id)) total += a->defense;
    return total;
}

float InventoryState::Toughness() const {
    float total = 0;
    for (const ItemSlot& s : armor) if (const ArmorDef* a = FindArmor(s.id)) total += a->toughness;
    return total;
}

float HurtResistance::Apply(float damage) {
    if (remaining > 0.5f) {
        if (damage <= lastDamage) return 0;
        const float extra = damage - lastDamage;
        lastDamage = damage;
        return extra;
    }
    lastDamage = damage;
    remaining = 1;
    return damage;
}

namespace {
const MobStats kMobStats[int(MobType::Count)] = {
    {"zombie", "Zombie", 20, 1.30f, 1.95f, 0.30f, 3, true, "rotten_flesh", 0, 2, "", 0, 0},
    {"pig", "Pig", 10, 1.00f, 0.90f, 0.45f, 0, false, "porkchop", 1, 3, "", 0, 0},
    {"cow", "Cow", 10, 0.95f, 1.40f, 0.45f, 0, false, "beef", 1, 3, "leather", 0, 2},
    {"sheep", "Sheep", 8, 1.00f, 1.30f, 0.45f, 0, false, "mutton", 1, 2, "white_wool", 1, 1},
    {"chicken", "Chicken", 4, 0.90f, 0.70f, 0.20f, 0, false, "chicken", 1, 1, "feather", 0, 2},
    {"creeper", "Creeper", 20, 1.15f, 1.70f, 0.30f, 0, true, "gunpowder", 0, 2, "", 0, 0},
    {"skeleton", "Skeleton", 20, 1.20f, 1.95f, 0.30f, 2, true, "bone", 0, 2, "arrow", 0, 2},
    {"spider", "Spider", 16, 1.35f, 0.90f, 0.70f, 2, true, "string", 0, 2, "", 0, 0},
    {"enderman", "Enderman", 40, 1.45f, 2.90f, 0.30f, 7, true, "ender_pearl", 0, 1, "", 0, 0},
};
} // namespace

const MobStats& GetMobStats(MobType type) {
    const int index = int(type);
    if (index < 0 || index >= int(MobType::Count)) return kMobStats[0];
    return kMobStats[index];
}

const char* MobId(MobType type) { return GetMobStats(type).id; }
const char* MobName(MobType type) { return GetMobStats(type).name; }

bool ParseMobId(const std::string& name, MobType& type) {
    std::string id = name;
    if (id.rfind("minecraft:", 0) == 0) id = id.substr(10);
    for (char& c : id) c = char(tolower(uint8_t(c)));
    for (int i = 0; i < int(MobType::Count); ++i) {
        if (id == kMobStats[i].id) { type = MobType(i); return true; }
    }
    return false;
}

void Mob::SetType(MobType value) {
    type = value;
    const MobStats& stats = GetMobStats(value);
    health = stats.health;
    passive = !stats.hostile;
    body.walkSpeed = stats.speed;
    body.flying = false;
    fuse = -1;
    attackDelay = 0;
    wanderTimer = 0;
}

void Mob::Update(float dt, const PlayerBody& player, const std::function<uint8_t(int, int, int)>& getBlock) {
    resistance.Advance(dt);
    attackDelay = std::max(0.0f, attackDelay - dt);
    if (health <= 0) return;
    InputState input;
    FlyCamera camera;
    const float dx = player.x - body.x, dz = player.z - body.z;
    const float distance2 = dx * dx + dz * dz;
    const MobStats& stats = GetMobStats(type);
    if (passive) {
        // Java-pig-style calm wander: pick a heading every few seconds and
        // flee briefly after taking damage.
        wanderTimer -= dt;
        if (wanderTimer <= 0) {
            wanderTimer = 2.0f + float(int(body.x * 7 + body.z * 13) & 7) * 0.5f;
            wanderYaw = float(int(body.z * 31 - body.x * 17) % 628) * 0.01f;
            input.keys['W'] = (int(body.x + body.z) & 3) != 0;
        }
        if (resistance.remaining > 1.0f && distance2 < 16 * 16) {
            yaw = atan2f(-dx, dz);
            input.keys['W'] = true;
        } else {
            yaw = wanderYaw;
        }
        if (input.keys['W'] && body.onGround &&
            BodyHits(getBlock, body.x + sinf(yaw) * 0.5f, body.y, body.z - cosf(yaw) * 0.5f))
            input.keys[VK_SPACE] = true;
    } else {
        yaw = atan2f(dx, -dz);
        const float reach = type == MobType::Skeleton ? 14.0f : 32.0f;
        input.keys['W'] = distance2 > 1.5f * 1.5f && distance2 < reach * reach;
        if (input.keys['W'] && body.onGround && BodyHits(getBlock, body.x + sinf(yaw) * 0.5f, body.y,
                                                       body.z - cosf(yaw) * 0.5f)) input.keys[VK_SPACE] = true;
        if (type == MobType::Creeper) {
            if (distance2 < 3.0f * 3.0f) {
                fuse = fuse < 0 ? 1.5f : fuse - dt;
            } else if (fuse > 0) {
                fuse = std::min(1.5f, fuse + dt * 2.0f);
            }
        }
    }
    (void)stats;
    camera.yaw = yaw;
    body.Update(input, dt, camera, getBlock);
}

float Mob::Hit(float damage, float knockX, float knockZ) {
    if (health <= 0) return 0;
    const float applied = resistance.Apply(damage);
    if (applied <= 0) return 0;
    health = std::max(0.0f, health - applied);
    body.impulseX = knockX; body.impulseZ = knockZ;
    body.vy = 4;
    body.onGround = false;
    return applied;
}

bool RayHitsBody(const FlyCamera& camera, const PlayerBody& body, float reach, float& distance) {
    const float origin[3] = {camera.x, camera.y, camera.z};
    const float direction[3] = {camera.fwdX, camera.fwdY, camera.fwdZ};
    const float lo[3] = {body.x - PlayerBody::HALF, body.y, body.z - PlayerBody::HALF};
    const float hi[3] = {body.x + PlayerBody::HALF, body.y + 1.95f, body.z + PlayerBody::HALF};
    float nearHit = 0, farHit = reach;
    for (int i = 0; i < 3; ++i) {
        if (fabsf(direction[i]) < 1e-6f) {
            if (origin[i] < lo[i] || origin[i] > hi[i]) return false;
        } else {
            float a = (lo[i] - origin[i]) / direction[i], b = (hi[i] - origin[i]) / direction[i];
            if (a > b) std::swap(a, b);
            nearHit = std::max(nearHit, a); farHit = std::min(farHit, b);
            if (nearHit > farHit) return false;
        }
    }
    distance = nearHit;
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

void ParticleSystem::SpawnBubbles(float x, float y, float z) {
    std::uniform_real_distribution<float> spread(-0.10f, 0.10f);
    std::uniform_real_distribution<float> rise(0.6f, 1.4f);
    std::uniform_real_distribution<float> drift(-0.15f, 0.15f);
    std::uniform_real_distribution<float> life(0.7f, 1.2f);
    for (int i = 0; i < 3; ++i) {
        if (mParticles.size() >= MAX_PARTICLES) return;
        Particle p;
        p.x = x + spread(mRng); p.y = y + spread(mRng) * 0.5f; p.z = z + spread(mRng);
        p.vx = drift(mRng); p.vz = drift(mRng);
        p.vy = rise(mRng);
        p.maxLife = p.life = life(mRng);
        p.bubble = true;
        mParticles.push_back(p);
    }
}

void ParticleSystem::SpawnRain(float x, float y, float z, bool snow) {
    if (mParticles.size() >= MAX_PARTICLES) return;
    Particle p;
    p.x = x; p.y = y; p.z = z;
    if (snow) {
        p.vx = 0.35f; p.vy = -1.4f; p.vz = 0.15f;
        p.maxLife = p.life = 2.4f;
        p.snow = true;
    } else {
        p.vx = 0.5f; p.vy = -13.0f; p.vz = 0.2f;
        p.maxLife = p.life = 0.9f;
        p.rain = true;
    }
    mParticles.push_back(p);
}

void ParticleSystem::Update(float dt) {
    size_t w = 0;
    for (size_t i = 0; i < mParticles.size(); ++i) {
        Particle& p = mParticles[i];
        p.life -= dt;
        if (p.life <= 0.0f) continue;
        if (p.bubble) {
            p.vy = std::min(p.vy + 2.5f * dt, 1.6f);
            p.vx *= 0.96f;
            p.vz *= 0.96f;
        } else if (p.rain) {
            // Terminal velocity: rain does not keep accelerating.
        } else if (p.snow) {
            p.vx *= 0.99f;
            p.vz *= 0.99f;
        } else {
            p.vy -= 9.8f * dt;
        }
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.z += p.vz * dt;
        mParticles[w++] = p;
    }
    mParticles.resize(w);
}

} // namespace potato