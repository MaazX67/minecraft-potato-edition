#include "assets.h"
#include "engine.h"
#include "registry.h"
#include "commands.h"
#include "world.h"
#include "shader_embed.h"
#include "json.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <thread>
#include <vector>

using namespace potato;

static int gFail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); } \
    else { printf("FAIL: %s\n", msg); ++gFail; } \
} while (0)

static void TestShaders() {
    struct Shader { const char* entry; const char* profile; };
    const Shader shaders[] = {
        {"VSMain", "vs_4_0"}, {"PSMain", "ps_4_0"},
        {"PSMainUI", "ps_4_0"}, {"PSMainSky", "ps_4_0"},
        {"PSMainWorldSky", "ps_4_0"}, {"PSMainCelestial", "ps_4_0"}, {"PSMainEntity", "ps_4_0"},
        {"PSMainCrack", "ps_4_0"}, {"PSMainWater", "ps_4_0"}
    };
    for (const Shader& shader : shaders) {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT result = D3DCompile(gShaderSource, strlen(gShaderSource), "shader.fx",
            nullptr, nullptr, shader.entry, shader.profile, D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &code, &errors);
        if (FAILED(result) && errors)
            fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
        char label[128];
        snprintf(label, sizeof(label), "embedded %s compiles as %s", shader.entry, shader.profile);
        CHECK(SUCCEEDED(result) && code && code->GetBufferSize() > 0, label);
        if (errors) errors->Release();
        if (code) code->Release();
    }
}

static void TestSkyTimeline() {
    auto closeEnough = [](double a, double b) { return std::abs(a - b) < 1e-5; };
    SkyTimeline timeline;
    const SkyState fallback = timeline.Sample(3 * uint64_t(24000));
    CHECK(!timeline.Loaded() && closeEnough(fallback.skyColor.x, 0.47) &&
          closeEnough(fallback.skyColor.y, 0.65) && closeEnough(fallback.skyColor.z, 1) &&
          closeEnough(fallback.fogColor.x, 0.47) && closeEnough(fallback.fogColor.y, 0.65) &&
          closeEnough(fallback.fogColor.z, 1) && closeEnough(fallback.lightColor.x, 1) &&
          closeEnough(fallback.lightColor.y, 1) && closeEnough(fallback.lightColor.z, 1) &&
          fallback.daylight == 1 && fallback.stars == 0 && fallback.sunAngle == 0 &&
          fallback.twilight.w == 0 && fallback.moonPhase == 3,
          "missing sky timeline returns daylight defaults and the current moon phase");
    CHECK(!timeline.LoadDay("{}") && !timeline.Loaded(), "incomplete initial timeline stays unloaded");

    // Deliberately small invented tracks and easing, not installed game content.
    const std::string synthetic = R"json({
      "clock": "minecraft:overworld", "period_ticks": 24000,
      "tracks": {
        "minecraft:visual/sky_color": {"keyframes": [
          {"ticks": 2000, "value": "#000000"}, {"ticks": 14000, "value": "#FFFFFF"}]},
        "minecraft:visual/fog_color": {"keyframes": [
          {"ticks": 2000, "value": "#ff0000"}, {"ticks": 14000, "value": "#0000ff"}]},
        "minecraft:visual/sky_light_color": {"keyframes": [
          {"ticks": 500, "value": "#abcdef"}]},
        "minecraft:visual/sky_light_factor": {"keyframes": [
          {"ticks": 2000, "value": 0.2}, {"ticks": 14000, "value": 0.8}]},
        "minecraft:visual/star_brightness": {"keyframes": [
          {"ticks": 2000, "value": 0.0}, {"ticks": 14000, "value": 0.5}]},
        "minecraft:visual/sunrise_sunset_color": {"keyframes": [
          {"ticks": 2000, "value": "#00000000"}, {"ticks": 14000, "value": "#80402010"}]},
        "minecraft:visual/sun_angle": {"ease": {"cubic_bezier": [0.2, 0.1, 0.8, 0.9]},
          "keyframes": [{"ticks": 6000, "value": 360}, {"ticks": 6000, "value": 0}]}
      }
    })json";
    CHECK(timeline.LoadDay(synthetic) && timeline.Loaded(), "complete synthetic sky timeline loads");
    const SkyState middle = timeline.Sample(8000);
    CHECK(closeEnough(middle.skyColor.x, 0.47 * 0.5) && closeEnough(middle.skyColor.y, 0.65 * 0.5) &&
          closeEnough(middle.skyColor.z, 0.5) && closeEnough(middle.daylight, 0.5) && closeEnough(middle.stars, 0.25),
          "sky RGB multiplier, daylight and actual star brightness interpolate linearly");
    CHECK(closeEnough(middle.fogColor.x, 0.47 * 0.5) && middle.fogColor.y == 0 &&
          closeEnough(middle.fogColor.z, 0.5) && closeEnough(middle.lightColor.x, 171.0 / 255) &&
          closeEnough(middle.lightColor.y, 205.0 / 255) && closeEnough(middle.lightColor.z, 239.0 / 255),
          "fog uses the sky base and single-keyframe light RGB remains constant");
    CHECK(closeEnough(middle.twilight.x, 32.0 / 255) && closeEnough(middle.twilight.y, 16.0 / 255) &&
          closeEnough(middle.twilight.z, 8.0 / 255) && closeEnough(middle.twilight.w, 64.0 / 255),
          "AARRGGBB twilight decodes and interpolates all four channels");
    const SkyState wrap = timeline.Sample(20000);
    CHECK(closeEnough(wrap.skyColor.z, 0.5) && closeEnough(wrap.daylight, 0.5) &&
          closeEnough(wrap.stars, 0.25) && closeEnough(wrap.twilight.w, 64.0 / 255) &&
          closeEnough(timeline.Sample(0).skyColor.z, 1.0 / 6),
          "last-to-first interpolation crosses midnight instead of clamping");
    CHECK(timeline.Sample(2000).skyColor.z == 0 && timeline.Sample(14000).skyColor.z == 1 &&
          timeline.Sample(14000).stars == 0.5f && timeline.Sample(2000).stars == 0,
          "exact color and brightness keyframes include the full 0..0.5 star range");
    CHECK(closeEnough(timeline.Sample(2000, 0.5).skyColor.z, 0.5 / 12000) &&
          closeEnough(timeline.Sample(23999, 1).skyColor.z, timeline.Sample(0).skyColor.z),
          "fractional ticks interpolate and roll over the period boundary");
    CHECK(timeline.Sample(6000).sunAngle == 0 && closeEnough(timeline.Sample(18000).sunAngle, DirectX::XM_PI),
          "duplicate sun endpoints wrap to zero at 6000 and pi at 18000");
    // For t=0.25 these invented controls give x=0.2125 and y=0.184375.
    CHECK(closeEnough(timeline.Sample(11100).sunAngle, 0.184375 * DirectX::XM_2PI),
          "cubic easing solves Bezier x before evaluating y using input controls");
    bool monotonic = true;
    float previous = -1;
    for (uint64_t step = 0; step < 24000; step += 37) {
        const float angle = timeline.Sample(6000 + step).sunAngle;
        if (angle < previous || angle < 0 || angle >= DirectX::XM_2PI) monotonic = false;
        previous = angle;
    }
    CHECK(monotonic && timeline.Sample(5999).sunAngle > 6.0f,
          "eased sun angle is monotonic through one revolution and wraps at noon");
    bool phases = true;
    for (uint64_t day = 0; day < 16; ++day) {
        if (timeline.Sample(day * 24000).moonPhase != int(day % 8) ||
            SkyTimeline().Sample(day * 24000 + 23999).moonPhase != int(day % 8)) phases = false;
    }
    CHECK(phases, "loaded and fallback skies cycle through all eight moon phases");

    auto sameVisual = [closeEnough](const SkyState& a, const SkyState& b) {
        return closeEnough(a.skyColor.x, b.skyColor.x) && closeEnough(a.skyColor.y, b.skyColor.y) &&
            closeEnough(a.skyColor.z, b.skyColor.z) && closeEnough(a.fogColor.x, b.fogColor.x) &&
            closeEnough(a.fogColor.y, b.fogColor.y) && closeEnough(a.fogColor.z, b.fogColor.z) &&
            closeEnough(a.lightColor.x, b.lightColor.x) && closeEnough(a.lightColor.y, b.lightColor.y) &&
            closeEnough(a.lightColor.z, b.lightColor.z) && closeEnough(a.daylight, b.daylight) &&
            closeEnough(a.stars, b.stars) && closeEnough(a.twilight.x, b.twilight.x) &&
            closeEnough(a.twilight.y, b.twilight.y) && closeEnough(a.twilight.z, b.twilight.z) &&
            closeEnough(a.twilight.w, b.twilight.w) && closeEnough(a.sunAngle, b.sunAngle);
    };
    const uint64_t largeTicks[] = {uint64_t(1) << 53, (uint64_t(1) << 60) + 17,
                                  (std::numeric_limits<uint64_t>::max)()};
    bool largeModulo = true;
    for (uint64_t ticks : largeTicks) {
        const SkyState large = timeline.Sample(ticks, 0.375);
        if (!sameVisual(large, timeline.Sample(ticks % 24000, 0.375)) ||
            large.moonPhase != int((ticks / 24000) % 8)) largeModulo = false;
    }
    CHECK(largeModulo, "uint64 time above double precision retains exact day modulo and moon phase");

    const auto original = nlohmann::json::parse(synthetic);
    std::vector<std::string> invalid = {"{", "{}", "null", "[]"};
    auto mutation = [&](const std::function<void(nlohmann::json&)>& change) {
        auto bad = original;
        change(bad);
        invalid.push_back(bad.dump());
    };
    mutation([](nlohmann::json& j) { j["clock"] = "minecraft:other"; });
    mutation([](nlohmann::json& j) { j["period_ticks"] = 23999; });
    const char* required[] = {"sky_color", "fog_color", "sky_light_color", "sky_light_factor",
                             "star_brightness", "sunrise_sunset_color", "sun_angle"};
    for (const char* track : required) {
        mutation([track](nlohmann::json& j) { j["tracks"].erase(std::string("minecraft:visual/") + track); });
    }
    mutation([](nlohmann::json& j) { j["tracks"]["minecraft:visual/sky_color"]["keyframes"] = nlohmann::json::array(); });
    mutation([](nlohmann::json& j) { j["tracks"]["minecraft:visual/sky_color"]["keyframes"][0]["ticks"] = 24000; });
    mutation([](nlohmann::json& j) { j["tracks"]["minecraft:visual/sky_color"]["keyframes"][0]["ticks"] = -1; });
    mutation([](nlohmann::json& j) { j["tracks"]["minecraft:visual/sky_color"]["keyframes"][0]["ticks"] = 1.5; });
    mutation([](nlohmann::json& j) { j["tracks"]["minecraft:visual/sky_color"]["keyframes"][0]["ticks"] = 16000; });
    mutation([](nlohmann::json& j) { j["tracks"]["minecraft:visual/sky_color"]["keyframes"][1]["ticks"] = 2000; });
    mutation([](nlohmann::json& j) { j["tracks"]["minecraft:visual/sky_color"]["keyframes"][0]["value"] = "#xyzxyz"; });
    mutation([](nlohmann::json& j) { j["tracks"]["minecraft:visual/sky_color"]["keyframes"][0]["value"] = "#1234"; });
    mutation([](nlohmann::json& j) { j["tracks"]["minecraft:visual/sky_light_factor"]["keyframes"][0]["value"] = nullptr; });
    mutation([](nlohmann::json& j) { j["tracks"]["minecraft:visual/sky_light_factor"]["keyframes"][0]["value"] = "NaN"; });
    mutation([](nlohmann::json& j) { j["tracks"]["minecraft:visual/star_brightness"]["keyframes"][0]["value"] = 0.6; });
    mutation([](nlohmann::json& j) { j["tracks"]["minecraft:visual/sun_angle"]["keyframes"][0]["value"] = 359; });
    mutation([](nlohmann::json& j) { j["tracks"]["minecraft:visual/sun_angle"]["ease"]["cubic_bezier"] = {0.2, 0.1, 0.8}; });
    mutation([](nlohmann::json& j) { j["tracks"]["minecraft:visual/sun_angle"]["ease"]["cubic_bezier"][0] = 1.2; });
    mutation([](nlohmann::json& j) { j["tracks"]["minecraft:visual/sun_angle"]["ease"] = "unknown"; });
    mutation([](nlohmann::json& j) {
        auto& frames = j["tracks"]["minecraft:visual/sky_color"]["keyframes"];
        frames = nlohmann::json::array();
        for (int i = 0; i < 1025; ++i) frames.push_back({{"ticks", i}, {"value", "#000000"}});
    });
    std::string nonfinite = synthetic;
    const size_t scalar = nonfinite.find("\"value\": 0.2");
    nonfinite.replace(scalar, std::string("\"value\": 0.2").size(), "\"value\": 1e999");
    invalid.push_back(nonfinite);
    std::string nonfiniteTick = synthetic;
    const size_t tick = nonfiniteTick.find("\"ticks\": 2000");
    nonfiniteTick.replace(tick, std::string("\"ticks\": 2000").size(), "\"ticks\": 1e999");
    invalid.push_back(nonfiniteTick);
    invalid.push_back(std::string(40, '[') + "0" + std::string(40, ']'));
    invalid.push_back(std::string(1024 * 1024 + 1, ' '));
    bool rejected = true, preserved = true;
    for (const std::string& bad : invalid) {
        if (timeline.LoadDay(bad)) rejected = false;
        if (!timeline.Loaded() || !sameVisual(timeline.Sample(8000), middle)) preserved = false;
    }
    CHECK(rejected && preserved,
          "invalid, incomplete, nonfinite, unsorted and oversized timelines preserve the last valid state");
    auto linear = original;
    linear["tracks"]["minecraft:visual/sun_angle"].erase("ease");
    CHECK(timeline.LoadDay(linear.dump()) && closeEnough(timeline.Sample(12000).sunAngle, DirectX::XM_PIDIV2),
          "successful reload replaces easing instead of retaining previous data");

    WorldClock clock;
    CHECK(clock.Ticks() == 0 && clock.Fraction() == 0, "world clock starts at zero without advancing itself");
    clock.Advance(1);
    CHECK(clock.Ticks() == 20 && clock.Fraction() == 0, "world clock runs at exactly 20 TPS");
    clock.Advance(0.025);
    clock.Advance(0.0125);
    CHECK(clock.Ticks() == 20 && closeEnough(clock.Fraction(), 0.75), "sub-tick world time is retained across advances");
    clock.Advance(0.0125);
    CHECK(clock.Ticks() == 21 && clock.Fraction() == 0, "fractional advances carry into whole ticks");
    clock.Set(0);
    clock.Advance(1200);
    CHECK(clock.Ticks() == 24000 && clock.Fraction() == 0, "exactly 1200 seconds advances one 24000-tick day");
    WorldClock split, combined;
    for (int i = 0; i < 72000; ++i) split.Advance(1.0 / 60.0);
    combined.Advance(1200);
    CHECK(split.Ticks() == combined.Ticks() && std::abs(split.Fraction() - combined.Fraction()) < 1e-10,
          "72000 frame advances equal one combined day without fractional drift");
    split.Advance(0.0125);
    split.Advance(0.025);
    combined.Advance(0.0375);
    CHECK(split.Ticks() == combined.Ticks() && closeEnough(split.Fraction(), combined.Fraction()) &&
          closeEnough(split.Fraction(), 0.75), "split and combined advances preserve the same remaining fraction");
    WorldClock tiny;
    for (int i = 0; i < 10000; ++i) tiny.Advance(1e-8);
    CHECK(tiny.Ticks() == 0 && std::abs(tiny.Fraction() - 0.002) < 1e-15,
          "tiny positive advances are accumulated rather than discarded");
    clock.Advance(0.025);
    clock.Set(123);
    clock.Advance(-1);
    clock.Advance(0);
    clock.Advance(std::numeric_limits<double>::quiet_NaN());
    clock.Advance(std::numeric_limits<double>::infinity());
    CHECK(clock.Ticks() == 123 && clock.Fraction() == 0, "Set resets the fraction and invalid deltas do not advance time");
    clock.Set(largeTicks[2] - 40);
    clock.Advance(1);
    CHECK(clock.Ticks() == largeTicks[2] - 20 && clock.Fraction() == 0,
          "world clock retains uint64 tick precision near its maximum");
    clock.Advance(2);
    CHECK(clock.Ticks() == largeTicks[2] && clock.Fraction() == 0,
          "world clock saturates safely instead of overflowing uint64 ticks");
}

