#include "gui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace potato {

const uint8_t HOTBAR_BLOCKS[9] = {
    BLOCK_GRASS, BLOCK_DIRT, BLOCK_STONE, BLOCK_COBBLE, BLOCK_PLANKS,
    BLOCK_LOG, BLOCK_SAND, BLOCK_GRAVEL, BLOCK_BRICK
};

// ---------------------------------------------------------------------------
// GuiRenderer
// ---------------------------------------------------------------------------

GuiTexture GuiRenderer::LoadGui(RenderContext& rc, AssetStore& store, const char* name) {
    GuiTexture t;
    std::vector<uint8_t> bytes, px;
    if (store.ReadFile(name, bytes) && DecodePNGAny(bytes, px, t.w, t.h)) {
        t.srv = CreateTextureSRV(rc, px.data(), t.w, t.h);
        if (t.srv) return t;
    }
    uint8_t checker[16 * 16 * 4];
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const bool m = ((x / 8) + (y / 8)) % 2 == 0;
            uint8_t* p = checker + (y * 16 + x) * 4;
            p[0] = m ? 255 : 0; p[1] = 0; p[2] = m ? 255 : 0; p[3] = 255;
        }
    }
    t.w = t.h = 16;
    t.srv = CreateTextureSRV(rc, checker, 16, 16);
    return t;
}

bool GuiRenderer::Init(RenderContext& rc, AssetStore& store) {
    texButton         = LoadGui(rc, store, "minecraft/textures/gui/sprites/widget/button.png");
    texButtonHi       = LoadGui(rc, store, "minecraft/textures/gui/sprites/widget/button_highlighted.png");
    texButtonDisabled = LoadGui(rc, store, "minecraft/textures/gui/sprites/widget/button_disabled.png");
    texLogo           = LoadGui(rc, store, "minecraft/textures/gui/title/minecraft.png");
    texCrosshair      = LoadGui(rc, store, "minecraft/textures/gui/sprites/hud/crosshair.png");
    texHotbar         = LoadGui(rc, store, "minecraft/textures/gui/sprites/hud/hotbar.png");
    texHotbarSel      = LoadGui(rc, store, "minecraft/textures/gui/sprites/hud/hotbar_selection.png");
    texFont           = LoadGui(rc, store, "minecraft/textures/font/ascii.png");
    texDirt           = LoadGui(rc, store, "minecraft/textures/block/dirt.png");
    texHeart[0]       = LoadGui(rc, store, "minecraft/textures/gui/sprites/hud/heart/container.png");
    texHeart[1]       = LoadGui(rc, store, "minecraft/textures/gui/sprites/hud/heart/full.png");
    texHeart[2]       = LoadGui(rc, store, "minecraft/textures/gui/sprites/hud/heart/half.png");
    texFood[0]        = LoadGui(rc, store, "minecraft/textures/gui/sprites/hud/food_empty.png");
    texFood[1]        = LoadGui(rc, store, "minecraft/textures/gui/sprites/hud/food_full.png");
    texFood[2]        = LoadGui(rc, store, "minecraft/textures/gui/sprites/hud/food_half.png");
    texAttackBg       = LoadGui(rc, store, "minecraft/textures/gui/sprites/hud/crosshair_attack_indicator_background.png");
    texAttackProgress = LoadGui(rc, store, "minecraft/textures/gui/sprites/hud/crosshair_attack_indicator_progress.png");
    texPanoramaOverlay= LoadGui(rc, store, "minecraft/textures/gui/title/background/panorama_overlay.png");
    texSlider         = LoadGui(rc, store, "minecraft/textures/gui/sprites/widget/slider.png");
    texSliderHi       = LoadGui(rc, store, "minecraft/textures/gui/sprites/widget/slider_highlighted.png");
    texSliderHandle   = LoadGui(rc, store, "minecraft/textures/gui/sprites/widget/slider_handle.png");
    texSliderHandleHi = LoadGui(rc, store, "minecraft/textures/gui/sprites/widget/slider_handle_highlighted.png");

    const uint8_t white[4] = {255, 255, 255, 255};
    texWhite.w = texWhite.h = 1;
    texWhite.srv = CreateTextureSRV(rc, white, 1, 1);
    const uint8_t black[4] = {0, 0, 0, 160};
    texBlack.w = texBlack.h = 1;
    texBlack.srv = CreateTextureSRV(rc, black, 1, 1);

    for (int b = 1; b < BLOCK_COUNT; ++b) {
        const std::string name = std::string("minecraft/textures/block/") + BLOCK_DEFS[b].side + ".png";
        texBlockIcon[b] = LoadGui(rc, store, name.c_str());
    }

    D3D10_BUFFER_DESC bd = {};
    bd.ByteWidth = VB_VERTS * UINT(sizeof(MeshVertex));
    bd.Usage = D3D10_USAGE_DYNAMIC;
    bd.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    return SUCCEEDED(rc.device->CreateBuffer(&bd, nullptr, &mVB));
}

