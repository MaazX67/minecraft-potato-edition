#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
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
    UINT opaqueIndexCount = 0; // [0,opaque) solid; [opaque,indexCount) liquid
};

template <typename T> static void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

static InputState gInput;
static HWND gHwnd = nullptr;
static Screen gScreen = Screen::Title;
static Screen gOptionsReturn = Screen::Title;
static Settings gSettings;
static FlyCamera gCamera;
static PlayerBody gBody;
static SurvivalState gSurvival;
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
static ID3D10ShaderResourceView* gWhiteSRV = nullptr;
static ID3D10Buffer* gOutlineVB = nullptr;
static int gHotbarSlot = 0;
static uint8_t gHotbar[9];
static std::string gActionBar;
static float gActionBarUntil = 0;
static float gNowMs = 0;
static float gTimeSec = 0;
static float gFps = 0;
static bool gRunning = true;
static bool gDebug = false;
static bool gFlyHook = false;
static bool gSpawned = false;
static float gFallStartY = 0;
static float gLastSpaceMs = -1000;
static bool gHaveTarget = false;
static int gTargetX = 0, gTargetY = 0, gTargetZ = 0;
static float gStepTimer = 0.3f;
static bool gWasInWater = false;
static std::string gSplash = "Potato Edition!";

static bool IsWorldScreen(Screen s) {
    return s == Screen::Game || s == Screen::Pause || s == Screen::Inventory || s == Screen::Death;
}

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
            gInput.leftDown = true;
            return 0;
        case WM_LBUTTONUP:
            gInput.leftDown = false;
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

static void StartGame() {
    gSurvival = SurvivalState{};
    gBody = PlayerBody{};
    gBody.flying = false;
    gSpawned = false;
    gActionBar.clear();
    SetScreen(gHwnd, Screen::Game);
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
    gSound.SetVolume(gSettings.soundVolume);
    SetBrightness(gRC, gSettings.brightness);
}