static void TestFrusta() {
    using namespace DirectX;
    Frustum unit;
    ExtractFrustum(XMMatrixIdentity(), unit);
    CHECK(AABBInFrustum(unit, -1, -1, 0, 1, 1, 1), "identity frustum includes D3D clip volume boundaries");
    CHECK(!AABBInFrustum(unit, 0, 0, -0.1f, 0, 0, -0.1f), "D3D near plane rejects negative clip Z");
    CHECK(!AABBInFrustum(unit, 0, 0, 1.1f, 0, 0, 1.1f), "D3D far plane rejects Z greater than W");

    struct CameraCase { float x, y, z, yaw, pitch; };
    const CameraCase cameras[] = {
        {0, 0, 0, 0, 0},
        {8.5f, 65.62f, 8.5f, -28, -19.5f},
        {8.5f, 65.62f, 8.5f, 0, 0},
        {8.5f, 65.62f, 8.5f, 90, 0},
        {8.5f, 65.62f, 8.5f, 180, 0},
        {8.5f, 65.62f, 8.5f, -90, 0},
        {-47, 92, 133, 0, 90},
        {-47, 92, 133, 0, -90},
        {-123.5f, 180.25f, -41.75f, 133, 27},
    };
    const float aspect = 1366.0f / 768.0f;
    const float fov = XMConvertToRadians(70.0f);
    const XMMATRIX projection = XMMatrixPerspectiveFovRH(fov, aspect, 0.1f, 1000.0f);
    bool normalized = true, visible = true, rejected = true, intersecting = true;
    bool planeEquivalent = true, pointEquivalent = true;
    int insidePoints = 0, outsidePoints = 0;
    for (const CameraCase& camera : cameras) {
        const float yaw = XMConvertToRadians(camera.yaw), pitch = XMConvertToRadians(camera.pitch);
        const XMVECTOR eye = XMVectorSet(camera.x, camera.y, camera.z, 1);
        const XMVECTOR forward = XMVectorSet(sinf(yaw) * cosf(pitch), sinf(pitch),
                                             -cosf(yaw) * cosf(pitch), 0);
        const XMVECTOR up = fabsf(camera.pitch) == 90.0f ? XMVectorSet(0, 0, 1, 0)
                                                                       : XMVectorSet(0, 1, 0, 0);
        const XMMATRIX view = XMMatrixLookAtRH(eye, XMVectorAdd(eye, forward), up);
        const XMMATRIX vp = XMMatrixMultiply(view, projection);
        const XMMATRIX inverseView = XMMatrixInverse(nullptr, view);
        Frustum frustum;
        ExtractFrustum(vp, frustum);
        for (const XMFLOAT4& plane : frustum.planes) {
            const float length = sqrtf(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
            if (fabsf(length - 1.0f) > 1e-5f) normalized = false;
        }
        auto worldPoint = [&](float x, float y, float depth) {
            XMFLOAT3 point;
            XMStoreFloat3(&point, XMVector3TransformCoord(XMVectorSet(x, y, -depth, 1), inverseView));
            return point;
        };
        auto boxVisible = [&](float x, float y, float depth, float half) {
            const XMFLOAT3 p = worldPoint(x, y, depth);
            return AABBInFrustum(frustum, p.x - half, p.y - half, p.z - half,
                                 p.x + half, p.y + half, p.z + half);
        };
        if (!boxVisible(0, 0, 8, 0.25f) || !boxVisible(0, 0, 800, 1)) visible = false;
        if (boxVisible(0, 0, -8, 0.25f) || boxVisible(0, 0, 0.025f, 0.005f) ||
            boxVisible(0, 0, 1200, 1) || boxVisible(-40, 0, 8, 0.25f) ||
            boxVisible(40, 0, 8, 0.25f) || boxVisible(0, -40, 8, 0.25f) ||
            boxVisible(0, 40, 8, 0.25f)) rejected = false;
        if (!boxVisible(0, 0, 0.1f, 0.025f) || !boxVisible(0, 0, 1000, 2)) intersecting = false;
        if (camera.x == 8.5f) {
            CHECK(AABBInFrustum(frustum, 0, 0, 0, float(CHUNK_X), float(CHUNK_Y), float(CHUNK_Z)),
                  "spawn chunk remains visible for pitched and cardinal cameras at (8.5,65.62,8.5)");
        }
        const float depths[] = {-0.5f, 0.025f, 0.2f, 4, 100, 800, 1200};
        const float offsets[] = {-2, -0.75f, 0, 0.75f, 2};
        for (float depth : depths) {
            for (float ox : offsets) {
                for (float oy : offsets) {
                    const float height = depth * tanf(fov * 0.5f);
                    const XMFLOAT3 p = worldPoint(ox * height * aspect, oy * height, depth);
                    XMFLOAT4 clip;
                    XMStoreFloat4(&clip, XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), vp));
                    const float distances[6] = {clip.w + clip.x, clip.w - clip.x,
                        clip.w + clip.y, clip.w - clip.y, clip.z, clip.w - clip.z};
                    bool clipInside = true;
                    for (int i = 0; i < 6; ++i) {
                        const XMFLOAT4& plane = frustum.planes[i];
                        const float distance = plane.x * p.x + plane.y * p.y + plane.z * p.z + plane.w;
                        if ((distance >= 0) != (distances[i] >= 0)) planeEquivalent = false;
                        if (distances[i] < 0) clipInside = false;
                    }
                    if (clipInside) ++insidePoints; else ++outsidePoints;
                    if (AABBInFrustum(frustum, p.x, p.y, p.z, p.x, p.y, p.z) != clipInside)
                        pointEquivalent = false;
                }
            }
        }
    }
    CHECK(normalized, "translated and rotated frusta have normalized planes");
    CHECK(visible, "forward AABBs are visible in all six cardinal and oblique orientations");
    CHECK(rejected, "AABBs behind, before near, beyond far and outside each side are rejected");
    CHECK(intersecting, "AABBs crossing near and far planes remain visible");
    CHECK(planeEquivalent, "all six extracted plane signs match homogeneous D3D clip inequalities");
    CHECK(pointEquivalent && insidePoints > 0 && outsidePoints > 0,
          "point AABB visibility matches direct clip-space tests for translated and rotated cameras");
}

static void CheckBoxMesh(const MeshData& mesh, float minX, float minY, float minZ,
                         float maxX, float maxY, float maxZ) {
    CHECK(mesh.vertices.size() == 24 && mesh.indices.size() == 36, "box mesh has six indexed quads");
    if (mesh.vertices.size() != 24 || mesh.indices.size() != 36) return;
    const float lo[3] = {minX, minY, minZ}, hi[3] = {maxX, maxY, maxZ};
    int faces[6] = {};
    bool winding = true, vertical = true, horizontal = true, extents = true, bounds = true;
    for (size_t q = 0; q < mesh.vertices.size(); q += 4) {
        const MeshVertex* v = &mesh.vertices[q];
        int axis = -1;
        for (int d = 0; d < 3; ++d) {
            const float position = d == 0 ? v[0].x : (d == 1 ? v[0].y : v[0].z);
            bool planar = true;
            for (int k = 1; k < 4; ++k) {
                if ((d == 0 ? v[k].x : (d == 1 ? v[k].y : v[k].z)) != position) planar = false;
            }
            if (planar) axis = d;
        }
        if (axis < 0) { bounds = false; continue; }
        const float position = axis == 0 ? v[0].x : (axis == 1 ? v[0].y : v[0].z);
        const int sign = position == lo[axis] ? -1 : (position == hi[axis] ? 1 : 0);
        if (sign == 0) { bounds = false; continue; }
        ++faces[axis * 2 + (sign > 0 ? 1 : 0)];
        for (int t = 0; t < 6; t += 3) {
            const size_t index = q / 4 * 6 + t;
            const uint32_t ia = mesh.indices[index], ib = mesh.indices[index + 1], ic = mesh.indices[index + 2];
            if (ia < q || ia >= q + 4 || ib < q || ib >= q + 4 || ic < q || ic >= q + 4) {
                winding = false;
                continue;
            }
            const MeshVertex& a = mesh.vertices[ia];
            const MeshVertex& b = mesh.vertices[ib];
            const MeshVertex& c = mesh.vertices[ic];
            const float ab[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
            const float ac[3] = {c.x - a.x, c.y - a.y, c.z - a.z};
            const float normal[3] = {ab[1] * ac[2] - ab[2] * ac[1],
                ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]};
            if (normal[axis] * sign <= 0 || normal[(axis + 1) % 3] != 0 ||
                normal[(axis + 2) % 3] != 0) winding = false;
        }
        float minU = v[0].u, maxU = v[0].u, minV = v[0].v, maxV = v[0].v;
        for (int k = 0; k < 4; ++k) {
            const float p[3] = {v[k].x, v[k].y, v[k].z};
            for (int d = 0; d < 3; ++d) if (p[d] != lo[d] && p[d] != hi[d]) bounds = false;
            minU = std::min(minU, v[k].u); maxU = std::max(maxU, v[k].u);
            minV = std::min(minV, v[k].v); maxV = std::max(maxV, v[k].v);
            for (int j = 0; j < 4; ++j) {
                const float dx = v[j].x - v[k].x, dy = v[j].y - v[k].y, dz = v[j].z - v[k].z;
                const float du = v[j].u - v[k].u, dv = v[j].v - v[k].v;
                if (axis != 1 && dv != -dy) vertical = false;
                const float expectedU = axis == 0 ? -sign * dz : sign * (axis == 1 ? dz : dx);
                if (du != expectedU) horizontal = false;
            }
        }
        const float width = axis == 2 ? maxX - minX : maxZ - minZ;
        const float height = axis == 1 ? maxX - minX : maxY - minY;
        if (minU != 0 || minV != 0 || maxU != width || maxV != height) extents = false;
    }
    for (int count : faces) if (count != 1) bounds = false;
    CHECK(bounds, "box has exactly one quad on each positive and negative face");
    CHECK(winding, "both indexed triangles on every box face have outward cross products");
    CHECK(vertical, "side-face V decreases one tile per block as Y increases on both X and Z faces");
    CHECK(horizontal, "face U follows horizontal distance with consistent opposite-face mirroring");
    CHECK(extents, "merged quad UV extents preserve one tile per block on both axes");
}

static bool Near(float a, float b) {
    return std::isfinite(a) && std::isfinite(b) && fabsf(a - b) < 1e-4f;
}

static void TestToolDefinitions() {
    const ToolKind kinds[] = {ToolKind::Pickaxe, ToolKind::Axe, ToolKind::Shovel, ToolKind::Sword};
    const char* materials[] = {"wooden", "stone", "iron", "diamond", "golden", "netherite"};
    const char* suffixes[] = {"pickaxe", "axe", "shovel", "sword"};
    const int tiers[] = {0, 1, 2, 3, 0, 4};
    const int durability[] = {59, 131, 250, 1561, 32, 2031};
    const float speeds[] = {2, 4, 6, 8, 12, 9};
    const float damage[][4] = {{2, 7, 2.5f, 4}, {3, 9, 3.5f, 5}, {4, 9, 4.5f, 6},
                               {5, 9, 5.5f, 7}, {2, 7, 2.5f, 4}, {6, 10, 6.5f, 8}};
    const float axeRates[] = {0.8f, 0.8f, 0.9f, 1, 1, 1};
    for (int material = 0; material < 6; ++material) {
        for (int kind = 0; kind < 4; ++kind) {
            const uint8_t id = uint8_t(ITEM_WOOD_PICKAXE + material * 4 + kind);
            const ToolDef* tool = FindTool(id);
            const std::string name = std::string(materials[material]) + "_" + suffixes[kind];
            const float rate = kind == 0 ? 1.2f : (kind == 1 ? axeRates[material] : (kind == 3 ? 1.6f : 1));
            const bool valid = tool && tool->name && name == tool->name && tool->kind == kinds[kind] &&
                tool->tier == tiers[material] && tool->durability == durability[material] &&
                Near(tool->speed, kind == 3 ? 1 : speeds[material]) && Near(tool->damage, damage[material][kind]) &&
                Near(tool->attackSpeed, rate) && IsValidItem(id) && ItemStackLimit(id) == 1 &&
                name == ItemName(id) && ItemTexture(id) == "minecraft/textures/item/" + name + ".png";
            const std::string label = name + " has correct stats, item texture and single-item stack limit";
            CHECK(valid, label.c_str());
        }
    }
    bool itemRanges = !IsValidItem(-1) && !IsValidItem(256);
    for (int id = 0; id < 256; ++id) {
        const bool tool = id >= ITEM_WOOD_PICKAXE && id <= ITEM_NETHERITE_SWORD;
        const bool armor = (id >= ITEM_LEATHER_HELMET && id <= ITEM_NETHERITE_BOOTS) || id == ITEM_ELYTRA;
        const bool extra = id >= ITEM_ELYTRA && id <= ITEM_COPPER_INGOT;
        const bool expected = id < BLOCK_COUNT || id == ITEM_STICK || tool || armor || extra ||
                              (id >= ITEM_COAL && id <= ITEM_RAW_COPPER);
        if (IsValidItem(id) != expected || (FindTool(uint8_t(id)) != nullptr) != tool) itemRanges = false;
        if ((FindArmor(uint8_t(id)) != nullptr) != armor) itemRanges = false;
        const int limit = (tool || armor || id == ITEM_FLINT_AND_STEEL) ? 1 : 64;
        if (expected && ItemStackLimit(uint8_t(id)) != limit) itemRanges = false;
        if (expected && std::string(ItemName(uint8_t(id))) == "unknown") itemRanges = false;
    }
    CHECK(itemRanges, "block, stick, tool and material ID ranges reject gaps and keep their stack limits");
    CHECK(ItemTexture(BLOCK_LOG) == "minecraft/textures/block/oak_log.png" &&
          ItemTexture(ITEM_STICK) == "minecraft/textures/item/stick.png" &&
          ItemTexture(ITEM_LAPIS) == "minecraft/textures/item/lapis_lazuli.png",
          "block textures and non-block item textures use separate asset namespaces");
}