void GuiRenderer::Shutdown() {
    GuiTexture* all[] = {&texButton, &texButtonHi, &texButtonDisabled, &texLogo,
                        &texCrosshair, &texHotbar, &texHotbarSel, &texFont, &texDirt,
                        &texWhite, &texBlack, &texAttackBg, &texAttackProgress, &texPanoramaOverlay,
                        &texSlider, &texSliderHi, &texSliderHandle, &texSliderHandleHi,
                        &texHeart[0], &texHeart[1], &texHeart[2],
                        &texFood[0], &texFood[1], &texFood[2]};
    for (GuiTexture* t : all) {
        if (t->srv) { t->srv->Release(); t->srv = nullptr; }
    }
    for (int b = 0; b < BLOCK_COUNT; ++b) {
        if (texBlockIcon[b].srv) { texBlockIcon[b].srv->Release(); texBlockIcon[b].srv = nullptr; }
    }
    if (mVB) { mVB->Release(); mVB = nullptr; }
}

void GuiRenderer::BeginFrame() {
    mBatches.clear();
}

void GuiRenderer::DrawQuad(const GuiTexture* tex, float x, float y, float w, float h,
                          float u0, float v0, float u1, float v1, float shade,
                          float cr, float cg, float cb) {
    // Preserve submission order: only coalesce with the immediately previous
    // batch. Keying batches globally by texture would let an early font batch
    // (splash) draw before later button quads, hiding the button labels.
    Batch* batch = nullptr;
    if (!mBatches.empty() && mBatches.back().tex == tex) batch = &mBatches.back();
    if (!batch) {
        mBatches.push_back({tex, {}});
        batch = &mBatches.back();
    }
    const float x2 = x + w, y2 = y + h;
    const MeshVertex quad[6] = {
        {x,  y,  0, 0, 0, u0, v0, shade, cr, cg, cb},
        {x2, y,  0, 0, 0, u1, v0, shade, cr, cg, cb},
        {x2, y2, 0, 0, 0, u1, v1, shade, cr, cg, cb},
        {x,  y,  0, 0, 0, u0, v0, shade, cr, cg, cb},
        {x2, y2, 0, 0, 0, u1, v1, shade, cr, cg, cb},
        {x,  y2, 0, 0, 0, u0, v1, shade, cr, cg, cb},
    };
    batch->verts.insert(batch->verts.end(), quad, quad + 6);
}

void GuiRenderer::DrawSprite(const GuiTexture* tex, float x, float y, float scale, float shade) {
    DrawQuad(tex, x, y, tex->w * scale, tex->h * scale, 0, 0, 1, 1, shade);
}

