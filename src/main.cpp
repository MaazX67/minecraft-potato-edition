#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include "engine.h"
#include "renderer.h"
#include "assets.h"
#include "world.h"
#include "sound.h"
#include "gui.h"
#include "shader_embed.h"

using namespace potato;
using namespace DirectX;

struct GpuChunk {
    ID3D10Buffer* vb = nullptr;
    ID3D10Buffer* ib = nullptr;
    UINT indexCount = 0;
};

template <typename T> static void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

static InputState gInput;
static Screen gScreen = Screen::Title;
static Screen gOptionsReturn = Screen::Title;
static Settings gSettings;
static FlyCamera gCamera;
static AttackCooldown gCooldown;
static PlayerState gPlayer;
static ParticleSystem gParticles;
static RenderContext gRC;
static Atlas gAtlas;
static AssetStore gStore;
static BlockTiles gTiles;
static GuiRenderer gGui;
static MenuSystem gMenu;
static SoundEngine gSound;
static World* gWorld = nullptr;
static std::unordered_map<uint64_t, GpuChunk> gGpu;
static uint64_t gSweepCursor = 0;
static ID3D10ShaderResourceView* gParticleSRV = nullptr;
static ID3D10Buffer* gParticleVB = nullptr;
static int gHotbarSlot = 0;
static std::string gActionBar;
static float gActionBarUntil = 0;
static float gNowMs = 0;
static bool gRunning = true;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INPUT: {
            UINT size = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, nullptr, &size,
                            sizeof(RAWINPUTHEADER));
            BYTE buf[128];
            if (size == 0 || size > sizeof(buf)) break;
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, buf, &size,
                                sizeof(RAWINPUTHEADER)) != size) break;
            const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
            if (ri->header.dwType == RIM_TYPEMOUSE) {
                gInput.mouseDX += float(ri->data.mouse.lLastX);
                gInput.mouseDY += float(ri->data.mouse.lLastY);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            gInput.mouseX = GET_X_LPARAM(lp);
            gInput.mouseY = GET_Y_LPARAM(lp);
            return 0;
        case WM_KEYDOWN:
            gInput.keys[wp & 0xFF] = true;
            return 0;
        case WM_KEYUP:
            gInput.keys[wp & 0xFF] = false;
            return 0;
        case WM_LBUTTONDOWN:
            gInput.leftClick = true;
            return 0;
        case WM_RBUTTONDOWN:
            gInput.rightClick = true;
            return 0;
        case WM_MOUSEWHEEL:
            gInput.wheel += float(GET_WHEEL_DELTA_WPARAM(wp)) / 120.0f;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

static void SetScreen(HWND hwnd, Screen s) {
    gScreen = s;
    const bool inGame = s == Screen::Game;
    ShowCursor(inGame ? FALSE : TRUE);
    if (inGame) {
        RECT client;
        GetClientRect(hwnd, &client);
        POINT tl = {0, 0};
        POINT br = {client.right, client.bottom};
        ClientToScreen(hwnd, &tl);
        ClientToScreen(hwnd, &br);
        const RECT clip = {tl.x, tl.y, br.x, br.y};
        ClipCursor(&clip);
    } else {
        ClipCursor(nullptr);
    }
}

static int ResolveTile(const char* tex) {
    if (!tex || !tex[0]) return 0;
    const int t = gAtlas.FindTile(std::string("minecraft/textures/block/") + tex + ".png");
    return t < 0 ? 0 : t;
}

static void ApplySettings() {
    gWorld->SetRenderDistance(gSettings.renderDistance);
    gCamera.sensitivity = gSettings.sensitivity;
    gSound.SetEnabled(gSettings.sound);
}

static void HandleMenuAction(HWND hwnd, int action) {
    switch (action) {
        case ACT_SINGLEPLAYER:
        case ACT_RESUME:
            SetScreen(hwnd, Screen::Game);
            break;
        case ACT_OPTIONS:
            gOptionsReturn = gScreen;
            SetScreen(hwnd, Screen::Options);
            break;
        case ACT_OPTIONS_DONE:
            SetScreen(hwnd, gOptionsReturn);
            break;
        case ACT_QUIT:
            gRunning = false;
            break;
        case ACT_QUIT_TO_TITLE:
            SetScreen(hwnd, Screen::Title);
            break;
        case ACT_CYCLE_RD:
            gSettings.renderDistance += 2;
            if (gSettings.renderDistance > 12) gSettings.renderDistance = 4;
            ApplySettings();
            break;
        case ACT_CYCLE_FOV:
            gSettings.fov += 10;
            if (gSettings.fov > 100) gSettings.fov = 60;
            break;
        case ACT_CYCLE_SENS: {
            static const int steps[] = {50, 75, 100, 125, 150, 200};
            const int pct = int(gSettings.sensitivity / 0.0025f * 100 + 0.5f);
            int idx = 2;
            for (int i = 0; i < 6; ++i) if (steps[i] == pct) idx = i;
            gSettings.sensitivity = steps[(idx + 1) % 6] * 0.000025f;
            ApplySettings();
            break;
        }
        case ACT_TOGGLE_SOUND:
            gSettings.sound = !gSettings.sound;
            ApplySettings();
            break;
        default:
            break;
    }
}

static void UploadMeshes() {
    uint64_t key;
    MeshData md;
    int budget = MAX_UPLOADS_PER_FRAME; // hard ceiling: no GPU stall spikes
    while (budget-- > 0 && gWorld->PopUpload(key, md)) {
        if (!gWorld->HasChunk(KeyCX(key), KeyCZ(key))) continue; // evicted in flight
        GpuChunk& gc = gGpu[key];
        SafeRelease(gc.vb);
        SafeRelease(gc.ib);
        gc.indexCount = 0;
        if (md.vertices.empty()) continue;
        D3D10_BUFFER_DESC vd = {};
        vd.ByteWidth = UINT(md.vertices.size() * sizeof(MeshVertex));
        vd.Usage = D3D10_USAGE_IMMUTABLE;
        vd.BindFlags = D3D10_BIND_VERTEX_BUFFER;
        D3D10_SUBRESOURCE_DATA vsub = {};
        vsub.pSysMem = md.vertices.data();
        if (FAILED(gRC.device->CreateBuffer(&vd, &vsub, &gc.vb))) continue;
        D3D10_BUFFER_DESC id = {};
        id.ByteWidth = UINT(md.indices.size() * sizeof(uint32_t));
        id.Usage = D3D10_USAGE_IMMUTABLE;
        id.BindFlags = D3D10_BIND_INDEX_BUFFER;
        D3D10_SUBRESOURCE_DATA isub = {};
        isub.pSysMem = md.indices.data();
        if (FAILED(gRC.device->CreateBuffer(&id, &isub, &gc.ib))) {
            SafeRelease(gc.vb);
            continue;
        }
        gc.indexCount = UINT(md.indices.size());
    }
    // Time-sliced GPU cleanup: examine at most MAX_GPU_SWEEP_PER_FRAME entries
    // per frame, resuming from the cursor key. Erase only invalidates the
    // erased iterator, and a rehash just restarts the sweep at begin().
    auto it = gGpu.begin();
    if (gSweepCursor != 0) {
        const auto resume = gGpu.find(gSweepCursor);
        if (resume != gGpu.end()) it = resume;
    }
    int checks = 0;
    while (it != gGpu.end() && checks < MAX_GPU_SWEEP_PER_FRAME) {
        if (!gWorld->HasChunk(KeyCX(it->first), KeyCZ(it->first))) {
            SafeRelease(it->second.vb);
            SafeRelease(it->second.ib);
            it = gGpu.erase(it);
        } else {
            gSweepCursor = it->first;
            ++it;
        }
        ++checks;
    }
    if (it == gGpu.end()) gSweepCursor = 0; // full wrap: restart next frame
}

static void HandleGameFrame(float dt) {
    gCamera.Look(gInput.mouseDX, gInput.mouseDY);
    gCamera.Update(gInput, dt);
    gWorld->Update(gCamera.x, gCamera.z);
    gPlayer.UpdateCombat(gCooldown, gNowMs); // QPC-driven charge indicator
    gParticles.Update(dt);
    gSound.SetListener(gCamera.x, gCamera.y, gCamera.z,
                       gCamera.fwdX, gCamera.fwdY, gCamera.fwdZ,
                       gCamera.rightX, gCamera.rightZ);

    for (int i = 0; i < 9; ++i) {
        if (gInput.KeyPressed('1' + i)) gHotbarSlot = i;
    }
    if (gInput.wheel != 0) {
        gHotbarSlot = (gHotbarSlot + (gInput.wheel > 0 ? -1 : 1) + 9) % 9;
    }

    auto getBlock = [&](int x, int y, int z) -> uint8_t { return gWorld->GetBlock(x, y, z); };
    int hx, hy, hz, nx, ny, nz;

    if (gInput.leftClick) {
        if (RaycastDDA(getBlock, gCamera.x, gCamera.y, gCamera.z,
                       gCamera.fwdX, gCamera.fwdY, gCamera.fwdZ, 5.0f,
                       hx, hy, hz, nx, ny, nz)) {
            const uint8_t old = gWorld->GetBlock(hx, hy, hz);
            // 1.9+ combat: damage scales with recharge; spam clicking is weak.
            const float strength = gCooldown.Strength(gNowMs);
            float dmg = gCooldown.Swing(gNowMs);
            // Vanilla crit gate: fully charged (strength > 0.9) AND falling.
            // Short-cycled clicks get the damage penalty and no particle sweep.
            const bool crit = strength > 0.9f && gCamera.vy < 0;
            if (crit) {
                dmg *= 1.5f;
                gParticles.SpawnCrit(float(hx) + 0.5f, float(hy) + 0.5f, float(hz) + 0.5f);
                gSound.Play(SND_CRIT);
            } else {
                gSound.Play(SND_HIT);
            }
            gWorld->SetBlock(hx, hy, hz, BLOCK_AIR);
            gSound.PlayAt(SoundEngine::BlockSound(old),
                          float(hx) + 0.5f, float(hy) + 0.5f, float(hz) + 0.5f);
            char buf[64];
            snprintf(buf, sizeof(buf), crit ? "Critical! %.1f damage" : "%.1f damage", double(dmg));
            gActionBar = buf;
            gActionBarUntil = gNowMs + 1000;
        } else {
            gCooldown.Swing(gNowMs); // whiff still resets the cooldown (vanilla rule)
            gSound.Play(SND_HIT);
        }
    }

    if (gInput.rightClick) {
        if (RaycastDDA(getBlock, gCamera.x, gCamera.y, gCamera.z,
                       gCamera.fwdX, gCamera.fwdY, gCamera.fwdZ, 5.0f,
                       hx, hy, hz, nx, ny, nz) && (nx || ny || nz)) {
            const int px = hx + nx, py = hy + ny, pz = hz + nz;
            if (py >= 0 && py < CHUNK_Y && gWorld->GetBlock(px, py, pz) == BLOCK_AIR) {
                const bool insideCamera =
                    px + 1 > gCamera.x - 0.3f && px < gCamera.x + 0.3f &&
                    pz + 1 > gCamera.z - 0.3f && pz < gCamera.z + 0.3f &&
                    py + 1 > gCamera.y - 1.6f && py < gCamera.y + 0.2f;
                if (!insideCamera) {
                    const uint8_t block = HOTBAR_BLOCKS[gHotbarSlot];
                    gWorld->SetBlock(px, py, pz, block);
                    gSound.PlayAt(SoundEngine::BlockSound(block),
                                  float(px) + 0.5f, float(py) + 0.5f, float(pz) + 0.5f);
                }
            }
        }
    }
}

static void RenderFrame() {
    const float sky[4] = {0.47f, 0.65f, 1.0f, 1.0f};
    gRC.device->ClearRenderTargetView(gRC.rtv, sky);
    gRC.device->ClearDepthStencilView(gRC.dsv, D3D10_CLEAR_DEPTH, 1.0f, 0);

    if (gScreen == Screen::Game || gScreen == Screen::Pause) {
        const XMVECTOR eye = XMVectorSet(gCamera.x, gCamera.y, gCamera.z, 1);
        const XMVECTOR at = XMVectorAdd(eye, XMVectorSet(gCamera.fwdX, gCamera.fwdY, gCamera.fwdZ, 0));
        const XMMATRIX view = XMMatrixLookAtRH(eye, at, XMVectorSet(0, 1, 0, 0));
        const XMMATRIX proj = XMMatrixPerspectiveFovRH(
            gSettings.fov * 0.01745329f, float(SCREEN_W) / float(SCREEN_H), 0.1f, 1000.0f);
        const XMMATRIX vp = XMMatrixMultiply(view, proj);
        Frustum frustum;
        ExtractFrustum(vp, frustum);
        SetViewProj(gRC, vp);
        gRC.device->OMSetDepthStencilState(gRC.depthOn, 0);
        gRC.device->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        gRC.device->PSSetShader(gRC.psWorld);
        gRC.device->PSSetShaderResources(0, 1, &gRC.atlasSRV);
        const UINT stride = UINT(sizeof(MeshVertex)), offset = 0;
        for (const auto& kv : gGpu) {
            const GpuChunk& gc = kv.second;
            if (!gc.vb || gc.indexCount == 0) continue;
            const float minX = float(KeyCX(kv.first) * CHUNK_X);
            const float minZ = float(KeyCZ(kv.first) * CHUNK_Z);
            if (!AABBInFrustum(frustum, minX, 0, minZ,
                               minX + CHUNK_X, float(CHUNK_Y), minZ + CHUNK_Z)) continue;
            gRC.device->IASetVertexBuffers(0, 1, &gc.vb, &stride, &offset);
            gRC.device->IASetIndexBuffer(gc.ib, DXGI_FORMAT_R32_UINT, 0);
            gRC.device->DrawIndexed(gc.indexCount, 0, 0);
        }

        // Crit particle sweep: camera-facing quads, depth-tested but never
        // writing depth, alpha-blended with life fade. PSMainUI samples any
        // bound sprite with plain 0..1 UVs, so no extra shader is needed.
        const auto& parts = gParticles.List();
        if (!parts.empty() && gParticleSRV && gParticleVB) {
            gRC.device->PSSetShader(gRC.psUI);
            gRC.device->PSSetShaderResources(0, 1, &gParticleSRV);
            gRC.device->OMSetDepthStencilState(gRC.depthReadOnly, 0);
            gRC.device->OMSetBlendState(gRC.alphaBlend, nullptr, 0xFFFFFFFF);
            MeshVertex verts[MAX_PARTICLES * 6];
            size_t n = 0;
            for (const ParticleSystem::Particle& p : parts) {
                const float s = 0.15f;
                const float rx = gCamera.rightX * s, rz = gCamera.rightZ * s;
                const float shade = 15.0f * (p.life / p.maxLife);
                const float x0 = p.x - rx, z0 = p.z - rz;
                const float x1 = p.x + rx, z1 = p.z + rz;
                const float y0 = p.y - s, y1 = p.y + s;
                const MeshVertex q[6] = {
                    {x0, y1, z0, 0, 0, 0, 0, shade},
                    {x1, y1, z1, 0, 0, 1, 0, shade},
                    {x1, y0, z1, 0, 0, 1, 1, shade},
                    {x0, y1, z0, 0, 0, 0, 0, shade},
                    {x1, y0, z1, 0, 0, 1, 1, shade},
                    {x0, y0, z0, 0, 0, 0, 1, shade},
                };
                memcpy(verts + n, q, sizeof(q));
                n += 6;
            }
            void* data = nullptr;
            if (SUCCEEDED(gParticleVB->Map(D3D10_MAP_WRITE_DISCARD, 0, &data))) {
                memcpy(data, verts, n * sizeof(MeshVertex));
                gParticleVB->Unmap();
                const UINT pstride = UINT(sizeof(MeshVertex)), poffset = 0;
                gRC.device->IASetVertexBuffers(0, 1, &gParticleVB, &pstride, &poffset);
                gRC.device->Draw(UINT(n), 0);
            }
            gRC.device->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            gRC.device->OMSetDepthStencilState(gRC.depthOn, 0);
        }
    }

    // Orthographic UI pass: Z-buffer disabled, alpha blend, pixel coordinates.
    const XMMATRIX ortho = XMMatrixOrthographicOffCenterRH(
        0, float(SCREEN_W), float(SCREEN_H), 0, -1, 1);
    SetViewProj(gRC, ortho);
    gGui.BeginFrame();
    if (gScreen == Screen::Game) {
        DrawHUD(gGui, gPlayer.attackCooldownProgress, gHotbarSlot,
                gNowMs < gActionBarUntil ? gActionBar : std::string());
    } else {
        gMenu.Draw(gGui, gScreen, gSettings, gInput.mouseX, gInput.mouseY);
    }
    gGui.Flush(gRC);
    gRC.swapChain->Present(0, 0); // vsync off
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = "PotatoEdition";
    if (!RegisterClassExA(&wc)) return 1;

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, SCREEN_W, SCREEN_H};
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExA(0, "PotatoEdition", "Minecraft Potato Edition", style,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01; // generic desktop
    rid.usUsage = 0x02;     // mouse
    rid.dwFlags = 0;
    rid.hwndTarget = hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    ShowWindow(hwnd, SW_SHOW);

    if (!InitRenderer(hwnd, gRC, gShaderSource)) return 1;

    // Asset index: default verified Migurinth path, overridable via --assets <path>.
    std::string assetsPath = DefaultAssetIndexPath();
    const char* flag = strstr(GetCommandLineA(), "--assets");
    if (flag) {
        const char* p = flag + 8;
        while (*p == ' ' || *p == '=') ++p;
        std::string parsed;
        if (*p == '"') {
            ++p;
            while (*p && *p != '"') { parsed += *p; ++p; }
        } else {
            while (*p && *p != ' ') { parsed += *p; ++p; }
        }
        if (!parsed.empty()) assetsPath = parsed;
    }

    const bool storeOk = gStore.LoadIndex(assetsPath);
    const int tilesLoaded = storeOk ? BuildBlockAtlas(gStore, gAtlas) : 0;
    if (tilesLoaded == 0) {
        gAtlas.AddCheckerTile("__missing");
        MessageBoxA(hwnd,
                    "Minecraft asset index not found or empty.\n"
                    "Running with checker textures. Use --assets <path-to-index.json> to override.",
                    "Potato Edition - assets missing", MB_ICONWARNING);
    }
    gRC.atlasSRV = CreateTextureSRV(gRC, gAtlas.Pixels(), ATLAS_SIZE, ATLAS_SIZE);
    for (int b = 1; b < BLOCK_COUNT; ++b) {
        gTiles.top[b] = ResolveTile(BLOCK_DEFS[b].top);
        gTiles.side[b] = ResolveTile(BLOCK_DEFS[b].side);
        gTiles.bottom[b] = ResolveTile(BLOCK_DEFS[b].bottom);
    }

    if (!gGui.Init(gRC, gStore)) return 1;
    gSound.Init(gStore); // optional: silence on failure

    // Vanilla crit particle sprite; missing file simply disables the sweep.
    {
        std::vector<uint8_t> bytes, px;
        int pw = 0, ph = 0;
        if (gStore.ReadFile("minecraft/textures/particle/critical_hit.png", bytes) &&
            DecodePNGAny(bytes, px, pw, ph)) {
            gParticleSRV = CreateTextureSRV(gRC, px.data(), pw, ph);
        }
        D3D10_BUFFER_DESC pbd = {};
        pbd.ByteWidth = MAX_PARTICLES * 6 * UINT(sizeof(MeshVertex));
        pbd.Usage = D3D10_USAGE_DYNAMIC;
        pbd.BindFlags = D3D10_BIND_VERTEX_BUFFER;
        pbd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
        gRC.device->CreateBuffer(&pbd, nullptr, &gParticleVB);
    }

    gWorld = new World(1337, gSettings.renderDistance);
    gWorld->SetBlockTiles(gTiles);

    DeltaClock clock;
    float fpsTimer = 0;
    int fpsFrames = 0;
    MSG msg = {};
    while (gRunning) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { gRunning = false; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!gRunning) break;

        const float dt = clock.Tick();
        gNowMs = SystemTimeMs(); // raw QPC: combat cooldown never sees clamped dt

        if (gInput.KeyPressed(VK_ESCAPE)) {
            if (gScreen == Screen::Game) SetScreen(hwnd, Screen::Pause);
            else if (gScreen == Screen::Pause) SetScreen(hwnd, Screen::Game);
            else if (gScreen == Screen::Options) SetScreen(hwnd, gOptionsReturn);
        }

        if (gScreen == Screen::Game) {
            HandleGameFrame(dt);
        } else if (gInput.leftClick) {
            HandleMenuAction(hwnd, gMenu.HitTest(gInput.mouseX, gInput.mouseY));
        }
        if (gScreen == Screen::Game || gScreen == Screen::Pause) {
            UploadMeshes();
            gWorld->EvictStep(MAX_EVICTS_PER_FRAME); // time-sliced subchunk cleanup
        }

        RenderFrame();

        ++fpsFrames;
        fpsTimer += dt;
        if (fpsTimer >= 0.5f) {
            char title[128];
            snprintf(title, sizeof(title), "Minecraft Potato Edition - %d FPS",
                     int(fpsFrames / fpsTimer + 0.5f));
            SetWindowTextA(hwnd, title);
            fpsFrames = 0;
            fpsTimer = 0;
        }
        gInput.EndFrame();
    }

    for (auto& kv : gGpu) {
        SafeRelease(kv.second.vb);
        SafeRelease(kv.second.ib);
    }
    gGpu.clear();
    delete gWorld;
    gSound.Shutdown();
    gGui.Shutdown();
    SafeRelease(gParticleSRV);
    SafeRelease(gParticleVB);
    ShutdownRenderer(gRC);
    DestroyWindow(hwnd);
    CoUninitialize();
    return 0;
}