static void TestToolRecipes() {
    const uint8_t materials[] = {BLOCK_PLANKS, BLOCK_COBBLE, ITEM_IRON_INGOT, ITEM_DIAMOND, ITEM_GOLD_INGOT};
    const uint8_t planks[] = {BLOCK_PLANKS, BLOCK_SPRUCE_PLANKS, BLOCK_BIRCH_PLANKS};
    const char* patterns[] = {"MMM S  S ", "MMMS S", "MSS", "MMS"};
    const int widths[] = {3, 2, 1, 1};
    for (int material = 0; material < 5; ++material) {
        for (int kind = 0; kind < 4; ++kind) {
            bool valid = true;
            const uint8_t output = uint8_t(ITEM_WOOD_PICKAXE + material * 4 + kind);
            for (int mirror = 0; mirror < 2; ++mirror) {
                for (int offset = 0; offset <= 3 - widths[kind]; ++offset) {
                    InventoryState inv;
                    for (int r = 0; r < 3; ++r) {
                        for (int c = 0; c < widths[kind]; ++c) {
                            const char symbol = patterns[kind][r * widths[kind] + (mirror ? widths[kind] - 1 - c : c)];
                            if (symbol == ' ') continue;
                            const uint8_t id = symbol == 'S' ? uint8_t(ITEM_STICK) :
                                (material == 0 ? planks[(r + c) % 3] : materials[material]);
                            inv.craft[r * 3 + offset + c] = {id, 2};
                        }
                    }
                    const InventoryState ingredients = inv;
                    const ItemSlot preview = inv.Result(true);
                    if (preview.id != output || preview.count != 1 || preview.damage != 0 || !inv.Result(false).Empty()) valid = false;
                    inv.cursor = {output, 1, 1};
                    if (inv.Click(45, false, true)) valid = false;
                    inv.cursor = {};
                    if (!inv.Click(45, mirror != 0, true) || inv.cursor.id != output || inv.cursor.count != 1 || inv.cursor.damage != 0) valid = false;
                    for (int i = 0; i < 9; ++i) {
                        if (ingredients.craft[i].Empty()) {
                            if (!inv.craft[i].Empty()) valid = false;
                        } else if (inv.craft[i].id != ingredients.craft[i].id || inv.craft[i].count != 1) valid = false;
                    }
                    if (inv.Click(45, false, true)) valid = false; // A second tool cannot stack on the cursor.
                    if (!inv.Click(27, false, true) || !inv.Click(45, false, true) || inv.cursor.id != output) valid = false;
                    for (const ItemSlot& slot : inv.craft) if (slot.id != 0 || slot.count != 0 || slot.damage != 0) valid = false;
                    if (!inv.Result(true).Empty() || inv.Click(45, false, true)) valid = false;

                    inv = ingredients;
                    inv.craft[0] = {ITEM_COAL, 1};
                    if (!inv.Result(true).Empty()) valid = false;
                    inv = ingredients;
                    if (!inv.Click(45, false, true, true) || inv.slots[27].id != output || inv.slots[28].id != output ||
                        inv.slots[27].count != 1 || inv.slots[28].count != 1 || inv.slots[27].damage != 0 ||
                        inv.slots[28].damage != 0 || !inv.Result(true).Empty() || !inv.cursor.Empty()) valid = false;
                }
            }
            const std::string label = std::string(ItemName(output)) + " recipes mirror/translate, consume exactly once and shift-craft unstacked";
            CHECK(valid, label.c_str());
        }
    }
    InventoryState sticks;
    sticks.craft[1] = {BLOCK_SPRUCE_PLANKS, 2};
    sticks.craft[4] = {BLOCK_BIRCH_PLANKS, 2};
    CHECK(sticks.Result(false).id == ITEM_STICK && sticks.Result(false).count == 4 &&
          sticks.Click(45, false, false, true) && sticks.slots[27].id == ITEM_STICK &&
          sticks.slots[27].count == 8 && sticks.craft[1].Empty() && sticks.craft[4].Empty(),
          "translated mixed-plank stick recipe consumes both ingredients for every batch");
    InventoryState bounded;
    bounded.craft[0] = {ITEM_DIAMOND, 3};
    bounded.craft[3] = bounded.craft[6] = {ITEM_STICK, 3};
    for (ItemSlot& slot : bounded.slots) slot = {BLOCK_STONE, 64};
    bounded.slots[0] = bounded.slots[1] = {};
    CHECK(bounded.Click(45, false, true, true) && bounded.slots[0].id == ITEM_DIAMOND_SHOVEL &&
          bounded.slots[1].id == ITEM_DIAMOND_SHOVEL && bounded.craft[0].count == 1 &&
          bounded.craft[3].count == 1 && bounded.craft[6].count == 1 && !bounded.Click(45, false, true, true),
          "shift-crafting tools stops at inventory capacity without consuming the remaining recipe");
    bounded.slots[2] = {};
    CHECK(bounded.Click(45, false, true, true) && bounded.slots[2].id == ITEM_DIAMOND_SHOVEL && bounded.Result(true).Empty(),
          "remaining tool recipe crafts after exactly one inventory cell is freed");
    InventoryState netherite;
    netherite.craft[0] = {ITEM_NETHERITE_INGOT, 1};
    netherite.craft[3] = netherite.craft[6] = {ITEM_STICK, 1};
    CHECK(netherite.Result(true).Empty(), "netherite ingots do not accidentally match ordinary shovel recipes");
}

using ItemTotals = std::map<std::pair<int, int>, int>;
static ItemTotals InventoryItems(const InventoryState& inv, bool* canonical = nullptr) {
    ItemTotals totals;
    bool valid = true;
    auto count = [&](const ItemSlot& slot) {
        if (slot.Empty()) {
            if (slot.id != 0 || slot.count != 0 || slot.damage != 0) valid = false;
            return;
        }
        if (!IsValidItem(slot.id) || slot.count > ItemStackLimit(slot.id)) valid = false;
        const ToolDef* tool = FindTool(slot.id);
        if (tool ? slot.damage >= tool->durability : slot.damage != 0) valid = false;
        totals[{slot.id, slot.damage}] += slot.count;
    };
    for (const ItemSlot& slot : inv.slots) count(slot);
    for (const ItemSlot& slot : inv.craft) count(slot);
    count(inv.cursor);
    if (canonical) *canonical = valid;
    return totals;
}

static void TestDamagedInventory() {
    bool allTools = true;
    for (int id = ITEM_WOOD_PICKAXE; id <= ITEM_NETHERITE_SWORD; ++id) {
        InventoryState inv;
        if (inv.Add(uint8_t(id), 2, 7) != 0 || inv.slots[27].id != id || inv.slots[28].id != id ||
            inv.slots[27].count != 1 || inv.slots[28].count != 1 || inv.slots[27].damage != 7 || inv.slots[28].damage != 7) allTools = false;
        const ItemTotals before = InventoryItems(inv);
        if (!inv.Click(27, true, true) || inv.cursor.damage != 7 || inv.slots[27].damage != 0 ||
            !inv.Click(36, false, true) || !inv.Click(36, false, true, true) ||
            !inv.Click(28, false, true, true) || !inv.Click(0, false, true) || !inv.Close()) allTools = false;
        bool canonical;
        if (InventoryItems(inv, &canonical) != before || !canonical) allTools = false;
    }
    CHECK(allTools, "all 24 damaged tools retain wear through pickup, right-click, grid/storage shift and close");

    InventoryState inv;
    CHECK(inv.Capacity(ITEM_WOOD_PICKAXE) == 36 && inv.Capacity(ITEM_STICK) == 36 * 64 &&
          inv.Add(ITEM_WOOD_PICKAXE, 2, 7) == 0 && inv.Add(ITEM_WOOD_PICKAXE, 1, 19) == 0 &&
          inv.Capacity(ITEM_WOOD_PICKAXE) == 33, "tool pickup uses separate cells even for identical IDs and damage");
    const ItemTotals before = InventoryItems(inv);
    CHECK(inv.Click(27, true, true) && inv.Click(29, false, true) && inv.cursor.damage == 19 &&
          inv.slots[29].damage == 7 && inv.Click(36, true, true) && inv.craft[0].damage == 19 && inv.Close() &&
          InventoryItems(inv) == before, "swapping differently damaged tools never merges or repairs them");

    InventoryState full;
    for (ItemSlot& slot : full.slots) slot = {BLOCK_STONE, 64};
    full.slots[0] = {};
    full.cursor = {ITEM_WOOD_PICKAXE, 1, 11};
    full.craft[0] = {ITEM_STONE_AXE, 1, 23};
    const ItemTotals blocked = InventoryItems(full);
    CHECK(!full.Close() && full.slots[0].Empty() && full.cursor.damage == 11 && full.craft[0].damage == 23 &&
          InventoryItems(full) == blocked, "failed damaged-tool close is atomic when only one of two tools fits");
    CHECK(full.Add(ITEM_DIAMOND_SWORD, 2, 31) == 1 && full.slots[0].count == 1 && full.slots[0].damage == 31,
          "partial tool pickup reports the exact uninserted count and preserves damage");
    CHECK(!full.Click(0, false, true, true) && full.slots[0].damage == 31,
          "shift-click into a full destination preserves the damaged source tool");
    full.slots[1] = full.slots[2] = {};
    const ItemTotals returned = InventoryItems(full);
    CHECK(full.Close() && InventoryItems(full) == returned && full.cursor.Empty() && full.craft[0].Empty(),
          "successful close returns differently damaged tools without changing their identities");

    inv = InventoryState{};
    inv.Add(ITEM_WOOD_PICKAXE, 2, 7);
    inv.Add(ITEM_WOOD_PICKAXE, 1, 19);
    inv.Add(ITEM_IRON_AXE, 1, 71);
    inv.Add(ITEM_DIAMOND_SHOVEL, 1, 103);
    inv.Add(ITEM_GOLD_SWORD, 1, 5);
    inv.Add(ITEM_NETHERITE_SWORD, 1, 1001);
    inv.Add(ITEM_STICK, 37);
    const ItemTotals initial = InventoryItems(inv);
    std::mt19937 rng(70423);
    bool conserved = true;
    for (int n = 0; n < 600; ++n) {
        inv.Click(int(rng() % 45), (rng() & 1) != 0, (rng() & 1) != 0, (rng() & 1) != 0);
        if (n % 13 == 0) inv.Close();
        bool canonical;
        if (InventoryItems(inv, &canonical) != initial || !canonical) conserved = false;
    }
    CHECK(conserved, "600 bounded randomized inventory operations conserve each (item, damage) pair and stack bounds");
}