static int CharWidth(char c) {
    switch (c) {
        case ' ': return 4;
        case 'i': case '!': case '.': case ',': case ':': case ';': case '|': return 2;
        case 'l': case '\'': return 3;
        case 't': case 'I': case '[': case ']': return 4;
        case 'f': case 'k': case '"': case '(': case ')': case '*': return 5;
        case 'm': case 'w': case '@': case '~': return 7;
        default: return 6;
    }
}

float GuiRenderer::TextWidth(const std::string& text, float scale) const {
    int units = 0;
    for (char c : text) units += CharWidth(c);
    return units * scale;
}

void GuiRenderer::DrawTextRaw(const std::string& text, float x, float y, float scale, float shade,
                             float cr, float cg, float cb) {
    if (!texFont.srv || texFont.w < 16) return;
    const float cell = float(texFont.w / 16);
    const float invW = 1.0f / float(texFont.w), invH = 1.0f / float(texFont.h);
    float pen = x;
    for (char c : text) {
        if (c >= 32 && c <= 126) {
            const int col = c % 16, row = c / 16;
            const float u0 = col * cell * invW, v0 = row * cell * invH;
            const float u1 = (col + 1) * cell * invW, v1 = (row + 1) * cell * invH;
            DrawQuad(&texFont, pen, y, cell * scale, cell * scale, u0, v0, u1, v1, shade, cr, cg, cb);
        }
        pen += CharWidth(c) * scale;
    }
}

void GuiRenderer::DrawText(const std::string& text, float x, float y, float scale,
                          float shade, bool shadow, float cr, float cg, float cb) {
    if (shadow) DrawTextRaw(text, x + scale, y + scale, scale, shade, cr * 0.25f, cg * 0.25f, cb * 0.25f);
    DrawTextRaw(text, x, y, scale, shade, cr, cg, cb);
}

void GuiRenderer::DrawTextCentered(const std::string& text, float cx, float y, float scale,
                                  float shade, bool shadow, float cr, float cg, float cb) {
    DrawText(text, cx - TextWidth(text, scale) * 0.5f, y, scale, shade, shadow, cr, cg, cb);
}

void GuiRenderer::DrawQuadPts(const GuiTexture* tex, const float p[4][2],
                              float u0, float v0, float u1, float v1, float shade,
                              float cr, float cg, float cb) {
    Batch* batch = nullptr;
    if (!mBatches.empty() && mBatches.back().tex == tex) batch = &mBatches.back();
    if (!batch) {
        mBatches.push_back({tex, {}});
        batch = &mBatches.back();
    }
    const MeshVertex quad[6] = {
        {p[0][0], p[0][1], 0, 0, 0, u0, v0, shade, cr, cg, cb},
        {p[1][0], p[1][1], 0, 0, 0, u1, v0, shade, cr, cg, cb},
        {p[2][0], p[2][1], 0, 0, 0, u1, v1, shade, cr, cg, cb},
        {p[0][0], p[0][1], 0, 0, 0, u0, v0, shade, cr, cg, cb},
        {p[2][0], p[2][1], 0, 0, 0, u1, v1, shade, cr, cg, cb},
        {p[3][0], p[3][1], 0, 0, 0, u0, v1, shade, cr, cg, cb},
    };
    batch->verts.insert(batch->verts.end(), quad, quad + 6);
}

