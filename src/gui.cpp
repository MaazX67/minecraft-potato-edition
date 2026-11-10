#include "gui.h"
#include <algorithm>
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
    // Honest fallback: visible checker so missing GUI assets are obvious.
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

    const uint8_t white[4] = {255, 255, 255, 255};
    texWhite.w = texWhite.h = 1;
    texWhite.srv = CreateTextureSRV(rc, white, 1, 1);

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
                        &texCrosshair, &texHotbar, &texHotbarSel, &texFont, &texDirt, &texWhite};
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
                          float u0, float v0, float u1, float v1, float shade) {
    Batch* batch = nullptr;
    for (Batch& b : mBatches) {
        if (b.tex == tex) { batch = &b; break; }
    }
    if (!batch) {
        mBatches.push_back({tex, {}});
        batch = &mBatches.back();
    }
    const float x2 = x + w, y2 = y + h;
    const MeshVertex quad[6] = {
        {x,  y,  0, 0, 0, u0, v0, shade},
        {x2, y,  0, 0, 0, u1, v0, shade},
        {x2, y2, 0, 0, 0, u1, v1, shade},
        {x,  y,  0, 0, 0, u0, v0, shade},
        {x2, y2, 0, 0, 0, u1, v1, shade},
        {x,  y2, 0, 0, 0, u0, v1, shade},
    };
    batch->verts.insert(batch->verts.end(), quad, quad + 6);
}

void GuiRenderer::DrawSprite(const GuiTexture* tex, float x, float y, float scale, float shade) {
    DrawQuad(tex, x, y, tex->w * scale, tex->h * scale, 0, 0, 1, 1, shade);
}

// Approximate vanilla glyph advances (default font is proportional).
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

void GuiRenderer::DrawTextRaw(const std::string& text, float x, float y, float scale, float shade) {
    if (!texFont.srv || texFont.w < 16) return;
    const float cell = float(texFont.w / 16);
    const float invW = 1.0f / float(texFont.w), invH = 1.0f / float(texFont.h);
    float pen = x;
    for (char c : text) {
        if (c >= 32 && c <= 126) {
            const int col = c % 16, row = c / 16;
            const float u0 = col * cell * invW, v0 = row * cell * invH;
            const float u1 = (col + 1) * cell * invW, v1 = (row + 1) * cell * invH;
            DrawQuad(&texFont, pen, y, cell * scale, cell * scale, u0, v0, u1, v1, shade);
        }
        pen += CharWidth(c) * scale;
    }
}

void GuiRenderer::DrawText(const std::string& text, float x, float y, float scale,
                          float shade, bool shadow) {
    if (shadow) DrawTextRaw(text, x + scale, y + scale, scale, shade * 0.25f);
    DrawTextRaw(text, x, y, scale, shade);
}

void GuiRenderer::DrawTextCentered(const std::string& text, float cx, float y, float scale,
                                  float shade, bool shadow) {
    DrawText(text, cx - TextWidth(text, scale) * 0.5f, y, scale, shade, shadow);
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
    ui.DrawTextCentered(b.label, b.x + b.w * 0.5f, b.y + (b.h - 16) * 0.5f, 2,
                        b.enabled ? 14.0f : 7.0f, true);
}