static void TestMiningAndDurability() {
    CHECK(BlockHardness(BLOCK_STONE) == 1.5f && BlockHardness(BLOCK_DIRT) == 0.5f &&
          BlockHardness(BLOCK_OBSIDIAN) == 50 && BlockHardness(BLOCK_BEDROCK) < 0 && BlockHardness(BLOCK_WATER) < 0,
          "representative mining hardness distinguishes soft, hard and unbreakable blocks");
    CHECK(MiningSeconds(BLOCK_AIR, ITEM_WOOD_PICKAXE, false, true) == 0 && MiningSeconds(BLOCK_TNT, 0) == 0 &&
          std::isinf(MiningSeconds(BLOCK_BEDROCK, ITEM_NETHERITE_PICKAXE)) &&
          std::isinf(MiningSeconds(BLOCK_WATER, ITEM_NETHERITE_PICKAXE)),
          "instant blocks stay instant and bedrock/water cannot be mined");
    CHECK(Near(MiningSeconds(BLOCK_STONE, 0), 7.5f) && Near(MiningSeconds(BLOCK_STONE, ITEM_WOOD_PICKAXE), 1.15f) &&
          Near(MiningSeconds(BLOCK_IRON_ORE, ITEM_WOOD_PICKAXE), 7.5f) &&
          Near(MiningSeconds(BLOCK_IRON_ORE, ITEM_STONE_PICKAXE), 1.15f) &&
          Near(MiningSeconds(BLOCK_DIAMOND_ORE, ITEM_GOLD_PICKAXE), 1.25f),
          "mining rounds up to whole ticks and wrong-tier picks use the no-harvest hardness multiplier");
    CHECK(Near(MiningSeconds(BLOCK_LOG, ITEM_IRON_AXE), 0.5f) && Near(MiningSeconds(BLOCK_LOG, ITEM_IRON_PICKAXE), 3) &&
          Near(MiningSeconds(BLOCK_DIRT, ITEM_IRON_SHOVEL), 0.15f) && Near(MiningSeconds(BLOCK_DIRT, ITEM_IRON_PICKAXE), 0.75f),
          "axes and shovels accelerate their blocks while wrong tool kinds do not");
    const float grounded = MiningSeconds(BLOCK_COAL_ORE, ITEM_WOOD_PICKAXE);
    CHECK(Near(grounded, 2.25f) && Near(MiningSeconds(BLOCK_COAL_ORE, ITEM_WOOD_PICKAXE, false), grounded * 5) &&
          Near(MiningSeconds(BLOCK_COAL_ORE, ITEM_WOOD_PICKAXE, true, true), grounded * 5) &&
          Near(MiningSeconds(BLOCK_COAL_ORE, ITEM_WOOD_PICKAXE, false, true), grounded * 25),
          "airborne and underwater mining penalties are independent and compound 25-fold");
    CHECK(!CanHarvest(BLOCK_STONE, 0) && !CanHarvest(BLOCK_STONE, ITEM_DIAMOND_AXE) &&
          CanHarvest(BLOCK_STONE, ITEM_WOOD_PICKAXE) && CanHarvest(BLOCK_COAL_ORE, ITEM_GOLD_PICKAXE) &&
          !CanHarvest(BLOCK_IRON_ORE, ITEM_GOLD_PICKAXE) && CanHarvest(BLOCK_IRON_ORE, ITEM_STONE_PICKAXE) &&
          !CanHarvest(BLOCK_DIAMOND_ORE, ITEM_STONE_PICKAXE) && CanHarvest(BLOCK_DIAMOND_ORE, ITEM_IRON_PICKAXE) &&
          !CanHarvest(BLOCK_OBSIDIAN, ITEM_IRON_PICKAXE) && CanHarvest(BLOCK_OBSIDIAN, ITEM_DIAMOND_PICKAXE) &&
          CanHarvest(BLOCK_OBSIDIAN, ITEM_NETHERITE_PICKAXE) && CanHarvest(BLOCK_DIRT, 0),
          "harvest tiers distinguish fast low-tier gold from stone, iron, diamond and netherite picks");
    const uint8_t ores[] = {BLOCK_COAL_ORE, BLOCK_IRON_ORE, BLOCK_GOLD_ORE, BLOCK_DIAMOND_ORE, BLOCK_REDSTONE_ORE, BLOCK_LAPIS_ORE};
    const uint8_t drops[] = {ITEM_COAL, ITEM_RAW_IRON, ITEM_RAW_GOLD, ITEM_DIAMOND, ITEM_REDSTONE, ITEM_LAPIS};
    const int requiredTiers[] = {0, 1, 2, 2, 2, 1};
    bool oreDrops = true;
    for (int i = 0; i < 6; ++i) {
        const ItemSlot drop = BlockDrops(ores[i], ITEM_NETHERITE_PICKAXE);
        if (drop.id != drops[i] || drop.count != (i >= 4 ? 4 : 1) || drop.damage != 0 ||
            !BlockDrops(ores[i], ITEM_NETHERITE_SWORD).Empty()) oreDrops = false;
        for (int id = ITEM_WOOD_PICKAXE; id <= ITEM_NETHERITE_SWORD; ++id) {
            const ToolDef* tool = FindTool(uint8_t(id));
            const bool harvest = tool && tool->kind == ToolKind::Pickaxe && tool->tier >= requiredTiers[i];
            if (CanHarvest(ores[i], uint8_t(id)) != harvest || BlockDrops(ores[i], uint8_t(id)).Empty() == harvest) oreDrops = false;
        }
    }
    CHECK(oreDrops, "all six ores enforce harvest tiers across all tools and yield the correct material/count");
    CHECK(BlockDrops(BLOCK_STONE, ITEM_WOOD_PICKAXE).id == BLOCK_COBBLE && BlockDrops(BLOCK_GRASS, 0).id == BLOCK_DIRT &&
          BlockDrops(BLOCK_LOG, 0).id == BLOCK_LOG && BlockDrops(BLOCK_OBSIDIAN, ITEM_DIAMOND_PICKAXE).id == BLOCK_OBSIDIAN &&
          BlockDrops(BLOCK_STONE, 0).Empty() && BlockDrops(BLOCK_OBSIDIAN, ITEM_GOLD_PICKAXE).Empty() &&
          BlockDrops(BLOCK_AIR, 0).Empty() && BlockDrops(BLOCK_WATER, 0).Empty() && BlockDrops(BLOCK_BEDROCK, 0).Empty() &&
          BlockDrops(BLOCK_GLASS, 0).Empty() && BlockDrops(BLOCK_ICE, 0).Empty() &&
          BlockDrops(BLOCK_OAK_LEAVES, 0).Empty() && BlockDrops(BLOCK_SPRUCE_LEAVES, 0).Empty() && BlockDrops(BLOCK_BIRCH_LEAVES, 0).Empty(),
          "stone/grass drops transform, ordinary blocks survive and non-dropping blocks stay empty");
    bool finite = true;
    for (int block = 0; block < BLOCK_COUNT; ++block) {
        const float seconds = MiningSeconds(uint8_t(block), ITEM_NETHERITE_PICKAXE);
        if (block == BLOCK_WATER || block == BLOCK_BEDROCK || block == BLOCK_LAVA || block == BLOCK_PORTAL) {
            if (!std::isinf(seconds) || CanHarvest(uint8_t(block), ITEM_NETHERITE_PICKAXE)) finite = false;
        } else if (!std::isfinite(seconds) || seconds < 0) finite = false;
    }
    CHECK(finite, "every registered block has a nonnegative finite mining time or is explicitly unbreakable");
    bool durability = true;
    for (int id = ITEM_WOOD_PICKAXE; id <= ITEM_NETHERITE_SWORD; ++id) {
        const ToolDef* tool = FindTool(uint8_t(id));
        if (!tool) { durability = false; continue; }
        ItemSlot slot = {uint8_t(id), 1};
        if (WearTool(slot, 0) || WearTool(slot, -1) || slot.damage != 0 ||
            WearTool(slot, tool->durability - 1) || slot.id != id || slot.count != 1 || slot.damage != tool->durability - 1 ||
            !WearTool(slot, 1) || slot.id != 0 || slot.count != 0 || slot.damage != 0 || WearTool(slot, 1)) durability = false;
        slot = {uint8_t(id), 1, uint16_t(tool->durability - 2)};
        if (!WearTool(slot, 3) || slot.id != 0 || slot.damage != 0 || slot.count != 0) durability = false;
    }
    ItemSlot material = {ITEM_STICK, 12};
    CHECK(durability && !WearTool(material, 100) && material.id == ITEM_STICK && material.count == 12 && material.damage == 0,
          "all 24 tools retain the last use, break exactly at/above durability and ignore nonpositive wear/non-tools");
}

static void TestHurtAndZombies() {
    HurtResistance resistance;
    CHECK(resistance.Apply(4) == 4 && resistance.remaining == 1 && resistance.lastDamage == 4 &&
          resistance.Apply(4) == 0 && resistance.Apply(2) == 0,
          "first hit starts hurt immunity and repeated equal/weaker hits deal no damage");
    resistance.Advance(0.25f);
    CHECK(resistance.Apply(7) == 3 && resistance.remaining == 0.75f && resistance.lastDamage == 7 && resistance.Apply(6) == 0,
          "stronger hits during immunity apply only excess damage without extending the timer");
    resistance.Advance(0.25f);
    CHECK(resistance.Apply(3) == 3 && resistance.remaining == 1 && resistance.lastDamage == 3,
          "a hit at the exact half-second immunity boundary applies full damage");
    resistance.Advance(2);
    CHECK(resistance.remaining == 0 && resistance.Apply(1) == 1, "hurt timers saturate at zero and expired immunity accepts a weaker hit");

    Zombie zombie;
    zombie.body.x = 0; zombie.body.y = 64; zombie.body.z = 0; zombie.body.onGround = true;
    CHECK(!zombie.body.flying && Near(zombie.body.walkSpeed, 1.3f) && zombie.health == 20 &&
          zombie.Hit(4, 2, -1) == 4 && zombie.health == 16 && zombie.body.impulseX == 2 &&
          zombie.body.impulseZ == -1 && zombie.body.vy == 4 && !zombie.body.onGround,
          "zombies start in walk mode and accepted hits apply health loss and horizontal/upward impulses");
    CHECK(zombie.Hit(4, 99, 99) == 0 && zombie.health == 16 && zombie.body.impulseX == 2 &&
          zombie.body.impulseZ == -1 && zombie.body.vy == 4,
          "immune zombie hits cannot refresh knockback or remove health");
    CHECK(zombie.Hit(7, -3, 2) == 3 && zombie.health == 13 && zombie.body.impulseX == -3 && zombie.body.impulseZ == 2,
          "an excess-damage zombie hit updates impulses and removes only the damage difference");
    zombie.resistance.Advance(0.5f);
    CHECK(zombie.Hit(3, 1, 1) == 3 && zombie.health == 10 && zombie.Hit(100, 4, -4) > 0 && zombie.health == 0 &&
          zombie.Hit(100, 99, 99) == 0 && zombie.body.impulseX == 4 && zombie.body.impulseZ == -4,
          "lethal hits clamp zombie health to zero and dead zombies reject subsequent damage/impulses");

    auto ground = [](int, int y, int) -> uint8_t { return y >= 0 && y < 64 ? BLOCK_STONE : BLOCK_AIR; };
    PlayerBody player;
    player.x = 10; player.y = 64; player.z = 0;
    Zombie passive;
    passive.passive = true;
    passive.body.x = 0; passive.body.y = 64; passive.body.z = 0; passive.body.onGround = true;
    passive.Hit(2, 2, -1);
    passive.Update(1.0f / 60.0f, player, ground);
    CHECK(passive.body.x > 0 && passive.body.z < 0 && passive.body.y > 64 &&
          passive.body.impulseX > 0 && passive.body.impulseX < 2 && passive.body.impulseZ > -1,
          "passive zombie physics still integrates and damps accepted hit impulses");
    for (int i = 0; i < 119; ++i) passive.Update(1.0f / 60.0f, player, ground);
    CHECK(passive.body.onGround && Near(passive.body.y, 64) && fabsf(passive.body.impulseX) < 0.001f &&
          fabsf(passive.body.impulseZ) < 0.001f && passive.resistance.remaining == 0,
          "bounded zombie simulation lands on voxels and expires impulse/hurt timers");
    Zombie chasing;
    chasing.body.x = 0; chasing.body.y = 64; chasing.body.z = 0; chasing.body.onGround = true;
    chasing.Update(1.0f / 60.0f, player, ground);
    CHECK(chasing.body.x > 0 && fabsf(chasing.body.z) < 0.001f && Near(chasing.yaw, DirectX::XM_PIDIV2),
          "active zombie faces and walks toward a nearby player");
    Zombie distant;
    distant.body.x = 0; distant.body.y = 64; distant.body.z = 0; distant.body.onGround = true;
    player.x = 40;
    distant.Update(1.0f / 60.0f, player, ground);
    CHECK(distant.body.x == 0 && distant.body.z == 0, "zombie pursuit stops outside its bounded detection range");

    FlyCamera ray;
    ray.x = -8; ray.y = 65; ray.z = 4;
    ray.fwdX = 0; ray.fwdY = 0; ray.fwdZ = -1;
    PlayerBody target;
    target.x = -8; target.y = 64; target.z = 0;
    float distance = -1;
    CHECK(RayHitsBody(ray, target, 4, distance) && Near(distance, 3.7f),
          "body ray reports the nearest AABB face distance at negative world coordinates");
    CHECK(!RayHitsBody(ray, target, 3.69f, distance) && RayHitsBody(ray, target, 3.7f, distance),
          "body ray respects reach and includes a face exactly at the reach boundary");
    ray.x = target.x + PlayerBody::HALF;
    CHECK(RayHitsBody(ray, target, 4, distance), "parallel body ray includes a grazing side face");
    ray.x += 0.01f;
    CHECK(!RayHitsBody(ray, target, 4, distance), "parallel body ray outside a side slab misses");
    ray.x = target.x; ray.y = 66.1f;
    CHECK(!RayHitsBody(ray, target, 4, distance), "body ray above the entire zombie hitbox misses");
    ray.y = 63.99f;
    CHECK(!RayHitsBody(ray, target, 4, distance), "body ray below the feet misses");
    ray.y = 65; ray.fwdZ = 1;
    CHECK(!RayHitsBody(ray, target, 8, distance), "body ray pointing away rejects a target behind the camera");
    ray.z = 0; ray.fwdZ = -1;
    CHECK(RayHitsBody(ray, target, 0, distance) && distance == 0, "body ray originating inside the hitbox has zero entry distance");
    ray.x = -12; ray.z = 0; ray.fwdX = 1; ray.fwdZ = 0;
    CHECK(RayHitsBody(ray, target, 4, distance) && Near(distance, 3.7f), "body ray handles the positive-X slab direction");
    ray.x = -8; ray.y = 68; ray.fwdX = 0; ray.fwdY = -1;
    CHECK(RayHitsBody(ray, target, 4, distance) && distance > 2 && distance < 3,
          "vertical body ray hits the top slab without dividing by zero on parallel axes");
}

static uint8_t ChunkBlock(const std::vector<uint8_t>& blocks, int x, int y, int z) {
    return blocks[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y];
}

static bool TreeBlock(uint8_t id) {
    return id == BLOCK_LOG || id == BLOCK_SPRUCE_LOG || id == BLOCK_BIRCH_LOG ||
           id == BLOCK_OAK_LEAVES || id == BLOCK_SPRUCE_LEAVES || id == BLOCK_BIRCH_LEAVES;
}

static bool ClimateColumnsValid(const std::vector<uint8_t>& blocks, const PerlinNoise& noise,
                                int cx, int cz, int* oreCounts, int& caveAir, bool edgesOnly = false) {
    const int oreMaxY[] = {144, 80, 40, 16, 24, 32};
    bool valid = true;
    for (int z = 0; z < CHUNK_Z; ++z) {
        for (int x = 0; x < CHUNK_X; ++x) {
            if (edgesOnly && x != 0 && x != CHUNK_X - 1 && z != 0 && z != CHUNK_Z - 1) continue;
            const int wx = cx * CHUNK_X + x, wz = cz * CHUNK_Z + z;
            const TerrainColumn column = SampleTerrain(noise, wx, wz);
            if (column.height < 5 || column.height > 210) { valid = false; continue; }
            const int h = column.height;
            const bool river = column.biome == Biome::River;
            const bool sandy = column.biome == Biome::Desert || (!river && h <= 65);
            const bool rocky = column.biome == Biome::Mountains && h > 104;
            const bool snowy = !sandy && !river && (column.temperature < 0.24f || h >= 155);
            const uint8_t surface = river ? uint8_t(BLOCK_GRAVEL) : (sandy ? uint8_t(BLOCK_SAND) :
                (snowy ? uint8_t(BLOCK_SNOW) : (rocky ? uint8_t(BLOCK_STONE) : uint8_t(BLOCK_GRASS))));
            if (ChunkBlock(blocks, x, 0, z) != BLOCK_BEDROCK || ChunkBlock(blocks, x, h, z) != surface) valid = false;
            // Sample all four WORLD neighbors, including those beyond this chunk's edges.
            int waterFloor = 63;
            waterFloor = std::min(waterFloor, SampleTerrain(noise, wx - 1, wz).height);
            waterFloor = std::min(waterFloor, SampleTerrain(noise, wx + 1, wz).height);
            waterFloor = std::min(waterFloor, SampleTerrain(noise, wx, wz - 1).height);
            waterFloor = std::min(waterFloor, SampleTerrain(noise, wx, wz + 1).height);
            for (int y = 1; y < CHUNK_Y; ++y) {
                const uint8_t block = ChunkBlock(blocks, x, y, z);
                if (block >= BLOCK_COUNT) { valid = false; continue; }
                if (y <= h) {
                    if (block == BLOCK_AIR) {
                        ++caveAir;
                        if (y <= 6 || y >= h - 5 || h <= 64 || river || column.biome == Biome::Ocean ||
                            (y <= 63 && y > waterFloor)) valid = false;
                    } else if (!(BLOCK_FLAGS[block] & BF_SOLID)) valid = false;
                    if (y >= h - 5 && !(BLOCK_FLAGS[block] & BF_SOLID)) valid = false;
                } else if (y <= 63) {
                    const uint8_t liquid = y == 63 && column.temperature < 0.27f ? uint8_t(BLOCK_ICE) : uint8_t(BLOCK_WATER);
                    if (block != liquid) valid = false;
                } else if (block != BLOCK_AIR && !TreeBlock(block)) valid = false;
                if (y >= 224 && block != BLOCK_AIR) valid = false;
                if (block >= BLOCK_COAL_ORE && block <= BLOCK_LAPIS_ORE) {
                    const int ore = block - BLOCK_COAL_ORE;
                    if (y >= oreMaxY[ore] || y >= h - 3) valid = false;
                    if (oreCounts) ++oreCounts[ore];
                }
            }
        }
    }
    return valid;
}