void GuiRenderer::DrawTextRotated(const std::string& text, float cx, float cy, float scale,
                                  float angle, float shade, bool shadow,
                                  float cr, float cg, float cb) {
    if (!texFont.srv || texFont.w < 16) return;
    const float cell = float(texFont.w / 16);
    const float invW = 1.0f / float(texFont.w), invH = 1.0f / float(texFont.h);
    const float ca = cosf(angle), sa = sinf(angle);
    const float halfW = TextWidth(text, scale) * 0.5f;
    for (int pass = shadow ? 0 : 1; pass < 2; ++pass) {
        const float ox = pass == 0 ? scale : 0.0f, oy = pass == 0 ? scale : 0.0f;
        const float pr = pass == 0 ? cr * 0.25f : cr;
        const float pg = pass == 0 ? cg * 0.25f : cg;
        const float pb = pass == 0 ? cb * 0.25f : cb;
        float pen = -halfW;
        for (char c : text) {
            if (c >= 32 && c <= 126) {
                const int col = c % 16, row = c / 16;
                const float u0 = col * cell * invW, v0 = row * cell * invH;
                const float u1 = (col + 1) * cell * invW, v1 = (row + 1) * cell * invH;
                const float w = cell * scale, h = cell * scale;
                const float local[4][2] = {{pen, 0}, {pen + w, 0}, {pen + w, h}, {pen, h}};
                float pts[4][2];
                for (int i = 0; i < 4; ++i) {
                    pts[i][0] = cx + ox + local[i][0] * ca - local[i][1] * sa;
                    pts[i][1] = cy + oy + local[i][0] * sa + local[i][1] * ca;
                }
                DrawQuadPts(&texFont, pts, u0, v0, u1, v1, shade, pr, pg, pb);
            }
            pen += CharWidth(c) * scale;
        }
    }
}

void GuiRenderer::Flush(RenderContext& rc) {
    if (mBatches.empty()) return;
    rc.device->PSSetShader(rc.psUI);
    rc.device->OMSetDepthStencilState(rc.depthOff, 0);
    rc.device->OMSetBlendState(rc.alphaBlend, nullptr, 0xFFFFFFFF);
    const UINT stride = UINT(sizeof(MeshVertex)), offset = 0;
    rc.device->IASetVertexBuffers(0, 1, &mVB, &stride, &offset);
    for (const Batch& b : mBatches) {
        if (b.verts.empty() || !b.tex || !b.tex->srv) continue;
        rc.device->PSSetShaderResources(0, 1, &b.tex->srv);
        size_t done = 0;
        while (done < b.verts.size()) {
            const size_t n = std::min(b.verts.size() - done, size_t(VB_VERTS));
            void* data = nullptr;
            if (FAILED(mVB->Map(D3D10_MAP_WRITE_DISCARD, 0, &data))) break;
            memcpy(data, b.verts.data() + done, n * sizeof(MeshVertex));
            mVB->Unmap();
            rc.device->Draw(UINT(n), 0);
            done += n;
        }
    }
    rc.device->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    rc.device->OMSetDepthStencilState(rc.depthOn, 0);
}

// ---------------------------------------------------------------------------
// MenuSystem
// ---------------------------------------------------------------------------

void MenuSystem::DrawButton(GuiRenderer& ui, const Button& b, int mouseX, int mouseY) {
    const bool hover = b.enabled && mouseX >= b.x && mouseX < b.x + b.w &&
                       mouseY >= b.y && mouseY < b.y + b.h;
    const GuiTexture* tex = !b.enabled ? &ui.texButtonDisabled : (hover ? &ui.texButtonHi : &ui.texButton);
    ui.DrawQuad(tex, b.x, b.y, b.w, b.h, 0, 0, 1, 1, 15);
    float cr = 0.878f, cg = 0.878f, cb = 0.878f;       // 0xE0E0E0
    if (!b.enabled) { cr = cg = cb = 0.627f; }          // 0xA0A0A0
    else if (hover) { cr = 1.0f; cg = 1.0f; cb = 0.627f; } // 0xFFFFA0
    ui.DrawTextCentered(b.label, b.x + b.w * 0.5f, b.y + (b.h - 16) * 0.5f, 2, 15, true, cr, cg, cb);
}