void MenuSystem::Draw(GuiRenderer& ui, Screen screen, const Settings& settings,
                      int mouseX, int mouseY) {
    mButtons.clear();
    const float cx = SCREEN_W * 0.5f;
    char label[96];

    if (screen == Screen::Title) {
        ui.DrawQuad(&ui.texDirt, 0, 0, SCREEN_W, SCREEN_H, 0, 0,
                    SCREEN_W / 64.0f, SCREEN_H / 64.0f, 8);
        ui.DrawSprite(&ui.texLogo, cx - ui.texLogo.w, 30, 2, 15);
        mButtons.push_back({cx - 200, 240, 400, 40, "Singleplayer", ACT_SINGLEPLAYER, true});
        mButtons.push_back({cx - 200, 288, 400, 40, "Multiplayer", ACT_NONE, false});
        mButtons.push_back({cx - 200, 336, 400, 40, "Options...", ACT_OPTIONS, true});
        mButtons.push_back({cx - 200, 384, 400, 40, "Quit Game", ACT_QUIT, true});
        ui.DrawText("Minecraft Potato Edition (fan project)", 4, SCREEN_H - 20, 2, 15, true);
        const char* tag = "DX10 / Feature Level 10_0";
        ui.DrawText(tag, SCREEN_W - ui.TextWidth(tag, 2) - 4, SCREEN_H - 20, 2, 15, true);
    } else if (screen == Screen::Options) {
        ui.DrawQuad(&ui.texDirt, 0, 0, SCREEN_W, SCREEN_H, 0, 0,
                    SCREEN_W / 64.0f, SCREEN_H / 64.0f, 5);
        ui.DrawTextCentered("Options", cx, 30, 2, 15, true);
        snprintf(label, sizeof(label), "Render Distance: %d chunks", settings.renderDistance);
        mButtons.push_back({cx - 155, 110, 310, 40, label, ACT_CYCLE_RD, true});
        snprintf(label, sizeof(label), "FOV: %d", int(settings.fov));
        mButtons.push_back({cx - 155, 160, 310, 40, label, ACT_CYCLE_FOV, true});
        snprintf(label, sizeof(label), "Sensitivity: %d%%", int(settings.sensitivity / 0.0025f * 100 + 0.5f));
        mButtons.push_back({cx - 155, 210, 310, 40, label, ACT_CYCLE_SENS, true});
        snprintf(label, sizeof(label), "Sound: %s", settings.sound ? "ON" : "OFF");
        mButtons.push_back({cx - 155, 260, 310, 40, label, ACT_TOGGLE_SOUND, true});
        mButtons.push_back({cx - 155, 330, 310, 40, "Done", ACT_OPTIONS_DONE, true});
    } else if (screen == Screen::Pause) {
        ui.DrawTextCentered("Game Menu", cx, 60, 2, 15, true);
        mButtons.push_back({cx - 200, 200, 400, 40, "Back to Game", ACT_RESUME, true});
        mButtons.push_back({cx - 200, 248, 400, 40, "Options...", ACT_OPTIONS, true});
        mButtons.push_back({cx - 200, 296, 400, 40, "Quit to Title", ACT_QUIT_TO_TITLE, true});
    }

    for (const Button& b : mButtons) DrawButton(ui, b, mouseX, mouseY);
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

void DrawHUD(GuiRenderer& ui, float attackStrength, int selectedSlot, const std::string& actionBar) {
    const float cx = SCREEN_W * 0.5f, cy = SCREEN_H * 0.5f;
    ui.DrawSprite(&ui.texCrosshair, cx - 15, cy - 15, 2, 15);

    // 1.9+ attack recharge indicator under the crosshair.
    if (attackStrength < 1.0f) {
        ui.DrawQuad(&ui.texWhite, cx - 18, cy + 24, 36, 4, 0, 0, 1, 1, 3);
        ui.DrawQuad(&ui.texWhite, cx - 18, cy + 24, 36 * attackStrength, 4, 0, 0, 1, 1, 15);
    }

    const float hotbarX = cx - 182, hotbarY = SCREEN_H - 44.0f;
    ui.DrawSprite(&ui.texHotbar, hotbarX, hotbarY, 2, 15);
    ui.DrawSprite(&ui.texHotbarSel, hotbarX + selectedSlot * 40 - 2, hotbarY - 2, 2, 15);
    for (int i = 0; i < 9; ++i) {
        const GuiTexture& icon = ui.texBlockIcon[HOTBAR_BLOCKS[i]];
        ui.DrawSprite(&icon, hotbarX + i * 40 + 4, hotbarY + 6, 2, 15);
    }
    if (!actionBar.empty()) {
        ui.DrawTextCentered(actionBar, cx, hotbarY - 26, 2, 15, true);
    }
}

} // namespace potato