static void TestClimateGeneration() {
    const WorldGenerator generators[] = {WorldGenerator::Legacy, WorldGenerator::Climate, WorldGenerator::Flat, WorldGenerator::Nether};
    const char* names[] = {"potato-perlin-v1", "potato-climate-v2", "potato-flat-v1", "potato-nether-v1"};
    const char* aliases[] = {"legacy", "climate", "flat", "nether"};
    bool generatorIds = true;
    for (int i = 0; i < 4; ++i) {
        WorldGenerator parsed = generators[(i + 1) % 4];
        if (std::string(GeneratorName(generators[i])) != names[i] || !ParseGenerator(names[i], parsed) || parsed != generators[i] ||
            !ParseGenerator(aliases[i], parsed) || parsed != generators[i]) generatorIds = false;
    }
    WorldGenerator preserved = WorldGenerator::Climate;
    if (ParseGenerator("", preserved) || ParseGenerator("potato-climate-v999", preserved) ||
        ParseGenerator("unknown", preserved) || preserved != WorldGenerator::Climate) generatorIds = false;
    CHECK(generatorIds, "versioned generator IDs and aliases round-trip and unknown IDs preserve the selection");
    bool coordinates = true;
    for (int v = -33; v <= 33; ++v) {
        const int chunk = FloorDiv16(v), local = v - chunk * 16;
        if (local < 0 || local >= 16 || KeyCX(ChunkKey(chunk, -chunk)) != chunk || KeyCZ(ChunkKey(chunk, -chunk)) != -chunk) coordinates = false;
    }
    CHECK(coordinates, "negative and positive world coordinates decompose without seams at -16, 0 and 16");

    const uint32_t seed = 1337;
    PerlinNoise noise(seed), sameNoise(seed), otherNoise(7);
    bool bounded = true, deterministic = true, differs = false, noise3Valid = true, noise3Differs = false;
    int dryX = 0, dryZ = 0, bestHeight = -1;
    // Only 81 column samples, not a generated huge-world biome census.
    for (int z = -1024; z <= 1024; z += 256) {
        for (int x = -1024; x <= 1024; x += 256) {
            const TerrainColumn a = SampleTerrain(noise, x, z), b = SampleTerrain(sameNoise, x, z), c = SampleTerrain(otherNoise, x, z);
            if (a.height < 5 || a.height > 210 || !std::isfinite(a.temperature) || !std::isfinite(a.humidity) ||
                a.temperature < 0 || a.temperature > 1 || a.humidity < 0 || a.humidity > 1 ||
                int(a.biome) < int(Biome::Plains) || int(a.biome) > int(Biome::Ocean) ||
                std::string(BiomeName(a.biome)) == "Unknown") bounded = false;
            if (a.height != b.height || a.biome != b.biome || a.temperature != b.temperature || a.humidity != b.humidity) deterministic = false;
            if (a.height != c.height || a.biome != c.biome || a.temperature != c.temperature || a.humidity != c.humidity) differs = true;
            if (a.height > bestHeight) { bestHeight = a.height; dryX = x; dryZ = z; }
            const float nx = float(x) * 0.013f + 0.37f, nz = float(z) * 0.019f - 0.21f;
            const float n3 = noise.Fbm3(nx, 2.73f, nz, 3), raw = noise.Noise3(nx, -5.31f, nz);
            if (!std::isfinite(n3) || !std::isfinite(raw) || fabsf(n3) > 1.01f || fabsf(raw) > 1.01f ||
                n3 != sameNoise.Fbm3(nx, 2.73f, nz, 3) || raw != sameNoise.Noise3(nx, -5.31f, nz)) noise3Valid = false;
            if (n3 != otherNoise.Fbm3(nx, 2.73f, nz, 3)) noise3Differs = true;
        }
    }
    CHECK(bounded && deterministic && differs, "climate column heights/biomes/climate stay bounded and deterministic but seed-sensitive");
    CHECK(noise3Valid && noise3Differs, "3D cave noise is finite, bounded, repeatable and seed-sensitive at fractional negative coordinates");
    CHECK(bestHeight > 70, "bounded column sampling finds an above-sea cave/ore fixture without biome quotas");

    const int dryCX = FloorDiv16(dryX), dryCZ = FloorDiv16(dryZ);
    const int chunks[][2] = {{-1, -1}, {0, -1}, {-1, 0}, {0, 0}, {dryCX, dryCZ}, {dryCX + 1, dryCZ}};
    std::vector<uint8_t> blocks(CHUNK_VOL), repeated(CHUNK_VOL), changed(CHUNK_VOL);
    int oreCounts[6] = {}, caveAir = 0;
    bool columns = true, seams = true, chunkDeterminism = true, chunkDiffers = false;
    // Six generated chunks total for coverage; two repeat/different-seed calls below.
    for (const auto& chunk : chunks) {
        GenerateClimateChunk(blocks.data(), noise, seed, chunk[0], chunk[1]);
        if (!ClimateColumnsValid(blocks, noise, chunk[0], chunk[1], oreCounts, caveAir)) columns = false;
        int ignoredCaves = 0;
        if (!ClimateColumnsValid(blocks, noise, chunk[0], chunk[1], nullptr, ignoredCaves, true)) seams = false;
        if (chunk[0] == 0 && chunk[1] == 0) repeated = blocks;
    }
    // Regenerate the origin after other chunks: no generation-order or hidden RNG dependence.
    GenerateClimateChunk(blocks.data(), sameNoise, seed, 0, 0);
    chunkDeterminism = blocks == repeated;
    GenerateClimateChunk(changed.data(), otherNoise, 7, 0, 0);
    chunkDiffers = blocks != changed;
    CHECK(chunkDeterminism && chunkDiffers, "complete climate chunks repeat byte-for-byte after other generation and differ across seeds");
    CHECK(columns, "six climate chunks keep valid block IDs, bedrock, sampled surfaces, water levels, solid roofs and ore depth bounds");
    CHECK(seams, "chunk edge/corner columns agree with world-space terrain and seal caves against cross-chunk water neighbors");
    CHECK(caveAir > 0, "bounded climate fixtures contain underground cave air below intact solid roofs");
    const int rareOres = oreCounts[2] + oreCounts[3] + oreCounts[4] + oreCounts[5];
    CHECK(oreCounts[0] > 0 && oreCounts[1] > 0 && rareOres > 0,
          "a few climate chunks contain coal, iron and deep ores without brittle per-biome/per-ore quotas");
    printf("climate fixtures: caves=%d, coal=%d iron=%d gold=%d diamond=%d redstone=%d lapis=%d\n",
           caveAir, oreCounts[0], oreCounts[1], oreCounts[2], oreCounts[3], oreCounts[4], oreCounts[5]);
}

static void TestBlockExpansion() {
    CHECK(BLOCK_COUNT == 138 && ITEM_STICK >= BLOCK_COUNT && ITEM_RAW_COPPER == ITEM_COAL + 10,
          "block registry grows to 138 entries with items relocated above the block range");
    bool solid = true;
    for (int b = BLOCK_DEEPSLATE; b < BLOCK_LAVA; ++b) {
        if (BLOCK_FLAGS[b] != (BF_SOLID | BF_OPAQUE) || !BLOCK_DEFS[b].name || !BLOCK_DEFS[b].side) solid = false;
    }
    CHECK(solid, "all 75 added blocks are solid opaque cubes with texture definitions");
    CHECK(BlockDrops(BLOCK_DEEPSLATE, ITEM_DIAMOND_PICKAXE).id == BLOCK_COBBLED_DEEPSLATE &&
          BlockDrops(BLOCK_DEEPSLATE_DIAMOND_ORE, ITEM_IRON_PICKAXE).id == ITEM_DIAMOND &&
          BlockDrops(BLOCK_DEEPSLATE_DIAMOND_ORE, ITEM_STONE_PICKAXE).Empty() &&
          BlockDrops(BLOCK_DEEPSLATE_COPPER_ORE, ITEM_STONE_PICKAXE).id == ITEM_RAW_COPPER &&
          BlockDrops(BLOCK_DEEPSLATE_COPPER_ORE, ITEM_WOOD_PICKAXE).Empty() &&
          BlockDrops(BLOCK_EMERALD_ORE, ITEM_IRON_PICKAXE).id == ITEM_EMERALD &&
          BlockDrops(BLOCK_EMERALD_ORE, ITEM_STONE_PICKAXE).Empty() &&
          BlockDrops(BLOCK_COPPER_ORE, ITEM_STONE_PICKAXE).count == 2,
          "deepslate/copper/emerald ores enforce harvest tiers and yield the correct material");
    CHECK(BlockDrops(BLOCK_IRON_BLOCK, ITEM_STONE_PICKAXE).id == BLOCK_IRON_BLOCK &&
          BlockDrops(BLOCK_IRON_BLOCK, 0).Empty() &&
          BlockDrops(BLOCK_DIAMOND_BLOCK, ITEM_IRON_PICKAXE).id == BLOCK_DIAMOND_BLOCK &&
          BlockDrops(BLOCK_DIAMOND_BLOCK, ITEM_STONE_PICKAXE).Empty() &&
          BlockDrops(BLOCK_NETHERITE_BLOCK, ITEM_DIAMOND_PICKAXE).id == BLOCK_NETHERITE_BLOCK &&
          BlockDrops(BLOCK_NETHERITE_BLOCK, ITEM_IRON_PICKAXE).Empty(),
          "mineral storage blocks drop themselves and enforce pickaxe tiers");
    CHECK(CanHarvest(BLOCK_ORANGE_WOOL, 0) && CanHarvest(BLOCK_GLOWSTONE, 0) &&
          CanHarvest(BLOCK_SOUL_SAND, 0) && !CanHarvest(BLOCK_NETHERRACK, 0) &&
          CanHarvest(BLOCK_NETHERRACK, ITEM_WOOD_PICKAXE) &&
          BlockDrops(BLOCK_PURPUR_BLOCK, ITEM_WOOD_PICKAXE).id == BLOCK_PURPUR_BLOCK,
          "wool/glowstone/soul sand break by hand while netherrack needs a pickaxe");
    CHECK(BlockDrops(BLOCK_CRYING_OBSIDIAN, ITEM_DIAMOND_PICKAXE).id == BLOCK_CRYING_OBSIDIAN &&
          BlockDrops(BLOCK_CRYING_OBSIDIAN, ITEM_IRON_PICKAXE).Empty() &&
          BlockDrops(BLOCK_ANCIENT_DEBRIS, ITEM_DIAMOND_PICKAXE).id == BLOCK_ANCIENT_DEBRIS &&
          BlockDrops(BLOCK_ANCIENT_DEBRIS, ITEM_IRON_PICKAXE).Empty() &&
          BlockDrops(BLOCK_END_STONE, ITEM_WOOD_PICKAXE).id == BLOCK_END_STONE &&
          CanHarvest(BLOCK_SEA_LANTERN, 0) && CanHarvest(BLOCK_MELON, 0) &&
          CanHarvest(BLOCK_RED_SAND, 0) && BlockDrops(BLOCK_PRISMARINE, ITEM_WOOD_PICKAXE).id == BLOCK_PRISMARINE,
          "second palette batch enforces diamond tier for crying obsidian/ancient debris and hand-breaking for sea lantern/melon/red sand");
    const uint32_t seed = 1337;
    PerlinNoise noise(seed);
    std::vector<uint8_t> blocks(CHUNK_VOL);
    GenerateClimateChunk(blocks.data(), noise, seed, 0, 0);
    bool deepslate = false;
    for (int y = 1; y < 8 && !deepslate; ++y)
        for (int i = 0; i < CHUNK_X * CHUNK_Z; ++i)
            if (blocks[i + CHUNK_X * CHUNK_Z * y] == BLOCK_DEEPSLATE) { deepslate = true; break; }
    CHECK(deepslate, "climate generation places a deepslate layer below y=8");
    bool deepOre = false;
    for (int cz = -2; cz <= 2 && !deepOre; ++cz) {
        for (int cx = -2; cx <= 2 && !deepOre; ++cx) {
            GenerateClimateChunk(blocks.data(), noise, seed, cx, cz);
            for (int y = 1; y < 8 && !deepOre; ++y)
                for (int i = 0; i < CHUNK_X * CHUNK_Z; ++i) {
                    const uint8_t b = blocks[i + CHUNK_X * CHUNK_Z * y];
                    if (b >= BLOCK_DEEPSLATE_COAL_ORE && b <= BLOCK_DEEPSLATE_COPPER_ORE) { deepOre = true; break; }
                }
        }
    }
    CHECK(deepOre, "climate generation converts deep ores into deepslate ore variants");
}

static void TestHeldItemMesh() {
    uint8_t px[16 * 16 * 4];
    memset(px, 0, sizeof(px));
    std::vector<MeshVertex> mesh;
    BuildExtrudedItemMesh(px, mesh);
    CHECK(mesh.empty(), "transparent item sprite extrudes to nothing");

    px[(8 * 16 + 8) * 4 + 3] = 255;
    mesh.clear();
    BuildExtrudedItemMesh(px, mesh);
    CHECK(mesh.size() == 36, "single opaque texel extrudes to 36 vertices");
    bool bounded = true, uvOk = true;
    for (const MeshVertex& v : mesh) {
        if (v.x > 0.30f || v.x < -0.30f || v.y > 0.30f || v.y < -0.30f ||
            v.z > 0.02f || v.z < -0.02f) bounded = false;
        if (v.u < 0.0f || v.u > 1.0f || v.v < 0.0f || v.v > 1.0f) uvOk = false;
    }
    CHECK(bounded, "extruded item stays within hand-local bounds");
    CHECK(uvOk, "extruded item sprite UVs stay in 0..1");

    memset(px, 0xFF, sizeof(px));
    mesh.clear();
    BuildExtrudedItemMesh(px, mesh);
    CHECK(mesh.size() == 3456, "solid 16x16 sprite extrudes to 3456 vertices");
}

static void TestWaterFrames() {
    // Synthetic 16x512 strip (32 frames), each frame a flat shade with Java's
    // baked water alpha (180/255 = 0.706).
    std::vector<uint8_t> strip(16 * 512 * 4, 0);
    for (int f = 0; f < 32; ++f) {
        for (int p = 0; p < 16 * 16; ++p) {
            uint8_t* px = &strip[(size_t(f) * 16 * 16 + p) * 4];
            px[0] = px[1] = px[2] = uint8_t(f * 8);
            px[3] = 180;
        }
    }
    uint8_t out[16 * 16 * 4];
    SliceAnimatedFrame(strip.data(), 16, 512, 0, out);
    bool frame0 = true, alphaKept = true;
    for (int p = 0; p < 16 * 16; ++p) {
        if (out[p * 4] != 0) frame0 = false;
        if (out[p * 4 + 3] != 180) alphaKept = false;
    }
    CHECK(frame0 && alphaKept, "water strip frame 0 extracts with baked 180 alpha");

    SliceAnimatedFrame(strip.data(), 16, 512, 5, out);
    bool frame5 = true;
    for (int p = 0; p < 16 * 16; ++p)
        if (out[p * 4] != 40) frame5 = false;
    CHECK(frame5, "water strip frame 5 extracts the matching vertical slice");

    SliceAnimatedFrame(strip.data(), 16, 512, 32, out);
    bool wrapped = true;
    for (int p = 0; p < 16 * 16; ++p)
        if (out[p * 4] != 0) wrapped = false;
    CHECK(wrapped, "water frame index wraps modulo the strip frame count");

    const uint8_t staticTile[16 * 16 * 4] = {};
    SliceAnimatedFrame(staticTile, 16, 16, 3, out);
    CHECK(out[0] == 0 && out[16 * 16 * 4 - 1] == 0, "static 16x16 tiles pass through unchanged");
}