void MenuSystem::Draw(GuiRenderer& ui, Screen screen, Settings& settings,
                      int mouseX, int mouseY, bool leftDown, const std::string& splash, float timeSec) {
    mButtons.clear();
    mSliders.clear();
    const float cx = SCREEN_W * 0.5f;
    char label[96];

    if (screen == Screen::Title) {
        if (ui.texPanoramaOverlay.srv)
            ui.DrawQuad(&ui.texPanoramaOverlay, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 1, 1, 15);
        ui.DrawSprite(&ui.texLogo, cx - ui.texLogo.w, 60, 2.0f, 15);
        ui.DrawTextCentered("POTATO EDITION", cx, 60 + ui.texLogo.h * 2.0f + 4, 2, 15, true);
        const float pulse = 2.0f * (1.0f + 0.06f * sinf(timeSec * 6.0f));
        ui.DrawTextRotated(splash, cx + 190, 140, pulse, -0.349f, 15, true, 1, 1, 0);
        mButtons.push_back({cx - 200, 288, 400, 40, "Singleplayer", ACT_SINGLEPLAYER, true});
        mButtons.push_back({cx - 200, 336, 400, 40, "Multiplayer", ACT_NONE, false});
        mButtons.push_back({cx - 200, 456, 196, 40, "Options...", ACT_OPTIONS, true});
        mButtons.push_back({cx + 4, 456, 196, 40, "Quit Game", ACT_QUIT, true});
        ui.DrawText("Minecraft Potato Edition (fan project)", 4, SCREEN_H - 20, 2, 15, true);
        const char* tag = "DX10 / Feature Level 10_0";
        ui.DrawText(tag, SCREEN_W - ui.TextWidth(tag, 2) - 4, SCREEN_H - 20, 2, 15, true);
    } else if (screen == Screen::Options) {
        ui.DrawTextCentered("Options", cx, 30, 2, 15, true);
        mSliders.push_back({SL_FOV, cx - 310, 80, 300, 40});
        mSliders.push_back({SL_RD, cx + 10, 80, 300, 40});
        mSliders.push_back({SL_SENS, cx - 310, 130, 300, 40});
        mSliders.push_back({SL_SOUND, cx + 10, 130, 300, 40});
        mSliders.push_back({SL_MUSIC, cx - 310, 180, 300, 40});
        mSliders.push_back({SL_BRIGHT, cx + 10, 180, 300, 40});
        snprintf(label, sizeof(label), "View Bobbing: %s", settings.viewBobbing ? "ON" : "OFF");
        mButtons.push_back({cx - 310, 230, 300, 40, label, ACT_TOGGLE_BOB, true});
        snprintf(label, sizeof(label), "VSync: %s", settings.vsync ? "ON" : "OFF");
        mButtons.push_back({cx + 10, 230, 300, 40, label, ACT_TOGGLE_VSYNC, true});
        snprintf(label, sizeof(label), "Fullscreen: %s", settings.fullscreen ? "ON" : "OFF");
        mButtons.push_back({cx - 310, 280, 300, 40, label, ACT_TOGGLE_FS, true});
        static const char* DIFF[] = {"Peaceful", "Easy", "Normal", "Hard"};
        snprintf(label, sizeof(label), "Difficulty: %s", DIFF[settings.difficulty & 3]);
        mButtons.push_back({cx + 10, 280, 300, 40, label, ACT_CYCLE_DIFF, true});
        mButtons.push_back({cx - 310, 330, 300, 40, "Chat Settings...", ACT_NONE, false});
        mButtons.push_back({cx + 10, 330, 300, 40, "Resource Packs...", ACT_NONE, false});
        mButtons.push_back({cx - 200, 400, 400, 40, "Done", ACT_OPTIONS_DONE, true});
        for (const Slider& s : mSliders) DrawSlider(ui, s, settings, mouseX, mouseY, leftDown);
    } else if (screen == Screen::Pause) {
        ui.DrawQuad(&ui.texBlack, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 1, 1, 15);
        ui.DrawTextCentered("Game Menu", cx, 60, 2, 15, true);
        mButtons.push_back({cx - 200, 180, 400, 40, "Back to Game", ACT_RESUME, true});
        mButtons.push_back({cx - 200, 228, 196, 40, "Options...", ACT_OPTIONS, true});
        mButtons.push_back({cx + 4, 228, 196, 40, "Advancements", ACT_NONE, false});
        mButtons.push_back({cx - 200, 276, 400, 40, "Quit to Title", ACT_QUIT_TO_TITLE, true});
    } else if (screen == Screen::Death) {
        ui.DrawQuad(&ui.texBlack, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 1, 1, 15, 1.0f, 0.25f, 0.2f);
        ui.DrawTextCentered("You died!", cx, 120, 3, 15, true);
        ui.DrawTextCentered("Score: 0", cx, 200, 2, 15, true, 1, 1, 0);
        mButtons.push_back({cx - 200, 280, 400, 40, "Respawn", ACT_RESPAWN, true});
        mButtons.push_back({cx - 200, 328, 400, 40, "Title Screen", ACT_QUIT_TO_TITLE, true});
    }

    for (const Button& b : mButtons) DrawButton(ui, b, mouseX, mouseY);
}