static void ApplyFullscreen() {
    static LONG_PTR savedStyle = 0;
    static RECT savedRect = {};
    static bool saved = false;
    if (gSettings.fullscreen) {
        if (!saved) {
            savedStyle = GetWindowLongPtrW(gHwnd, GWL_STYLE);
            GetWindowRect(gHwnd, &savedRect);
            saved = true;
        }
        SetWindowLongPtrW(gHwnd, GWL_STYLE,
                          savedStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX));
        SetWindowPos(gHwnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                     GetSystemMetrics(SM_CYSCREEN), SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else if (saved) {
        SetWindowLongPtrW(gHwnd, GWL_STYLE, savedStyle);
        SetWindowPos(gHwnd, HWND_TOP, savedRect.left, savedRect.top,
                     savedRect.right - savedRect.left, savedRect.bottom - savedRect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

static void HandleMenuAction(int action) {
    if (action != ACT_NONE) gSound.Play(SND_CLICK);
    switch (action) {
        case ACT_SINGLEPLAYER:
        case ACT_RESPAWN:
            StartGame();
            break;
        case ACT_RESUME:
            SetScreen(gHwnd, Screen::Game);
            break;
        case ACT_OPTIONS:
            gOptionsReturn = gScreen;
            SetScreen(gHwnd, Screen::Options);
            break;
        case ACT_OPTIONS_DONE:
            SetScreen(gHwnd, gOptionsReturn);
            break;
        case ACT_QUIT:
            gRunning = false;
            break;
        case ACT_QUIT_TO_TITLE:
            SetScreen(gHwnd, Screen::Title);
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
        case ACT_TOGGLE_BOB:
            gSettings.viewBobbing = !gSettings.viewBobbing;
            break;
        case ACT_TOGGLE_VSYNC:
            gSettings.vsync = !gSettings.vsync;
            break;
        case ACT_TOGGLE_FS:
            gSettings.fullscreen = !gSettings.fullscreen;
            ApplyFullscreen();
            break;
        case ACT_CYCLE_DIFF:
            gSettings.difficulty = (gSettings.difficulty + 1) & 3;
            break;
        default:
            break;
    }
}

static void UploadMeshes() {
    uint64_t key;
    MeshData md;
    int budget = MAX_UPLOADS_PER_FRAME;
    while (budget-- > 0 && gWorld->PopUpload(key, md)) {
        if (!gWorld->HasChunk(KeyCX(key), KeyCZ(key))) continue;
        GpuChunk& gc = gGpu[key];
        SafeRelease(gc.vb);
        SafeRelease(gc.ib);
        gc.indexCount = 0;
        gc.opaqueIndexCount = 0;
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
        gc.opaqueIndexCount = md.opaqueIndexCount;
    }
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
    if (it == gGpu.end()) gSweepCursor = 0;
}

static void SaveScreenshot() {
    ID3D10Texture2D* back = nullptr;
    if (FAILED(gRC.swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return;
    D3D10_TEXTURE2D_DESC bd;
    back->GetDesc(&bd);
    D3D10_TEXTURE2D_DESC sd = bd;
    sd.Usage = D3D10_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D10Texture2D* stage = nullptr;
    HRESULT hr = gRC.device->CreateTexture2D(&sd, nullptr, &stage);
    if (SUCCEEDED(hr)) {
        gRC.device->CopyResource(stage, back);
        D3D10_MAPPED_TEXTURE2D map = {};
        if (SUCCEEDED(stage->Map(0, D3D10_MAP_READ, 0, &map))) {
            CreateDirectoryA("screenshots", nullptr);
            static int shotIdx = 0;
            char path[128];
            snprintf(path, sizeof(path), "screenshots/shot_%03d.bmp", shotIdx++);
            FILE* f = fopen(path, "wb");
            if (f) {
                const uint32_t w = bd.Width, h = bd.Height;
                const uint32_t rowBytes = (w * 3 + 3) & ~3u;
                const uint32_t imgBytes = rowBytes * h;
                uint8_t hdr[54] = {};
                hdr[0] = 'B'; hdr[1] = 'M';
                const uint32_t total = 54 + imgBytes;
                memcpy(hdr + 2, &total, 4);
                const uint32_t off = 54;
                memcpy(hdr + 10, &off, 4);
                const uint32_t ihdr = 40;
                memcpy(hdr + 14, &ihdr, 4);
                memcpy(hdr + 18, &w, 4);
                memcpy(hdr + 22, &h, 4);
                hdr[26] = 1; hdr[28] = 24;
                memcpy(hdr + 34, &imgBytes, 4);
                fwrite(hdr, 1, 54, f);
                std::vector<uint8_t> row(rowBytes);
                for (uint32_t y = h; y-- > 0;) {
                    const uint8_t* src = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch;
                    for (uint32_t x = 0; x < w; ++x) {
                        row[x * 3 + 0] = src[x * 4 + 0];
                        row[x * 3 + 1] = src[x * 4 + 1];
                        row[x * 3 + 2] = src[x * 4 + 2];
                    }
                    fwrite(row.data(), 1, rowBytes, f);
                }
                fclose(f);
                gActionBar = "Saved screenshot";
                gActionBarUntil = gNowMs + 2000;
            }
            stage->Unmap(0);
        }
        stage->Release();
    }
    back->Release();
}

static void HandleGameFrame(float dt) {
    gCamera.Look(gInput.mouseDX, gInput.mouseDY);
    const float cp = cosf(gCamera.pitch);
    gCamera.fwdX = sinf(gCamera.yaw) * cp;
    gCamera.fwdY = sinf(gCamera.pitch);
    gCamera.fwdZ = -cosf(gCamera.yaw) * cp;
    gCamera.rightX = cosf(gCamera.yaw);
    gCamera.rightZ = sinf(gCamera.yaw);

    auto getBlock = [&](int x, int y, int z) -> uint8_t { return gWorld->GetBlock(x, y, z); };

    if (!gSpawned) {
        gWorld->Update(8.5f, 8.5f);
        const int sy = gWorld->SurfaceY(8, 8);
        if (sy < 0) return; // spawn chunk still generating
        gBody.x = 8.5f; gBody.z = 8.5f; gBody.y = float(sy + 1);
        gBody.vx = gBody.vy = gBody.vz = 0;
        gBody.flying = false;
        if (gFlyHook) {
            gBody.y = float(sy + 40);
            gBody.flying = true;
            gCamera.pitch = -1.1f;
        }
        gFallStartY = gBody.y;
        gSpawned = true;
    }

    // Vanilla creative double-tap-space toggles flight.
    if (gInput.KeyPressed(VK_SPACE)) {
        if (gNowMs - gLastSpaceMs < 300.0f) { gBody.flying = !gBody.flying; gBody.vy = 0; }
        gLastSpaceMs = gNowMs;
    }

    const bool wasGround = gBody.onGround;
    gBody.Update(gInput, dt, gCamera, getBlock);
    gCamera.x = gBody.x;
    gCamera.y = gBody.y + PlayerBody::EYE;
    gCamera.z = gBody.z;
    if (gBody.onGround) {
        if (!wasGround && !gBody.flying && !gBody.inWater) {
            const float dist = gFallStartY - gBody.y;
            const int hp = gSurvival.health;
            if (dist > 0) gSurvival.OnLand(dist);
            if (gSurvival.health < hp) gSound.Play(SND_HURT);
        }
        gFallStartY = gBody.y;
    } else if (gBody.y > gFallStartY) {
        gFallStartY = gBody.y;
    }
    const bool moving = (gBody.vx * gBody.vx + gBody.vz * gBody.vz) > 0.01f;
    if (gSettings.difficulty == 0) gSurvival.hunger = 20;
    gSurvival.Update(dt, moving, gBody.sprinting);
    if (moving && gBody.onGround && !gBody.flying && !gBody.inWater) {
        gStepTimer += dt;
        if (gStepTimer >= (gBody.sprinting ? 0.28f : 0.38f)) {
            gStepTimer = 0;
            const uint8_t under = gWorld->GetBlock(int(floorf(gBody.x)),
                                                   int(floorf(gBody.y - 0.5f)),
                                                   int(floorf(gBody.z)));
            gSound.PlayAt(SoundEngine::StepSound(under), gBody.x, gBody.y, gBody.z);
        }
    } else {
        gStepTimer = 0.3f;
    }
    if (gBody.inWater && !gWasInWater)
        gSound.PlayAt(SND_SPLASH, gBody.x, gBody.y, gBody.z);
    gWasInWater = gBody.inWater;
    if (gSettings.viewBobbing && moving && gBody.onGround && !gBody.flying)
        gCamera.y += sinf(gTimeSec * 9.0f) * 0.05f;
    if (gSurvival.dead) { SetScreen(gHwnd, Screen::Death); return; }

    gWorld->Update(gCamera.x, gCamera.z);
    gPlayer.UpdateCombat(gCooldown, gNowMs);
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
    if (gInput.KeyPressed('E')) { SetScreen(gHwnd, Screen::Inventory); return; }
    if (gInput.KeyPressed(VK_F2)) SaveScreenshot();
    if (gInput.KeyPressed(VK_F3)) gDebug = !gDebug;

    int hx, hy, hz, nx, ny, nz;
    gHaveTarget = RaycastDDA(getBlock, gCamera.x, gCamera.y, gCamera.z,
                             gCamera.fwdX, gCamera.fwdY, gCamera.fwdZ, 5.0f,
                             hx, hy, hz, nx, ny, nz);
    if (gHaveTarget) { gTargetX = hx; gTargetY = hy; gTargetZ = hz; }

    if (gInput.leftClick) {
        if (gHaveTarget) {
            const uint8_t old = gWorld->GetBlock(hx, hy, hz);
            const float strength = gCooldown.Strength(gNowMs);
            float dmg = gCooldown.Swing(gNowMs);
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
            gCooldown.Swing(gNowMs);
            gSound.Play(SND_HIT);
        }
    }

    if (gInput.rightClick && gHaveTarget && (nx || ny || nz)) {
        const int px = hx + nx, py = hy + ny, pz = hz + nz;
        if (py >= 0 && py < CHUNK_Y && gWorld->GetBlock(px, py, pz) == BLOCK_AIR) {
            const bool insideBody =
                px + 1 > gBody.x - PlayerBody::HALF && px < gBody.x + PlayerBody::HALF &&
                pz + 1 > gBody.z - PlayerBody::HALF && pz < gBody.z + PlayerBody::HALF &&
                py + 1 > gBody.y && py < gBody.y + PlayerBody::HEIGHT;
            if (!insideBody) {
                const uint8_t block = gHotbar[gHotbarSlot];
                gWorld->SetBlock(px, py, pz, block);
                gSound.PlayAt(SoundEngine::BlockSound(block),
                              float(px) + 0.5f, float(py) + 0.5f, float(pz) + 0.5f);
            }
        }
    }
}

static void DrawSkybox() {
    const float yaw = gTimeSec * 0.06f;
    const XMMATRIX view = XMMatrixMultiply(XMMatrixRotationY(yaw), XMMatrixRotationX(-0.12f));
    const XMMATRIX proj = XMMatrixPerspectiveFovRH(
        1.6f, float(SCREEN_W) / float(SCREEN_H), 0.1f, 100.0f);
    SetViewProj(gRC, XMMatrixMultiply(view, proj));
    gRC.device->PSSetShader(gRC.psSky);
    gRC.device->PSSetShaderResources(1, 1, &gRC.panoramaSRV);
    gRC.device->OMSetDepthStencilState(gRC.depthOff, 0);
    gRC.device->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    const UINT stride = UINT(sizeof(MeshVertex)), offset = 0;
    gRC.device->IASetVertexBuffers(0, 1, &gRC.skyVB, &stride, &offset);
    gRC.device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gRC.device->Draw(36, 0);
    gRC.device->OMSetDepthStencilState(gRC.depthOn, 0);
}

static void DrawBlockOutline() {
    if (!gHaveTarget || !gOutlineVB || !gWhiteSRV) return;
    const float e = 0.003f;
    const float x0 = gTargetX - e, y0 = gTargetY - e, z0 = gTargetZ - e;
    const float x1 = gTargetX + 1 + e, y1 = gTargetY + 1 + e, z1 = gTargetZ + 1 + e;
    const float c[8][3] = {
        {x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},
        {x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1},
    };
    static const int E[24] = {0,1,1,2,2,3,3,0, 4,5,5,6,6,7,7,4, 0,4,1,5,2,6,3,7};
    MeshVertex v[24];
    for (int i = 0; i < 24; ++i) {
        MeshVertex& m = v[i];
        m.x = c[E[i]][0]; m.y = c[E[i]][1]; m.z = c[E[i]][2];
        m.tu = m.tv = m.u = m.v = 0; m.light = 15;
        m.cr = m.cg = m.cb = 0; // black selection box
    }
    void* data = nullptr;
    if (FAILED(gOutlineVB->Map(D3D10_MAP_WRITE_DISCARD, 0, &data))) return;
    memcpy(data, v, sizeof(v));
    gOutlineVB->Unmap();
    gRC.device->PSSetShader(gRC.psUI);
    gRC.device->PSSetShaderResources(0, 1, &gWhiteSRV);
    gRC.device->OMSetDepthStencilState(gRC.depthReadOnly, 0);
    const UINT stride = UINT(sizeof(MeshVertex)), offset = 0;
    gRC.device->IASetVertexBuffers(0, 1, &gOutlineVB, &stride, &offset);
    gRC.device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_LINELIST);
    gRC.device->Draw(24, 0);
    gRC.device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gRC.device->OMSetDepthStencilState(gRC.depthOn, 0);
}

static void DrawWorld() {
    const XMVECTOR eye = XMVectorSet(gCamera.x, gCamera.y, gCamera.z, 1);
    const XMVECTOR at = XMVectorAdd(eye, XMVectorSet(gCamera.fwdX, gCamera.fwdY, gCamera.fwdZ, 0));
    const XMMATRIX view = XMMatrixLookAtRH(eye, at, XMVectorSet(0, 1, 0, 0));
    const XMMATRIX proj = XMMatrixPerspectiveFovRH(
        gSettings.fov * 0.01745329f, float(SCREEN_W) / float(SCREEN_H), 0.1f, 1000.0f);
    const XMMATRIX vp = XMMatrixMultiply(view, proj);
    Frustum frustum;
    ExtractFrustum(vp, frustum);
    SetViewProj(gRC, vp);
    gRC.device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gRC.device->OMSetDepthStencilState(gRC.depthOn, 0);
    gRC.device->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    gRC.device->PSSetShader(gRC.psWorld);
    gRC.device->PSSetShaderResources(0, 1, &gRC.atlasSRV);
    const UINT stride = UINT(sizeof(MeshVertex)), offset = 0;

    // Opaque + cutout pass.
    for (const auto& kv : gGpu) {
        const GpuChunk& gc = kv.second;
        if (!gc.vb || gc.opaqueIndexCount == 0) continue;
        const float minX = float(KeyCX(kv.first) * CHUNK_X);
        const float minZ = float(KeyCZ(kv.first) * CHUNK_Z);
        if (!AABBInFrustum(frustum, minX, 0, minZ, minX + CHUNK_X, float(CHUNK_Y), minZ + CHUNK_Z)) continue;
        gRC.device->IASetVertexBuffers(0, 1, &gc.vb, &stride, &offset);
        gRC.device->IASetIndexBuffer(gc.ib, DXGI_FORMAT_R32_UINT, 0);
        gRC.device->DrawIndexed(gc.opaqueIndexCount, 0, 0);
    }
    // Liquid pass: alpha blended, depth-tested but never writing depth.
    gRC.device->OMSetBlendState(gRC.alphaBlend, nullptr, 0xFFFFFFFF);
    gRC.device->OMSetDepthStencilState(gRC.depthReadOnly, 0);
    for (const auto& kv : gGpu) {
        const GpuChunk& gc = kv.second;
        if (!gc.vb || gc.indexCount <= gc.opaqueIndexCount) continue;
        const float minX = float(KeyCX(kv.first) * CHUNK_X);
        const float minZ = float(KeyCZ(kv.first) * CHUNK_Z);
        if (!AABBInFrustum(frustum, minX, 0, minZ, minX + CHUNK_X, float(CHUNK_Y), minZ + CHUNK_Z)) continue;
        gRC.device->IASetVertexBuffers(0, 1, &gc.vb, &stride, &offset);
        gRC.device->IASetIndexBuffer(gc.ib, DXGI_FORMAT_R32_UINT, 0);
        gRC.device->DrawIndexed(gc.indexCount - gc.opaqueIndexCount, gc.opaqueIndexCount, 0);
    }
    gRC.device->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    gRC.device->OMSetDepthStencilState(gRC.depthOn, 0);

    // Crit particle sweep.
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
                {x0, y1, z0, 0, 0, 0, 0, shade, 1, 1, 1},
                {x1, y1, z1, 0, 0, 1, 0, shade, 1, 1, 1},
                {x1, y0, z1, 0, 0, 1, 1, shade, 1, 1, 1},
                {x0, y1, z0, 0, 0, 0, 0, shade, 1, 1, 1},
                {x1, y0, z1, 0, 0, 1, 1, shade, 1, 1, 1},
                {x0, y0, z0, 0, 0, 0, 1, shade, 1, 1, 1},
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

    if (gScreen == Screen::Game) DrawBlockOutline();
}

static void DrawDebug() {
    char line[160];
    gGui.DrawQuad(&gGui.texBlack, 2, 2, 360, 196, 0, 0, 1, 1, 15);
    int y = 6;
    auto row = [&](const char* s) { gGui.DrawText(s, 6, float(y), 2, 15, true); y += 20; };
    row("Minecraft Potato Edition (DX10 / FL 10_0)");
    snprintf(line, sizeof(line), "%.0f fps  (%d chunks, %zu gpu)", double(gFps),
             int(gWorld->ChunkCount()), gGpu.size());
    row(line);
    snprintf(line, sizeof(line), "XYZ: %.2f / %.2f / %.2f", double(gBody.x), double(gBody.y), double(gBody.z));
    row(line);
    snprintf(line, sizeof(line), "Chunk: %d %d  RD: %d", FloorDiv16(int(gBody.x)), FloorDiv16(int(gBody.z)),
             gSettings.renderDistance);
    row(line);
    snprintf(line, sizeof(line), "Facing: %.1f / %.1f  %s", double(gCamera.yaw * 57.2958f),
             double(gCamera.pitch * 57.2958f), gBody.flying ? "flying" : (gBody.inWater ? "swimming" : "walking"));
    row(line);
    snprintf(line, sizeof(line), "HP: %d  Hunger: %d  Slot: %s", gSurvival.health, gSurvival.hunger,
             BLOCK_DEFS[gHotbar[gHotbarSlot]].name);
    row(line);
    if (gHaveTarget) {
        const uint8_t tb = gWorld->GetBlock(gTargetX, gTargetY, gTargetZ);
        snprintf(line, sizeof(line), "Target: %d %d %d (%s)", gTargetX, gTargetY, gTargetZ,
                 BLOCK_DEFS[tb].name);
        row(line);
        snprintf(line, sizeof(line), "Tiles T/S/B: %d/%d/%d", gTiles.top[tb], gTiles.side[tb],
                 gTiles.bottom[tb]);
        row(line);
    } else {
        row("Target: -");
    }
}

static void RenderFrame() {
    const float sky[4] = {0.47f, 0.65f, 1.0f, 1.0f};
    gRC.device->ClearRenderTargetView(gRC.rtv, sky);
    gRC.device->ClearDepthStencilView(gRC.dsv, D3D10_CLEAR_DEPTH, 1.0f, 0);

    const bool menuBg = (gScreen == Screen::Title || gScreen == Screen::Options);
    if (menuBg && gRC.panoramaSRV) {
        DrawSkybox();
    } else if (IsWorldScreen(gScreen)) {
        DrawWorld();
    }

    const XMMATRIX ortho = XMMatrixOrthographicOffCenterRH(
        0, float(SCREEN_W), float(SCREEN_H), 0, -1, 1);
    SetViewProj(gRC, ortho);
    gGui.BeginFrame();
    if (gScreen == Screen::Game) {
        DrawHUD(gGui, gPlayer.attackCooldownProgress, gHotbarSlot, gHotbar,
                gSurvival.health, gSurvival.hunger,
                gNowMs < gActionBarUntil ? gActionBar : std::string());
        if (gDebug) DrawDebug();
    } else if (gScreen == Screen::Inventory) {
        DrawInventory(gGui, gInput.mouseX, gInput.mouseY, gHotbarSlot, gHotbar);
    } else {
        if (menuBg && !gRC.panoramaSRV) {
            gGui.DrawQuad(&gGui.texDirt, 0, 0, SCREEN_W, SCREEN_H, 0, 0,
                          SCREEN_W / 64.0f, SCREEN_H / 64.0f, 8);
        }
        gMenu.Draw(gGui, gScreen, gSettings, gInput.mouseX, gInput.mouseY, gInput.leftDown, gSplash, gTimeSec);
        if (gMenu.ConsumeChanged()) ApplySettings();
    }
    gGui.Flush(gRC);
    gRC.swapChain->Present(gSettings.vsync ? 1 : 0, 0);
}

static void LoadSplash() {
    std::vector<uint8_t> bytes;
    if (!gStore.ReadFile("minecraft/texts/splashes.txt", bytes)) return;
    std::vector<std::string> lines;
    std::string cur;
    for (uint8_t b : bytes) {
        if (b == '\n') { if (!cur.empty()) lines.push_back(cur); cur.clear(); }
        else if (b != '\r') cur += char(b);
    }
    if (!cur.empty()) lines.push_back(cur);
    if (!lines.empty()) gSplash = lines[size_t(rand()) % lines.size()];
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
    gHwnd = hwnd;

    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = 0;
    rid.hwndTarget = hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    ShowWindow(hwnd, SW_SHOW);

    if (!InitRenderer(hwnd, gRC, gShaderSource)) return 1;

    std::string assetsPath = DefaultAssetIndexPath();
    const char* flag = strstr(GetCommandLineA(), "--assets");
    if (flag) {
        const char* p = flag + 8;
        while (*p == ' ' || *p == '=') ++p;
        std::string parsed;
        if (*p == '"') { ++p; while (*p && *p != '"') { parsed += *p; ++p; } }
        else { while (*p && *p != ' ') { parsed += *p; ++p; } }
        if (!parsed.empty()) assetsPath = parsed;
    }
    const bool storeOk = gStore.LoadIndex(assetsPath);

    std::string jarPath = DefaultClientJarPath();
    const char* jflag = strstr(GetCommandLineA(), "--jar");
    if (jflag) {
        const char* p = jflag + 5;
        while (*p == ' ' || *p == '=') ++p;
        std::string parsed;
        if (*p == '"') { ++p; while (*p && *p != '"') { parsed += *p; ++p; } }
        else { while (*p && *p != ' ') { parsed += *p; ++p; } }
        if (!parsed.empty()) jarPath = parsed;
    }
    const bool jarOk = !jarPath.empty() && gStore.LoadJar(jarPath);
    const int tilesLoaded = (storeOk || jarOk) ? BuildBlockAtlas(gStore, gAtlas) : 0;
    if (tilesLoaded == 0) {
        gAtlas.AddCheckerTile("__missing");
        MessageBoxA(hwnd,
                    "Minecraft assets not found.\n"
                    "Expected the Migurinth launcher asset index and client jar under %APPDATA%.\n"
                    "Running with checker textures. Overrides: --assets <index.json> --jar <client.jar>",
                    "Potato Edition - assets missing", MB_ICONWARNING);
    }
    gRC.atlasSRV = CreateTextureSRV(gRC, gAtlas.Pixels(), ATLAS_SIZE, ATLAS_SIZE);
    for (int b = 1; b < BLOCK_COUNT; ++b) {
        gTiles.top[b] = ResolveTile(BLOCK_DEFS[b].top);
        gTiles.side[b] = ResolveTile(BLOCK_DEFS[b].side);
        gTiles.bottom[b] = ResolveTile(BLOCK_DEFS[b].bottom);
    }

    if (!gGui.Init(gRC, gStore)) return 1;
    gSound.Init(gStore);
    CreatePanorama(gRC, gStore); // optional; null falls back to dirt title
    LoadSplash();
    memcpy(gHotbar, HOTBAR_BLOCKS, sizeof(gHotbar));

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

        const uint8_t white[4] = {255, 255, 255, 255};
        gWhiteSRV = CreateTextureSRV(gRC, white, 1, 1);
        D3D10_BUFFER_DESC obd = {};
        obd.ByteWidth = 24 * UINT(sizeof(MeshVertex));
        obd.Usage = D3D10_USAGE_DYNAMIC;
        obd.BindFlags = D3D10_BIND_VERTEX_BUFFER;
        obd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
        gRC.device->CreateBuffer(&obd, nullptr, &gOutlineVB);
    }

    gWorld = new World(1337, gSettings.renderDistance);
    gWorld->SetBlockTiles(gTiles);
    if (strstr(GetCommandLineA(), "--autostart")) StartGame();
    if (strstr(GetCommandLineA(), "--debug")) gDebug = true;
    if (strstr(GetCommandLineA(), "--fly")) gFlyHook = true;
    if (strstr(GetCommandLineA(), "--show-options")) {
        gOptionsReturn = Screen::Title;
        SetScreen(hwnd, Screen::Options);
    }
    if (strstr(GetCommandLineA(), "--show-inv")) SetScreen(hwnd, Screen::Inventory);

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
        gNowMs = SystemTimeMs();
        gTimeSec += dt;

        if (gInput.KeyPressed(VK_ESCAPE)) {
            if (gScreen == Screen::Game) SetScreen(hwnd, Screen::Pause);
            else if (gScreen == Screen::Pause) SetScreen(hwnd, Screen::Game);
            else if (gScreen == Screen::Inventory) SetScreen(hwnd, Screen::Game);
            else if (gScreen == Screen::Options) SetScreen(hwnd, gOptionsReturn);
        }

        if (gScreen == Screen::Game) {
            HandleGameFrame(dt);
        } else if (gScreen == Screen::Inventory) {
            if (gInput.leftClick) {
                const int b = InventoryHitTest(gInput.mouseX, gInput.mouseY);
                if (b > 0) {
                    gHotbar[gHotbarSlot] = uint8_t(b);
                    gSound.Play(SND_CLICK);
                }
            }
        } else if (gInput.leftClick) {
            HandleMenuAction(gMenu.HitTest(gInput.mouseX, gInput.mouseY));
        }

        if (IsWorldScreen(gScreen)) {
            UploadMeshes();
            gWorld->EvictStep(MAX_EVICTS_PER_FRAME);
        }

        RenderFrame();

        ++fpsFrames;
        fpsTimer += dt;
        if (fpsTimer >= 0.5f) {
            gFps = fpsFrames / fpsTimer;
            char title[128];
            snprintf(title, sizeof(title), "Minecraft Potato Edition - %d FPS", int(gFps + 0.5f));
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
    SafeRelease(gWhiteSRV);
    SafeRelease(gOutlineVB);
    ShutdownRenderer(gRC);
    DestroyWindow(hwnd);
    CoUninitialize();
    return 0;
}