static void TestWaterShaderPass() {
    const std::string src(gShaderSource);
    auto bodyOf = [&](const char* signature) {
        const size_t start = src.find(signature);
        if (start == std::string::npos) return std::string();
        const size_t end = src.find("\n}\n", start);
        return src.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };
    const std::string water = bodyOf("float4 PSMainWater(");
    CHECK(!water.empty() && water.find("gWaterTime") != std::string::npos,
          "liquid shader scrolls tile UVs on the shared water clock");
    CHECK(!water.empty() && water.find("clip(") == std::string::npos,
          "liquid shader keeps partial alpha instead of opaque clipping");
    const std::string opaque = bodyOf("float4 PSMain(");
    CHECK(!opaque.empty() && opaque.find("clip(tex.a - 0.5f)") != std::string::npos,
          "opaque world pass still alpha-clips cutout blocks");
}

static void TestBubbleParticles() {
    ParticleSystem parts;
    parts.SpawnBubbles(0.0f, 0.0f, 0.0f);
    const auto& list = parts.List();
    CHECK(list.size() == 3, "underwater spawn emits three oxygen bubbles");
    bool rising = !list.empty();
    for (const ParticleSystem::Particle& p : list)
        if (!p.bubble || p.vy <= 0.0f) rising = false;
    CHECK(rising, "bubbles spawn flagged with upward buoyancy");
    const float y0 = list.empty() ? 0.0f : list[0].y;
    parts.Update(0.1f);
    CHECK(!parts.List().empty() && parts.List()[0].y > y0, "bubbles rise over time instead of falling");

    ParticleSystem crit;
    crit.SpawnCrit(0.0f, 10.0f, 0.0f);
    crit.Update(0.3f);
    bool gravity = !crit.List().empty();
    for (const ParticleSystem::Particle& p : crit.List())
        if (p.bubble || p.vy > 2.5f) gravity = false;
    CHECK(gravity, "crit particles still accelerate downward under gravity");
}

// Java 26.3 resource parsing. The sample documents below are invented for the
// test and mirror the schema only; no game data ships in the repository.
static void TestRegistryParsing() {
    CraftingRecipe shaped;
    const std::string shapedJson = R"json({
      "type": "minecraft:crafting_shaped", "category": "building",
      "pattern": ["##", "##"],
      "key": {"#": "minecraft:oak_planks"},
      "result": {"id": "minecraft:crafting_table", "count": 1}
    })json";
    CHECK(ParseRecipeJson(shapedJson, shaped), "jar shaped recipe parses");
    CHECK(shaped.width == 2 && shaped.height == 2 && !shaped.shapeless,
          "shaped recipe keeps its pattern size");
    CHECK(shaped.cells[0] == BLOCK_PLANKS && shaped.cells[1] == BLOCK_PLANKS &&
          shaped.cells[3] == BLOCK_PLANKS && shaped.cells[4] == BLOCK_PLANKS &&
          shaped.cells[2] == 0 && shaped.cells[5] == 0,
          "shaped keys land in row-major cells with empty columns preserved");
    CHECK(shaped.result.id == BLOCK_CRAFTING_TABLE && shaped.result.count == 1,
          "shaped result maps the Java name onto the internal id");

    CraftingRecipe shapeless;
    const std::string shapelessJson = R"json({
      "type": "minecraft:crafting_shapeless",
      "ingredients": ["#minecraft:planks", {"item": "minecraft:stick"},
                      [{"item": "minecraft:oak_sapling"}, {"item": "minecraft:stick"}]],
      "result": {"id": "minecraft:chest", "count": 1}
    })json";
    CHECK(ParseRecipeJson(shapelessJson, shapeless), "jar shapeless recipe parses");
    CHECK(shapeless.shapeless && shapeless.cells[0] == TAG_PLANKS &&
          shapeless.cells[1] == ITEM_STICK && shapeless.cells[2] == ITEM_STICK,
          "shapeless cells keep tag and first-usable alternative");
    CHECK(shapeless.result.id == BLOCK_CHEST, "shapeless result resolves");

    CraftingRecipe invalid;
    CHECK(!ParseRecipeJson("not json", invalid), "malformed recipe text is rejected");
    CHECK(!ParseRecipeJson(R"json({"type":"minecraft:crafting_shaped","pattern":["#"]})json",
                           invalid),
          "pattern without a key is rejected");

    LootEntry loot;
    bool hasItem = false;
    const std::string lootJson = R"json({
      "type": "minecraft:block",
      "pools": [{
        "rolls": 1,
        "entries": [
          {"type": "minecraft:item", "name": "minecraft:stone", "conditions": [
            {"condition": "minecraft:match_tool", "predicate": {"enchantments": [
              {"enchantment": "minecraft:silk_touch", "levels": {"min": 1}}]}}]},
          {"type": "minecraft:item", "name": "minecraft:cobblestone",
           "functions": [{"function": "minecraft:set_count", "count": {"min": 1, "max": 1}}]}
        ]
      }]
    })json";
    CHECK(ParseLootTableJson(lootJson, loot, hasItem), "jar loot table parses");
    CHECK(hasItem && loot.item == BLOCK_COBBLE && loot.count == 1,
          "silk-touch alternatives are skipped and the base drop wins");

    LootEntry ranged;
    CHECK(ParseLootTableJson(R"json({"type":"minecraft:block","pools":[{"rolls":1,
      "entries":[{"type":"minecraft:item","name":"minecraft:raw_iron",
                  "functions":[{"function":"minecraft:set_count","count":{"min":2,"max":3}}]}]}]})json",
                             ranged, hasItem) &&
          hasItem && ranged.item == ITEM_RAW_IRON && ranged.count == 2,
          "set_count ranges use their minimum when fortune is unavailable");

    LootEntry empty;
    CHECK(ParseLootTableJson(R"json({"type":"minecraft:block","pools":[]})json", empty, hasItem) &&
          !hasItem, "a table with no pools parses as an empty drop");
    CHECK(!ParseLootTableJson("{", empty, hasItem), "malformed loot text is rejected");

    CHECK(ItemIdFromResourceName("minecraft:oak_planks") == BLOCK_PLANKS &&
          ItemIdFromResourceName("oak_log") == BLOCK_LOG &&
          ItemIdFromResourceName("minecraft:grass_block") == BLOCK_GRASS &&
          ItemIdFromResourceName("minecraft:iron_pickaxe") == ITEM_IRON_PICKAXE,
          "resource names map onto the internal block and item ids");
    CHECK(ResourceNameForItem(BLOCK_GRASS) == "minecraft:grass_block" &&
          ResourceNameForItem(ITEM_DIAMOND) == "minecraft:diamond",
          "internal ids round-trip to Java resource names");
    CHECK(ItemIdFromResourceName("minecraft:not_a_real_block") == BLOCK_AIR,
          "unknown resource names map to air");

    CHECK(RecipeCellMatches(TAG_PLANKS, BLOCK_PLANKS) &&
          RecipeCellMatches(TAG_PLANKS, BLOCK_SPRUCE_PLANKS) &&
          !RecipeCellMatches(TAG_PLANKS, BLOCK_LOG),
          "planks tag accepts every plank variant and nothing else");
    CHECK(RecipeCellMatches(TAG_STONE_CRAFTING, BLOCK_COBBLE) &&
          RecipeCellMatches(uint16_t(ITEM_STICK), ITEM_STICK) &&
          !RecipeCellMatches(uint16_t(ITEM_STICK), BLOCK_PLANKS),
          "exact id cells only match that id");

    AssetStore emptyStore;
    CHECK(!LoadRegistry(emptyStore) && !RegistryInfo().jarLoaded,
          "registry load without a jar leaves the built-in fallback in place");

    CHECK(ItemCreativeTab(ITEM_STICK) == CTAB_INGREDIENTS &&
          ItemCreativeTab(ITEM_IRON_PICKAXE) == CTAB_TOOLS &&
          ItemCreativeTab(ITEM_IRON_SWORD) == CTAB_COMBAT &&
          ItemCreativeTab(BLOCK_RED_WOOL) == CTAB_COLORED &&
          ItemCreativeTab(BLOCK_GRASS) == CTAB_NATURAL &&
          ItemCreativeTab(BLOCK_STONE) == CTAB_BUILDING &&
          ItemCreativeTab(BLOCK_TNT) == CTAB_REDSTONE &&
          ItemCreativeTab(BLOCK_CRAFTING_TABLE) == CTAB_FUNCTIONAL,
          "creative tabs classify blocks and items like Java");
    CHECK(ItemMatchesSearch(BLOCK_DIAMOND_ORE, "diamond") &&
          ItemMatchesSearch(BLOCK_RED_WOOL, "RED WOOL") &&
          !ItemMatchesSearch(BLOCK_STONE, "diamond") &&
          ItemMatchesSearch(BLOCK_STONE, ""),
          "creative search is a case-insensitive substring with spaces as underscores");
}

static void TestShiftClickParity() {
    InventoryState inv;
    inv.slots[0] = {BLOCK_STONE, 32};
    CHECK(inv.Click(0, false, false, true) && inv.slots[0].Empty(),
          "shift-click moves a full storage stack");
    bool inHotbar = false;
    for (int i = 27; i < 36; ++i)
        if (inv.slots[i].id == BLOCK_STONE && inv.slots[i].count == 32) inHotbar = true;
    CHECK(inHotbar, "shift-clicked storage stack lands in the hotbar row");

    inv = InventoryState{};
    inv.slots[30] = {BLOCK_DIRT, 20};
    CHECK(inv.Click(30, false, false, true) && inv.slots[30].Empty(),
          "shift-click drains a hotbar stack");
    bool inMain = false;
    for (int i = 0; i < 27; ++i)
        if (inv.slots[i].id == BLOCK_DIRT && inv.slots[i].count == 20) inMain = true;
    CHECK(inMain, "shift-clicked hotbar stack lands in main storage");

    inv = InventoryState{};
    inv.slots[5] = {ITEM_IRON_HELMET, 1};
    CHECK(inv.Click(5, false, false, true) && inv.armor[0].id == ITEM_IRON_HELMET &&
          inv.slots[5].Empty(), "shift-click equips armor into its matching slot");

    inv = InventoryState{};
    inv.craft[0] = {BLOCK_LOG, 1};
    CHECK(inv.Click(45, false, false, true) && inv.craft[0].Empty(),
          "shift-click on the craft result consumes the grid");
    bool planks = false;
    for (int i = 0; i < 36; ++i)
        if (inv.slots[i].id == BLOCK_PLANKS && inv.slots[i].count == 4) planks = true;
    CHECK(planks, "shift-clicked craft result lands in the inventory");

    inv = InventoryState{};
    inv.craft[4] = {BLOCK_PLANKS, 6};
    CHECK(inv.Click(40, false, false, true) && inv.craft[4].Empty(),
          "shift-click returns a crafting grid stack to storage");
}

class MockChatHost : public potato::CommandHost {
public:
    int mode = 0;
    float px = 0, py = 64, pz = 0;
    uint64_t ticks = 1000;
    int weather = 0;
    int weatherDuration = 0;
    int kills = 3;
    int cleared = 4;
    int givenCount = 0;
    int summoned = 0;
    bool keepInventory = false;
    std::string lastBiome;
    int GameMode() const override { return mode; }
    void SetGameMode(int value) override { mode = value; }
    void PlayerPos(float& x, float& y, float& z) const override { x = px; y = py; z = pz; }
    void Teleport(float x, float y, float z) override { px = x; py = y; pz = z; }
    uint64_t TimeTicks() const override { return ticks; }
    void SetTimeTicks(uint64_t value) override { ticks = value; }
    uint32_t WorldSeed() const override { return 1337; }
    void SetWeather(int value, int duration) override { weather = value; weatherDuration = duration; }
    int Weather() const override { return weather; }
    bool GiveItem(const std::string& item, int count, std::string& name) override {
        if (item != "stone" && item != "minecraft:stone") return false;
        name = "Stone";
        givenCount = count;
        return true;
    }
    bool LocateBiome(const std::string& biome, int& x, int& z, int& distance) override {
        if (biome != "plains") return false;
        lastBiome = biome;
        x = 24; z = -40; distance = 61;
        return true;
    }
    int Summon(const std::string& entity, int count) override {
        if (entity != "zombie") return -1;
        summoned += count;
        return count;
    }
    int KillEntities(const std::string& target) override {
        if (target != "player" && target != "@s" && target != "zombie" && target != "@e") return -1;
        return kills;
    }
    int ClearInventory() override { return cleared; }
    bool GetGameRule(const std::string& rule, std::string& value) const override {
        if (rule != "keepInventory") return false;
        value = keepInventory ? "true" : "false";
        return true;
    }
    bool SetGameRule(const std::string& rule, const std::string& value) override {
        if (rule != "keepInventory") return false;
        keepInventory = value == "true";
        return true;
    }
    std::string PlayerName() const override { return "Player"; }
};