static float SliderT(const Settings& s, int id) {
    switch (id) {
        case SL_FOV: return (s.fov - 30.0f) / 80.0f;
        case SL_RD: return (s.renderDistance - 2) / 14.0f;
        case SL_SENS: return s.sensitivity / 0.005f;
        case SL_SOUND: return s.soundVolume;
        case SL_MUSIC: return s.musicVolume;
        default: return s.brightness;
    }
}

static void SetSliderT(Settings& s, int id, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    switch (id) {
        case SL_FOV: s.fov = 30.0f + t * 80.0f; break;
        case SL_RD: s.renderDistance = 2 + int(t * 14.0f + 0.5f); break;
        case SL_SENS: s.sensitivity = t * 0.005f; break;
        case SL_SOUND: s.soundVolume = t; break;
        case SL_MUSIC: s.musicVolume = t; break;
        default: s.brightness = t; break;
    }
}

static void SliderLabel(const Settings& s, int id, char* out, size_t n) {
    switch (id) {
        case SL_FOV: snprintf(out, n, "FOV: %d", int(s.fov + 0.5f)); break;
        case SL_RD: snprintf(out, n, "Render Distance: %d", s.renderDistance); break;
        case SL_SENS: snprintf(out, n, "Sensitivity: %d%%", int(s.sensitivity / 0.0025f * 100 + 0.5f)); break;
        case SL_SOUND: snprintf(out, n, "Sound: %d%%", int(s.soundVolume * 100 + 0.5f)); break;
        case SL_MUSIC: snprintf(out, n, "Music: %d%%", int(s.musicVolume * 100 + 0.5f)); break;
        default: snprintf(out, n, "Brightness: %d%%", int(s.brightness * 100 + 0.5f)); break;
    }
}

void MenuSystem::DrawSlider(GuiRenderer& ui, const Slider& s, Settings& settings,
                            int mouseX, int mouseY, bool leftDown) {
    const bool hover = mouseX >= s.x && mouseX < s.x + s.w &&
                       mouseY >= s.y && mouseY < s.y + s.h;
    if (leftDown && hover) {
        SetSliderT(settings, s.id, (mouseX - s.x - 8.0f) / (s.w - 16.0f));
        mChanged = true;
    }
    ui.DrawQuad(hover ? &ui.texSliderHi : &ui.texSlider, s.x, s.y, s.w, s.h, 0, 0, 1, 1, 15);
    const float hx = s.x + SliderT(settings, s.id) * (s.w - 16.0f);
    ui.DrawSprite(hover ? &ui.texSliderHandleHi : &ui.texSliderHandle, hx, s.y, 2, 15);
    char label[96];
    SliderLabel(settings, s.id, label, sizeof(label));
    ui.DrawTextCentered(label, s.x + s.w * 0.5f, s.y + (s.h - 16) * 0.5f, 2, 15, true);
}

int MenuSystem::HitTest(int mouseX, int mouseY) const {
    for (const Button& b : mButtons) {
        if (b.enabled && mouseX >= b.x && mouseX < b.x + b.w &&
            mouseY >= b.y && mouseY < b.y + b.h) {
            return b.action;
        }
    }
    return ACT_NONE;
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------

void DrawHotbar(GuiRenderer& ui, int selectedSlot, const uint8_t* hotbar) {
    const float cx = SCREEN_W * 0.5f;
    const float hotbarX = cx - 182, hotbarY = SCREEN_H - 44.0f;
    ui.DrawSprite(&ui.texHotbar, hotbarX, hotbarY, 2, 15);
    ui.DrawSprite(&ui.texHotbarSel, hotbarX + selectedSlot * 40 - 2, hotbarY - 2, 2, 15);
    for (int i = 0; i < 9; ++i) {
        const GuiTexture& icon = ui.texBlockIcon[hotbar[i]];
        ui.DrawSprite(&icon, hotbarX + i * 40 + 4, hotbarY + 6, 2, 15);
    }
}

void DrawHUD(GuiRenderer& ui, float attackStrength, int selectedSlot,
             const uint8_t* hotbar, int health, int hunger, const std::string& actionBar) {
    const float cx = SCREEN_W * 0.5f, cy = SCREEN_H * 0.5f;
    ui.DrawSprite(&ui.texCrosshair, cx - 15, cy - 15, 2, 15);

    // 1.9+ attack recharge indicator under the crosshair (hidden when full).
    if (attackStrength < 1.0f) {
        ui.DrawSprite(&ui.texAttackBg, cx - 16, cy + 22, 2, 15);
        ui.DrawQuad(&ui.texAttackProgress, cx - 16, cy + 22, 32 * attackStrength, 8,
                    0, 0, attackStrength, 1, 15);
    }

    const float rowY = SCREEN_H - 78.0f;
    for (int i = 0; i < 10; ++i) {
        const float hx = cx - 182 + i * 16;
        ui.DrawSprite(&ui.texHeart[0], hx, rowY, 2, 15);
        if (health >= i * 2 + 2) ui.DrawSprite(&ui.texHeart[1], hx, rowY, 2, 15);
        else if (health == i * 2 + 1) ui.DrawSprite(&ui.texHeart[2], hx, rowY, 2, 15);
        const float fx = cx + 182 - 18 - i * 16;
        ui.DrawSprite(&ui.texFood[0], fx, rowY, 2, 15);
        if (hunger >= i * 2 + 2) ui.DrawSprite(&ui.texFood[1], fx, rowY, 2, 15);
        else if (hunger == i * 2 + 1) ui.DrawSprite(&ui.texFood[2], fx, rowY, 2, 15);
    }

    DrawHotbar(ui, selectedSlot, hotbar);
    if (!actionBar.empty()) {
        ui.DrawTextCentered(actionBar, cx, SCREEN_H - 70.0f, 2, 15, true);
    }
}

// ---------------------------------------------------------------------------
// Creative inventory picker
// ---------------------------------------------------------------------------

namespace {
constexpr int INV_COLS = 9;
constexpr int INV_CELL = 40;
constexpr int INV_ROWS = (BLOCK_COUNT - 1 + INV_COLS - 1) / INV_COLS;
constexpr float INV_PAD = 12;
constexpr float INV_W = INV_COLS * INV_CELL + INV_PAD * 2;
constexpr float INV_TITLE_H = 40;
constexpr float INV_HINT_H = 36;
constexpr float INV_H = INV_TITLE_H + INV_ROWS * INV_CELL + INV_HINT_H;
constexpr float INV_X = (SCREEN_W - INV_W) * 0.5f;
constexpr float INV_Y = (SCREEN_H - INV_H) * 0.5f - 20;
constexpr float INV_GX = INV_X + INV_PAD;
constexpr float INV_GY = INV_Y + INV_TITLE_H;

void CellRect(int block, float& x, float& y) {
    const int i = block - 1;
    x = INV_GX + (i % INV_COLS) * INV_CELL;
    y = INV_GY + (i / INV_COLS) * INV_CELL;
}
} // namespace

int InventoryHitTest(int mouseX, int mouseY) {
    for (int b = 1; b < BLOCK_COUNT; ++b) {
        float x, y;
        CellRect(b, x, y);
        if (mouseX >= x && mouseX < x + INV_CELL && mouseY >= y && mouseY < y + INV_CELL)
            return b;
    }
    return -1;
}

void DrawInventory(GuiRenderer& ui, int mouseX, int mouseY, int selectedSlot, const uint8_t* hotbar) {
    ui.DrawQuad(&ui.texBlack, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 1, 1, 15);

    // Beveled gray panel (light top/left, dark bottom/right).
    ui.DrawQuad(&ui.texWhite, INV_X, INV_Y, INV_W, INV_H, 0, 0, 1, 1, 15, 0.78f, 0.78f, 0.78f);
    ui.DrawQuad(&ui.texWhite, INV_X, INV_Y, INV_W, 2, 0, 0, 1, 1, 15, 1, 1, 1);
    ui.DrawQuad(&ui.texWhite, INV_X, INV_Y, 2, INV_H, 0, 0, 1, 1, 15, 1, 1, 1);
    ui.DrawQuad(&ui.texWhite, INV_X, INV_Y + INV_H - 2, INV_W, 2, 0, 0, 1, 1, 15, 0.33f, 0.33f, 0.33f);
    ui.DrawQuad(&ui.texWhite, INV_X + INV_W - 2, INV_Y, 2, INV_H, 0, 0, 1, 1, 15, 0.33f, 0.33f, 0.33f);
    ui.DrawText("Creative Inventory", INV_X + INV_PAD, INV_Y + 12, 2, 15, false, 0.25f, 0.25f, 0.25f);

    const int hovered = InventoryHitTest(mouseX, mouseY);
    for (int b = 1; b < BLOCK_COUNT; ++b) {
        float x, y;
        CellRect(b, x, y);
        ui.DrawQuad(&ui.texWhite, x + 2, y + 2, INV_CELL - 4, INV_CELL - 4, 0, 0, 1, 1, 15, 0.55f, 0.55f, 0.55f);
        ui.DrawQuad(&ui.texWhite, x + 2, y + 2, INV_CELL - 4, 1, 0, 0, 1, 1, 15, 0.35f, 0.35f, 0.35f);
        ui.DrawQuad(&ui.texWhite, x + 2, y + 2, 1, INV_CELL - 4, 0, 0, 1, 1, 15, 0.35f, 0.35f, 0.35f);
        if (b == hovered)
            ui.DrawQuad(&ui.texHotbarSel, x, y, INV_CELL, INV_CELL, 0, 0, 1, 1, 15);
        ui.DrawSprite(&ui.texBlockIcon[b], x + 4, y + 4, 2, 15);
    }

    ui.DrawText("Click a block to select it", INV_X + INV_PAD, INV_Y + INV_H - 26, 2, 15, false, 0.3f, 0.3f, 0.3f);

    if (hovered > 0) {
        std::string name = BLOCK_DEFS[hovered].name;
        for (char& c : name) if (c == '_') c = ' ';
        const float tw = ui.TextWidth(name, 2);
        float tx = mouseX + 16, ty = mouseY - 12;
        if (tx + tw + 12 > SCREEN_W) tx = SCREEN_W - tw - 12;
        if (ty < 4) ty = 4;
        ui.DrawQuad(&ui.texBlack, tx - 6, ty - 6, tw + 12, 28, 0, 0, 1, 1, 15, 0.1f, 0.05f, 0.2f);
        ui.DrawText(name, tx, ty, 2, 15, true);
    }

    DrawHotbar(ui, selectedSlot, hotbar);
}

} // namespace potato