static void TestChatCommands() {
    MockChatHost host;
    auto run = [&](const std::string& line) { return potato::ExecuteCommand(line, host); };
    auto has = [](const std::string& text, const char* needle) {
        return text.find(needle) != std::string::npos;
    };

    potato::CommandResult r = run("/gamemode creative");
    CHECK(r.ok && !r.error && host.mode == 1, "gamemode switches the world to creative");
    CHECK(!r.lines.empty() && has(r.lines[0], "Creative Mode"), "gamemode feedback matches Java");
    run("/gamemode s");
    CHECK(host.mode == 0, "gamemode accepts the s alias");
    CHECK(run("/gamemode adventure").error, "unsupported game modes are rejected");

    r = run("/give stone 3");
    CHECK(r.ok && host.givenCount == 3, "give honours the amount argument");
    CHECK(!r.lines.empty() && has(r.lines[0], "[Stone]"), "give names the item in brackets");
    CHECK(run("/give stone").ok && host.givenCount == 1, "give defaults to one item");
    CHECK(run("/give not_an_item").error, "unknown items are rejected");

    r = run("/tp 10 64 -20");
    CHECK(r.ok && host.px == 10.0f && host.py == 64.0f && host.pz == -20.0f, "tp applies absolute coordinates");
    run("/tp ~5 ~ ~-2");
    CHECK(host.px == 15.0f && host.py == 64.0f && host.pz == -22.0f, "tp resolves relative coordinates");
    CHECK(run("/tp 1 2").error, "tp requires three coordinates");

    run("/time set day");
    CHECK(host.ticks == 1000, "time set day matches Java's 1000 ticks");
    run("/time add 500");
    CHECK(host.ticks == 1500, "time add advances the clock");
    r = run("/time query daytime");
    CHECK(r.ok && has(r.lines[0], "1500"), "time query reports the current tick");

    r = run("/weather rain");
    CHECK(host.weather == 1 && has(r.lines[0], "rainy weather"), "weather rain starts rain");
    CHECK(host.weatherDuration == 0, "weather without seconds uses the Java default duration");
    run("/weather thunder 60");
    CHECK(host.weather == 2 && host.weatherDuration == 1200, "weather thunder converts seconds to ticks");
    run("/weather clear");
    CHECK(host.weather == 0, "weather clear stops precipitation");

    r = run("/seed");
    CHECK(r.ok && has(r.lines[0], "1337"), "seed reports the world seed");

    r = run("/locate biome plains");
    CHECK(r.ok && host.lastBiome == "plains" && has(r.lines[0], "61 blocks away"), "locate reports the found biome");
    CHECK(run("/locate biome candyland").error, "unknown biomes are rejected");

    r = run("/summon zombie 2");
    CHECK(r.ok && host.summoned == 2 && has(r.lines[0], "Zombie"), "summon spawns zombies");
    CHECK(run("/summon dragon").error, "unknown entities are rejected");

    CHECK(run("/kill").ok && host.kills == 3, "kill targets the player by default");
    r = run("/kill zombie");
    CHECK(has(r.lines[0], "Killed 3 entities"), "kill reports the entity count");

    r = run("/clear");
    CHECK(r.ok && has(r.lines[0], "Removed 4 items"), "clear reports removed stacks");

    r = run("/gamerule keepInventory true");
    CHECK(r.ok && host.keepInventory, "gamerule applies a boolean value");
    r = run("/gamerule keepInventory");
    CHECK(r.ok && has(r.lines[0], "currently set to"), "gamerule queries without a value");
    CHECK(run("/gamerule doFireTick true").error, "unknown gamerules are rejected");
    CHECK(run("/gamerule keepInventory maybe").error, "invalid gamerule values are rejected");

    CHECK(run("/nope").error, "unknown commands are rejected");
    CHECK(run("/").error, "an empty command is rejected");

    potato::ChatHistory history;
    history.Push("first");
    history.Push("second");
    std::string out;
    CHECK(history.Prev(out) && out == "second", "history walks back to the newest line");
    CHECK(history.Prev(out) && out == "first", "history walks back further");
    CHECK(!history.Prev(out), "history stops at the oldest line");
    CHECK(history.Next(out) && out == "second", "history walks forward again");
    CHECK(history.Next(out) && out.empty(), "history forward restores a blank line");
    CHECK(!history.Next(out), "history forward stops past the newest line");

    std::vector<std::string> options = potato::CommandCompletions("/gam");
    CHECK(options.size() == 1 && options[0] == "/gamemode", "tab completes command names");
    options = potato::CommandCompletions("/weather ");
    CHECK(options.size() == 3, "tab offers every weather argument");
    options = potato::CommandCompletions("/time set mid");
    CHECK(options.size() == 1 && options[0] == "/time set midnight", "tab completes nested arguments");
    options = potato::CommandCompletions("/give sto");
    CHECK(!options.empty() && has(options[0], "stone"), "tab completes item ids");
    CHECK(potato::CommonPrefix({"/gamemode creative", "/gamemode c"}) == "/gamemode c",
          "common prefix keeps the shared text");

    potato::ParticleSystem weather;
    weather.SpawnRain(0, 30, 0, false);
    weather.SpawnRain(1, 30, 0, true);
    CHECK(weather.List().size() == 2 && weather.List()[0].rain && weather.List()[1].snow,
          "weather spawns distinct rain and snow drops");
    const float rainY = weather.List()[0].y;
    weather.Update(0.1f);
    CHECK(weather.List()[0].y < rainY, "rain falls over time");
}

int main() {
    TestShaders();
    TestSkyTimeline();
    TestFrusta();
    TestToolDefinitions();
    TestToolRecipes();
    TestDamagedInventory();
    TestMiningAndDurability();
    TestHurtAndZombies();
    TestClimateGeneration();
    TestBlockExpansion();
    TestHeldItemMesh();
    TestWaterFrames();
    TestWaterShaderPass();
    TestBubbleParticles();
    TestRegistryParsing();
    TestShiftClickParity();
    TestChatCommands();

    // --- Perlin determinism and bounds ---
    PerlinNoise a(1337), b(1337), c(7);
    bool same = true, bounded = true, differs = false;
    for (int i = 0; i < 100; ++i) {
        const float x = i * 0.37f, z = i * 0.91f;
        const float na = a.Fbm2(x, z, 4), nb = b.Fbm2(x, z, 4);
        if (na != nb) same = false;
        if (na < -1.01f || na > 1.01f) bounded = false;
        if (na != c.Fbm2(x, z, 4)) differs = true;
    }
    CHECK(same, "Perlin deterministic for same seed");
    CHECK(bounded, "Perlin fBm bounded to [-1, 1]");
    CHECK(differs, "Perlin differs across seeds");

    BlockTiles tiles;
    for (int i = 0; i < 256; ++i) {
        tiles.top[i] = 1;
        tiles.side[i] = 2;
        tiles.bottom[i] = 3;
    }

    // --- Greedy meshing ---
    const std::vector<uint8_t> solid(CHUNK_VOL, BLOCK_STONE);
    MeshData md;
    BuildChunkMeshCPU(solid.data(), nullptr, nullptr, nullptr, nullptr, tiles, md);
    CHECK(md.vertices.size() == 24 && md.indices.size() == 36,
          "solid chunk greedy-meshes to exactly 6 quads");
    bool hasTop = false, hasBottom = false;
    for (const MeshVertex& v : md.vertices) {
        if (v.light == 15) hasTop = true;
        if (v.light == 8) hasBottom = true;
    }
    CHECK(hasTop && hasBottom, "face light levels present (top 15, bottom 8)");
    CheckBoxMesh(md, 0, 0, 0, float(CHUNK_X), float(CHUNK_Y), float(CHUNK_Z));

    std::vector<uint8_t> one(CHUNK_VOL, 0);
    one[5 + 16 * 5 + 256 * 64] = BLOCK_STONE;
    BuildChunkMeshCPU(one.data(), nullptr, nullptr, nullptr, nullptr, tiles, md);
    CHECK(md.vertices.size() == 24, "isolated voxel -> 6 quads");
    bool inPlace = true;
    for (const MeshVertex& v : md.vertices) {
        if (v.x < 5 || v.x > 6 || v.y < 64 || v.y > 65 || v.z < 5 || v.z > 6) inPlace = false;
    }
    if (!inPlace) {
        for (size_t i = 0; i < md.vertices.size(); i += 4) {
            const MeshVertex& v = md.vertices[i];
            printf("quad %zu corner: %.1f %.1f %.1f\n", i / 4, double(v.x), double(v.y), double(v.z));
        }
    }
    CHECK(inPlace, "voxel quads sit on the voxel (no plane-origin drift)");
    CheckBoxMesh(md, 5, 64, 5, 6, 65, 6);

    std::vector<uint8_t> two(CHUNK_VOL, 0);
    two[5 + 16 * 5 + 256 * 64] = BLOCK_STONE;
    two[6 + 16 * 5 + 256 * 64] = BLOCK_STONE;
    BuildChunkMeshCPU(two.data(), nullptr, nullptr, nullptr, nullptr, tiles, md);
    CHECK(md.vertices.size() == 24, "two adjacent voxels merge to 6 quads (10 faces -> 6)");
    CheckBoxMesh(md, 5, 64, 5, 7, 65, 6);

    {
        std::vector<uint8_t> prism(CHUNK_VOL, BLOCK_AIR);
        for (int y = 60; y < 65; ++y)
            for (int z = 4; z < 11; ++z)
                for (int x = 3; x < 6; ++x)
                    prism[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y] = BLOCK_GRASS;
        MeshData merged;
        BuildChunkMeshCPU(prism.data(), nullptr, nullptr, nullptr, nullptr, tiles, merged);
        CHECK(merged.vertices.size() == 24 && merged.indices.size() == 36,
              "3x5x7 grass prism merges to six non-square quads");
        CheckBoxMesh(merged, 3, 60, 4, 6, 65, 11);
    }

    // --- Raycast ---
    auto get = [&](int, int y, int) -> uint8_t {
        return (y >= 0 && y < CHUNK_Y) ? BLOCK_STONE : 0;
    };
    int hx, hy, hz, nx, ny, nz;
    bool hit = RaycastDDA(get, 8, 300, 8, 0, -1, 0, 500, hx, hy, hz, nx, ny, nz);
    CHECK(hit && hy == CHUNK_Y - 1 && ny == 1, "raycast down hits top face at y=255");
    hit = RaycastDDA(get, 8, 300, 8, 0, 1, 0, 500, hx, hy, hz, nx, ny, nz);
    CHECK(!hit, "raycast up misses");

    {
        auto liquidThenStone = [](int, int y, int) -> uint8_t {
            if (y == 65) return BLOCK_WATER;
            return y == 64 ? BLOCK_STONE : BLOCK_AIR;
        };
        hit = RaycastDDA(liquidThenStone, 8, 70, 8, 0, -1, 0, 10, hx, hy, hz, nx, ny, nz);
        CHECK(hit && hy == 64, "selection ray skips water and hits the solid block below");
    }

    {
        InventoryState inv;
        CHECK(inv.Add(BLOCK_LOG, 65) == 0 && inv.slots[27].count == 64 && inv.slots[28].count == 1,
              "pickup fills hotbar first and caps stacks at 64");
        CHECK(inv.Click(28, true, false) && inv.cursor.count == 1 && inv.slots[28].id == 0,
              "right-clicking one item clears the emptied slot id");
        CHECK(inv.Click(36, false, false) && inv.cursor.Empty(), "move log into 2x2 crafting grid");
        CHECK(inv.Result(false).id == BLOCK_PLANKS && inv.Result(false).count == 4,
              "one log previews four planks");
        CHECK(inv.Click(45, false, false) && inv.cursor.count == 4 && inv.craft[0].id == 0,
              "taking output consumes and clears the ingredient");
        CHECK(inv.Result(false).Empty() && !inv.Click(45, false, false) && inv.cursor.count == 4,
              "repeated output click cannot duplicate exhausted ingredients");
        CHECK(inv.Click(28, false, false), "place crafted stack in hotbar");
        inv.slots[0] = {BLOCK_PLANKS, 63};
        CHECK(inv.Click(28, false, false, true) && inv.slots[0].count == 64 && inv.slots[1].count == 3,
              "shift-click merges before filling empty storage cells");
        CHECK(inv.Click(1, true, false) && inv.cursor.count == 2 && inv.slots[1].count == 1,
              "right-click splits an odd stack with ceiling half on cursor");
        CHECK(inv.Click(2, true, false) && inv.slots[2].count == 1 && inv.cursor.count == 1,
              "right-click places exactly one item");
        CHECK(inv.Close() && inv.cursor.Empty() && inv.craft[0].Empty(),
              "closing inventory returns cursor and crafting items");
    }
    {
        InventoryState inv;
        inv.craft[0] = {BLOCK_PLANKS, 1};
        inv.craft[1] = {BLOCK_SPRUCE_PLANKS, 1};
        inv.craft[3] = {BLOCK_BIRCH_PLANKS, 1};
        inv.craft[4] = {BLOCK_PLANKS, 1};
        CHECK(inv.Result(false).id == BLOCK_CRAFTING_TABLE,
              "crafting table accepts mixed plank species");
        inv.cursor = {BLOCK_DIRT, 1};
        CHECK(!inv.Click(45, false, false) && inv.craft[0].count == 1,
              "incompatible cursor does not consume ingredients");
        inv.cursor = {BLOCK_CRAFTING_TABLE, 64};
        CHECK(!inv.Click(45, false, false) && inv.craft[4].count == 1,
              "full cursor does not consume ingredients");
        inv.cursor = {};
        CHECK(inv.Click(45, true, false) && inv.cursor.count == 1 && inv.Result(false).Empty(),
              "right-clicking output crafts exactly once");
        inv = InventoryState{};
        inv.craft[4] = {BLOCK_LOG, 2};
        CHECK(inv.Result(false).id == BLOCK_PLANKS, "single-cell recipes work away from grid origin");
        inv.craft[4] = {BLOCK_LOG, 0};
        CHECK(inv.Result(false).Empty(), "zero-count slot never participates in a recipe");
        CHECK(!inv.Click(-1, false, false) && !inv.Click(46, false, false) && !inv.Click(38, false, false),
              "outside and hidden 2x2 cells cannot be clicked");
    }
    {
        InventoryState inv;
        for (int i = 0; i < 9; ++i) if (i != 4) inv.craft[i] = {BLOCK_COBBLE, 2};
        CHECK(inv.Result(false).Empty() && inv.Result(true).id == BLOCK_FURNACE,
              "furnace recipe requires the 3x3 crafting table");
        inv.craft[4] = {BLOCK_COBBLE, 1};
        CHECK(inv.Result(true).Empty(), "filled center invalidates furnace recipe");
        inv.craft[4] = {};
        CHECK(inv.Click(45, false, true, true) && inv.slots[27].id == BLOCK_FURNACE &&
              inv.slots[27].count == 2 && inv.Result(true).Empty(),
              "shift-click crafts only as many furnaces as ingredients allow");
    }
    {
        InventoryState inv;
        for (ItemSlot& slot : inv.slots) slot = {BLOCK_STONE, 64};
        CHECK(inv.Add(BLOCK_DIRT, 5) == 5 && inv.Capacity(BLOCK_DIRT) == 0,
              "full inventory returns uninserted items instead of deleting them");
        inv.cursor = {BLOCK_LOG, 1};
        inv.craft[0] = {BLOCK_LOG, 1};
        CHECK(!inv.Close() && inv.cursor.count == 1 && inv.craft[0].count == 1,
              "failed close preserves cursor and crafting grid");
        CHECK(!inv.Click(45, false, false, true) && inv.craft[0].count == 1,
              "shift-crafting into a full inventory consumes nothing");
        inv.slots[0] = {BLOCK_PLANKS, 61};
        CHECK(!inv.Click(45, false, false, true) && inv.slots[0].count == 61,
              "craft output is atomic when only part of a batch would fit");
        inv.slots[0].count = 60;
        CHECK(inv.Click(45, false, false, true) && inv.slots[0].count == 64 && inv.craft[0].Empty(),
              "craft output succeeds at exact available capacity");
        inv.craft[0] = {BLOCK_LOG, 1};
        inv.slots[1] = {BLOCK_LOG, 63};
        CHECK(!inv.Close() && inv.slots[1].count == 63 && inv.cursor.count == 1 && inv.craft[0].count == 1,
              "close is all-or-nothing when only cursor fits");
    }
    {
        InventoryState inv;
        inv.slots[0] = {BLOCK_DIRT, 37};
        inv.slots[27] = {BLOCK_LOG, 64};
        inv.slots[28] = {BLOCK_STONE, 13};
        std::mt19937 rng(451);
        bool conserved = true;
        for (int n = 0; n < 2000; ++n) {
            const bool table = (rng() & 1) != 0;
            inv.Click(int(rng() % 45), (rng() & 1) != 0, table, (rng() & 1) != 0);
            if (n % 11 == 0) inv.Close();
            int totals[BLOCK_COUNT] = {};
            auto countSlot = [&](const ItemSlot& slot) {
                if (slot.Empty()) { if (slot.id != 0 || slot.count != 0) conserved = false; }
                else {
                    if (slot.count > 64 || slot.id >= BLOCK_COUNT) conserved = false;
                    else totals[slot.id] += slot.count;
                }
            };
            for (const ItemSlot& slot : inv.slots) countSlot(slot);
            for (const ItemSlot& slot : inv.craft) countSlot(slot);
            countSlot(inv.cursor);
            if (totals[BLOCK_DIRT] != 37 || totals[BLOCK_LOG] != 64 || totals[BLOCK_STONE] != 13)
                conserved = false;
        }
        CHECK(conserved, "2000 randomized slot operations preserve items and canonical stack bounds");
    }
    {
        SurvivalState easy, normal, hard, peaceful;
        easy.hunger = normal.hunger = hard.hunger = peaceful.hunger = 0;
        peaceful.health = 5;
        for (int tick = 0; tick < 2000; ++tick) {
            easy.Update(0.1f, false, false, 1);
            normal.Update(0.1f, false, false, 2);
            hard.Update(0.1f, false, false, 3);
            peaceful.Update(0.1f, false, false, 0);
        }
        CHECK(easy.health == 10 && normal.health == 1 && hard.dead && hard.health == 0,
              "difficulty controls starvation floor");
        CHECK(peaceful.health == 20 && peaceful.hunger == 20 && !peaceful.dead,
              "peaceful regenerates health without hunger depletion");
    }

    // --- Atlas ---
    Atlas atlas;
    uint8_t red[16 * 16 * 4], blue[16 * 16 * 4];
    for (int i = 0; i < 256; ++i) {
        red[i * 4] = 255; red[i * 4 + 1] = 0; red[i * 4 + 2] = 0; red[i * 4 + 3] = 255;
        blue[i * 4] = 0; blue[i * 4 + 1] = 0; blue[i * 4 + 2] = 255; blue[i * 4 + 3] = 255;
    }
    const int t0 = atlas.AddCheckerTile("__missing");
    const int t1 = atlas.AddTile("red", red);
    const int t2 = atlas.AddTile("blue", blue);
    CHECK(t0 == 0 && t1 == 1 && t2 == 2, "atlas tile ids sequential");
    const uint8_t* px = atlas.Pixels();
    CHECK(px[0] == 255 && px[2] == 255, "checker fallback occupies tile 0");
    CHECK(px[16 * 4] == 255 && px[16 * 4 + 2] == 0, "red tile lands at x=16");
    CHECK(px[32 * 4 + 2] == 255, "blue tile lands at x=32");
    float u = 0, v = 0;
    CHECK(atlas.GetTileOrigin(t2, u, v) && u == 32.0f / 1024.0f && v == 0.0f, "tile origin UV");
    CHECK(atlas.FindTile("blue") == t2 && atlas.FindTile("nope") == -1, "tile name lookup");
    CHECK(atlas.AddTile("red", red) == t1, "duplicate tile dedup");

    // --- 1.9+ combat cooldown math ---
    AttackCooldown cd;
    CHECK(cd.Strength(1000.0f) == 1.0f, "fully charged after idle");
    const float fullDmg = cd.Swing(1000.0f);
    CHECK(fullDmg == 1.0f, "full-strength swing deals base damage");
    const float spamDmg = cd.Swing(1000.0f);
    CHECK(spamDmg < 0.25f, "instant re-swing suffers the damage penalty");
    CHECK(cd.Strength(1200.0f) == 0.9f, "strength 0.9 at 200ms (crit gate boundary)");
    CHECK(cd.Strength(1250.0f) == 1.0f, "recharged after one 250ms period");
    PlayerState ps;
    ps.UpdateCombat(cd, 1100.0f);
    CHECK(ps.attackCooldownProgress == cd.Strength(1100.0f),
          "player cooldown progress mirrors QPC-driven strength");

    // --- Rigid ring queue ---
    RingQueue<int, 4> ring;
    CHECK(ring.TryPush(1) && ring.TryPush(2) && ring.TryPush(3) && ring.TryPush(4),
          "ring fills to capacity");
    CHECK(!ring.TryPush(5), "ring rejects overflow (backpressure, no allocation)");
    int rv = 0;
    CHECK(ring.TryPop(rv) && rv == 1 && ring.TryPop(rv) && rv == 2, "ring pops FIFO");
    CHECK(ring.TryPush(5) && ring.TryPush(6), "ring wrap-around reuses slots");
    CHECK(ring.TryPop(rv) && rv == 3 && ring.TryPop(rv) && rv == 4 &&
          ring.TryPop(rv) && rv == 5 && ring.TryPop(rv) && rv == 6, "ring drains in order");
    CHECK(!ring.TryPop(rv), "empty ring pop fails");

    // --- Crit particle sweep ---
    ParticleSystem parts;
    parts.SpawnCrit(0, 70, 0);
    CHECK(parts.List().size() == 16, "crit spawns 16 particles");
    for (int i = 0; i < 100; ++i) parts.SpawnCrit(0, 70, 0);
    CHECK(parts.List().size() <= MAX_PARTICLES, "particle count hard-capped");
    parts.Update(2.0f);
    CHECK(parts.List().empty(), "expired particles are swept");

    // --- Block flags ---
    CHECK(BLOCK_FLAGS[BLOCK_AIR] == 0, "air has no flags");
    CHECK((BLOCK_FLAGS[BLOCK_WATER] & BF_LIQUID) && !(BLOCK_FLAGS[BLOCK_WATER] & BF_SOLID),
          "water is liquid and non-solid");
    CHECK((BLOCK_FLAGS[BLOCK_OAK_LEAVES] & BF_CUTOUT) && (BLOCK_FLAGS[BLOCK_OAK_LEAVES] & BF_SOLID),
          "leaves are solid cutout");
    CHECK((BLOCK_FLAGS[BLOCK_STONE] & BF_OPAQUE) && (BLOCK_FLAGS[BLOCK_STONE] & BF_SOLID),
          "stone is solid opaque");
    CHECK((BLOCK_FLAGS[BLOCK_GLASS] & BF_CUTOUT) && !(BLOCK_FLAGS[BLOCK_GLASS] & BF_OPAQUE),
          "glass is cutout, not opaque");

    // --- Greedy mesher emits all six face directions ---
    {
        std::vector<uint8_t> cube(CHUNK_VOL, 0);
        for (int y = 0; y < 16; ++y)
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x)
                    cube[x + 16 * z + 256 * y] = BLOCK_STONE;
        BlockTiles bt;
        MeshData cm;
        BuildChunkMeshCPU(cube.data(), nullptr, nullptr, nullptr, nullptr, bt, cm);
        CHECK(cm.vertices.size() == 24, "solid cube meshes to exactly 6 quads");
        CHECK(cm.indices.size() == 36, "solid cube index count");
    }

    // --- Liquid mesh partition ---
    std::vector<uint8_t> water(CHUNK_VOL, 0);
    water[5 + 16 * 5 + 256 * 64] = BLOCK_WATER;
    MeshData wm;
    BuildChunkMeshCPU(water.data(), nullptr, nullptr, nullptr, nullptr, tiles, wm);
    CHECK(wm.opaqueIndexCount == 0 && wm.indices.size() == 36,
          "isolated water block meshes to 6 liquid quads (opaque count 0)");
    CheckBoxMesh(wm, 5, 64, 5, 6, 65, 6);

    // --- Player physics: gravity, landing, jump ---
    {
        InputState in;
        FlyCamera cam;
        PlayerBody body;
        body.flying = false;
        body.x = 8.5f; body.y = 70.0f; body.z = 8.5f;
        auto ground = [](int, int y, int) -> uint8_t {
            return (y >= 0 && y < 64) ? BLOCK_STONE : BLOCK_AIR;
        };
        for (int i = 0; i < 240; ++i) body.Update(in, 1.0f / 60.0f, cam, ground);
        CHECK(body.onGround && fabsf(body.y - 64.0f) < 0.05f,
              "player falls under gravity and lands on ground at y=64");
        in.keys[VK_SPACE] = true;
        body.Update(in, 1.0f / 60.0f, cam, ground);
        CHECK(body.y > 64.0f, "jump lifts the player off the ground");
    }

    // --- Terrain generation (real async World) ---
    {
        World world(1337, 2);
        world.SetBlockTiles(tiles);
        world.Update(8.5f, 8.5f);
        int sy = -1;
        for (int i = 0; i < 300 && sy < 0; ++i) {
            sy = world.SurfaceY(8, 8);
            if (sy < 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(sy > 0, "terrain generates a surface near spawn");
        CHECK(world.GetBlock(8, 0, 8) == BLOCK_BEDROCK, "bedrock floor at y=0");
        if (sy > 0) CHECK((BLOCK_FLAGS[world.GetBlock(8, sy, 8)] & BF_SOLID) != 0,
                          "surface block is solid");
        std::vector<uint8_t> cb(CHUNK_VOL);
        for (int y = 0; y < CHUNK_Y; ++y)
            for (int z = 0; z < CHUNK_Z; ++z)
                for (int x = 0; x < CHUNK_X; ++x)
                    cb[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y] = world.GetBlock(x, y, z);
        MeshData gm;
        BuildChunkMeshCPU(cb.data(), nullptr, nullptr, nullptr, nullptr, tiles, gm);
        int qy = 0, qx = 0, qz = 0;
        for (size_t q = 0; q + 3 < gm.vertices.size(); q += 4) {
            const MeshVertex& a = gm.vertices[q];
            const MeshVertex& b = gm.vertices[q + 1];
            const MeshVertex& c = gm.vertices[q + 2];
            if (a.y == b.y && b.y == c.y) ++qy;
            else if (a.x == b.x && b.x == c.x) ++qx;
            else ++qz;
        }
        printf("generated chunk mesh: %zu verts, %zu idx, Y=%d X=%d Z=%d\n",
               gm.vertices.size(), gm.indices.size(), qy, qx, qz);
        int exposedTop = 0;
        for (int z = 0; z < CHUNK_Z; ++z) {
            for (int x = 0; x < CHUNK_X; ++x) {
                for (int y = 0; y < CHUNK_Y - 1; ++y) {
                    const uint8_t a = cb[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * y];
                    const uint8_t b = cb[x + CHUNK_X * z + CHUNK_X * CHUNK_Z * (y + 1)];
                    if ((BLOCK_FLAGS[a] & BF_SOLID) && !(BLOCK_FLAGS[b] & BF_SOLID)) ++exposedTop;
                }
            }
        }
        printf("exposed top cells: %d\n", exposedTop);
        CHECK(qy > 0 && qy * 256 >= exposedTop, "greedy top quads cover the exposed surface");
        CHECK(qx + qz > 0, "generated chunk meshes side faces");
    }

    // --- Seed determinism: same seed must rebuild the identical column ---
    {
        World a(424242, 2), b(424242, 2);
        a.SetBlockTiles(tiles);
        b.SetBlockTiles(tiles);
        a.Update(8.5f, 8.5f);
        b.Update(8.5f, 8.5f);
        int sa = -1, sb = -1;
        for (int i = 0; i < 300 && (sa < 0 || sb < 0); ++i) {
            if (sa < 0) sa = a.SurfaceY(8, 8);
            if (sb < 0) sb = b.SurfaceY(8, 8);
            if (sa < 0 || sb < 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(sa > 0 && sa == sb, "same seed produces the same spawn height");
        bool same = sa == sb;
        if (same) {
            for (int y = 0; y <= sa && same; ++y)
                same = a.GetBlock(8, y, 8) == b.GetBlock(8, y, 8);
        }
        CHECK(same, "same seed produces identical blocks (seed drives generation)");
    }

    {
        EditMap changes;
        changes[ChunkKey(-2, 3)][0] = BLOCK_AIR;
        changes[ChunkKey(-2, 3)][65535] = BLOCK_BRICK;
        changes[ChunkKey(0, -1)][32888] = BLOCK_FURNACE;
        const auto bytes = EncodeEdits(changes);
        EditMap loaded;
        CHECK(DecodeEdits(bytes.data(), bytes.size(), loaded) && loaded == changes,
              "block edits round-trip with negative chunks, air and boundary indices");
        CHECK(EncodeEdits(loaded) == bytes, "block edit encoding is deterministic");
        bool truncatedRejected = true;
        for (size_t i = 0; i < bytes.size(); ++i)
            if (DecodeEdits(bytes.data(), i, loaded)) truncatedRejected = false;
        CHECK(truncatedRejected && loaded == changes,
              "all truncated edit streams are rejected without replacing valid edits");
        auto invalid = bytes;
        invalid.back() = 255;
        CHECK(!DecodeEdits(invalid.data(), invalid.size(), loaded), "unknown saved block ids are rejected");
        invalid = bytes;
        invalid.push_back(0);
        CHECK(!DecodeEdits(invalid.data(), invalid.size(), loaded), "trailing saved block bytes are rejected");
        invalid = bytes;
        invalid[4] = 99;
        CHECK(!DecodeEdits(invalid.data(), invalid.size(), loaded), "unsupported edit versions are rejected");
        const auto empty = EncodeEdits({});
        CHECK(DecodeEdits(empty.data(), empty.size(), loaded) && loaded.empty(), "empty world edits round-trip");
    }
    {
        World edited(1337, 2);
        edited.SetBlockTiles(tiles);
        auto waitFor = [&](int cx, int cz) {
            edited.Update(float(cx * 16 + 8), float(cz * 16 + 8));
            for (int i = 0; i < 500; ++i) {
                uint64_t key; MeshData mesh;
                while (edited.PopUpload(key, mesh)) {}
                if (edited.SurfaceY(cx * 16, cz * 16) >= 0) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        };
        CHECK(waitFor(0, 0), "edit fixture chunk is generated");
        edited.SetBlock(1, 0, 1, BLOCK_AIR);
        edited.SetBlock(2, 100, 2, BLOCK_FURNACE);
        const auto saved = edited.SnapshotEdits();
        CHECK(saved.at(ChunkKey(0, 0)).size() == 2, "player edits track removed and placed blocks");
        CHECK(waitFor(20, 20), "streaming reaches a distant chunk");
        CHECK(!edited.HasChunk(0, 0), "edit fixture chunk is evicted");
        CHECK(waitFor(0, 0) && edited.GetBlock(1, 0, 1) == BLOCK_AIR &&
              edited.GetBlock(2, 100, 2) == BLOCK_FURNACE,
              "player edits survive eviction and regeneration");
        World restored(1337, 2);
        restored.SetBlockTiles(tiles);
        restored.SetEdits(saved);
        restored.Update(8.5f, 8.5f);
        for (int i = 0; i < 500 && restored.SurfaceY(8, 8) < 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(restored.GetBlock(1, 0, 1) == BLOCK_AIR && restored.GetBlock(2, 100, 2) == BLOCK_FURNACE,
              "saved edits apply to a fresh world before meshing");
    }

    if (gFail) {
        printf("\n%d TEST(S) FAILED\n", gFail);
        return 1;
    }
    printf("\nALL TESTS PASSED\n");
    return 0;
}