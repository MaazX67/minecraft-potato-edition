#include "gui.h"
#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace potato {

const uint8_t HOTBAR_BLOCKS[9] = {
    BLOCK_GRASS, BLOCK_DIRT, BLOCK_STONE, BLOCK_COBBLE, BLOCK_PLANKS,
    BLOCK_LOG, BLOCK_SAND, BLOCK_GRAVEL, BLOCK_BRICK
};

namespace {
struct BundledSkin { const char* name; const char* file; };
const BundledSkin SKINS[] = {
    {"Steve", "steve.png"}, {"Alex", "alex.png"}, {"Ari", "ari.png"},
    {"Efe", "efe.png"}, {"Kai", "kai.png"}, {"Makena", "makena.png"},
    {"Noor", "noor.png"}, {"Sunny", "sunny.png"}, {"Zuri", "zuri.png"}
};
constexpr int SKIN_COUNT = int(sizeof(SKINS) / sizeof(SKINS[0]));

void LoadSpriteScaling(AssetStore& store, const char* name, GuiTexture& tex) {
    std::vector<uint8_t> bytes;
    if (!store.ReadFile(std::string(name) + ".mcmeta", bytes)) return;
    try {
        const auto meta = nlohmann::json::parse(bytes.begin(), bytes.end());
        const auto& s = meta.at("gui").at("scaling");
        if (s.value("type", std::string()) != "nine_slice") return;
        const int w = s.at("width").get<int>(), h = s.at("height").get<int>();
        const auto& border = s.at("border");
        int l, t, r, b;
        if (border.is_number_integer()) l = t = r = b = border.get<int>();
        else {
            l = border.at("left").get<int>(); t = border.at("top").get<int>();
            r = border.at("right").get<int>(); b = border.at("bottom").get<int>();
        }
        const bool stretch = s.value("stretch_inner", false);
        if (w <= 0 || h <= 0 || l < 0 || t < 0 || r < 0 || b < 0 ||
            l >= w || r >= w - l || t >= h || b >= h - t) return;
        tex.sliceW = w; tex.sliceH = h;
        tex.borderLeft = l; tex.borderTop = t; tex.borderRight = r; tex.borderBottom = b;
        tex.stretchInner = stretch;
    } catch (const nlohmann::json::exception&) {
        // Missing/malformed scaling metadata must not prevent loading the sprite.
    }
}

// Explicit tiling keeps cropped edge UVs valid even with a clamped sampler.
void DrawTiled(GuiRenderer& ui, const GuiTexture& tex, float x, float y,
               float w, float h, float scale = 2.0f) {
    if (!tex.srv || tex.w <= 0 || tex.h <= 0 || scale <= 0) return;
    const float tw = tex.w * scale, th = tex.h * scale;
    for (float dy = 0; dy < h; dy += th) {
        for (float dx = 0; dx < w; dx += tw) {
            const float cw = std::min(tw, w - dx), ch = std::min(th, h - dy);
            ui.DrawQuad(&tex, x + dx, y + dy, cw, ch, 0, 0, cw / tw, ch / th, 15);
        }
    }
}
} // namespace

bool IsValidPlayerName(const std::string& name) {
    if (name.size() < 3 || name.size() > 16) return false;
    for (unsigned char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

const char* SkinName(int skin) {
    return skin >= 0 && skin < SKIN_COUNT ? SKINS[skin].name : "Unknown";
}

// ---------------------------------------------------------------------------
// GuiRenderer
// ---------------------------------------------------------------------------

GuiTexture GuiRenderer::LoadGui(RenderContext& rc, AssetStore& store, const char* name) {
    GuiTexture t;
    std::vector<uint8_t> bytes, px;
    if (store.ReadFile(name, bytes) && DecodePNGAny(bytes, px, t.w, t.h)) {
        t.srv = CreateTextureSRV(rc, px.data(), t.w, t.h);
        if (t.srv) {
            LoadSpriteScaling(store, name, t);
            return t;
        }
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

bool GuiRenderer::LoadPlayerSkin(RenderContext& rc, AssetStore& store, const PlayerProfile& profile) {
    if (profile.skin < 0 || profile.skin >= SKIN_COUNT) return false;
    const std::string path = std::string("minecraft/textures/entity/player/") +
        (profile.slim ? "slim/" : "wide/") + SKINS[profile.skin].file;
    std::vector<uint8_t> bytes, pixels;
    int w = 0, h = 0;
    if (!store.ReadFile(path, bytes) || !DecodePNGAny(bytes, pixels, w, h) || w != 64 || h != 64)
        return false;
    ID3D10ShaderResourceView* texture = CreateTextureSRV(rc, pixels.data(), w, h);
    if (!texture) return false;
    if (texSkin.srv) texSkin.srv->Release();
    texSkin = {};
    texSkin.srv = texture;
    texSkin.w = w; texSkin.h = h;
    mSkinSlim = profile.slim;
    return true;
}

bool GuiRenderer::Init(RenderContext& rc, AssetStore& store) {
    texButton         = LoadGui(rc, store, "minecraft/textures/gui/sprites/widget/button.png");
    texButtonHi       = LoadGui(rc, store, "minecraft/textures/gui/sprites/widget/button_highlighted.png");
    texButtonDisabled = LoadGui(rc, store, "minecraft/textures/gui/sprites/widget/button_disabled.png");
    texLogo           = LoadGui(rc, store, "minecraft/textures/gui/title/minecraft.png");
    texEdition        = LoadGui(rc, store, "minecraft/textures/gui/title/edition.png");
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
    texInvBg          = LoadGui(rc, store, "minecraft/textures/gui/container/inventory.png");
    texCraftBg        = LoadGui(rc, store, "minecraft/textures/gui/container/crafting_table.png");
    texMenuBg         = LoadGui(rc, store, "minecraft/textures/gui/menu_background.png");
    texHeaderSeparator= LoadGui(rc, store, "minecraft/textures/gui/header_separator.png");
    texFooterSeparator= LoadGui(rc, store, "minecraft/textures/gui/footer_separator.png");
    texTab            = LoadGui(rc, store, "minecraft/textures/gui/sprites/widget/tab.png");
    texTabHi          = LoadGui(rc, store, "minecraft/textures/gui/sprites/widget/tab_highlighted.png");
    texTabSelected    = LoadGui(rc, store, "minecraft/textures/gui/sprites/widget/tab_selected.png");
    texTabSelectedHi  = LoadGui(rc, store, "minecraft/textures/gui/sprites/widget/tab_selected_highlighted.png");

    const uint8_t white[4] = {255, 255, 255, 255};
    texWhite.w = texWhite.h = 1;
    texWhite.srv = CreateTextureSRV(rc, white, 1, 1);
    const uint8_t black[4] = {0, 0, 0, 160};
    texBlack.w = texBlack.h = 1;
    texBlack.srv = CreateTextureSRV(rc, black, 1, 1);

    for (int id = 1; id < 256; ++id) {
        if (IsValidItem(id)) texItemIcon[id] = LoadGui(rc, store, ItemTexture(uint8_t(id)).c_str());
    }
    for (int i = 0; i < 10; ++i) {
        const std::string name = "minecraft/textures/block/destroy_stage_" + std::to_string(i) + ".png";
        texDestroy[i] = LoadGui(rc, store, name.c_str());
    }

    texZombie = LoadGui(rc, store, "minecraft/textures/entity/zombie/zombie.png");

    D3D10_BUFFER_DESC bd = {};
    bd.ByteWidth = VB_VERTS * UINT(sizeof(MeshVertex));
    bd.Usage = D3D10_USAGE_DYNAMIC;
    bd.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    return SUCCEEDED(rc.device->CreateBuffer(&bd, nullptr, &mVB));
}

void GuiRenderer::Shutdown() {
    GuiTexture* all[] = {&texButton, &texButtonHi, &texButtonDisabled, &texLogo, &texEdition,
                        &texCrosshair, &texHotbar, &texHotbarSel, &texFont, &texDirt,
                        &texWhite, &texBlack, &texAttackBg, &texAttackProgress, &texPanoramaOverlay,
                        &texSlider, &texSliderHi, &texSliderHandle, &texSliderHandleHi,
                        &texInvBg, &texCraftBg, &texMenuBg, &texHeaderSeparator, &texFooterSeparator,
                        &texTab, &texTabHi, &texTabSelected, &texTabSelectedHi, &texSkin, &texZombie,
                        &texHeart[0], &texHeart[1], &texHeart[2],
                        &texFood[0], &texFood[1], &texFood[2]};
    for (GuiTexture* t : all) {
        if (t->srv) { t->srv->Release(); t->srv = nullptr; }
    }
    for (GuiTexture& t : texItemIcon) {
        if (t.srv) { t.srv->Release(); t.srv = nullptr; }
    }
    for (GuiTexture& t : texDestroy) {
        if (t.srv) { t.srv->Release(); t.srv = nullptr; }
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

void GuiRenderer::DrawNineSlice(const GuiTexture* tex, float x, float y, float w, float h,
                                float scale, float shade) {
    if (!tex || !tex->srv || w <= 0 || h <= 0 || scale <= 0) return;
    if (tex->sliceW <= 0 || tex->sliceH <= 0) {
        DrawQuad(tex, x, y, w, h, 0, 0, 1, 1, shade);
        return;
    }
    const float sw = float(tex->sliceW), sh = float(tex->sliceH);
    const float l = float(tex->borderLeft), t = float(tex->borderTop);
    const float r = float(tex->borderRight), b = float(tex->borderBottom);
    const float bx = l + r > 0 ? std::min(scale, w / (l + r)) : scale;
    const float by = t + b > 0 ? std::min(scale, h / (t + b)) : scale;
    const float xs[4] = {x, x + l * bx, x + w - r * bx, x + w};
    const float ys[4] = {y, y + t * by, y + h - b * by, y + h};
    const float us[4] = {0, l / sw, (sw - r) / sw, 1};
    const float vs[4] = {0, t / sh, (sh - b) / sh, 1};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const float dw = xs[col + 1] - xs[col], dh = ys[row + 1] - ys[row];
            const float du = us[col + 1] - us[col], dv = vs[row + 1] - vs[row];
            if (dw <= 0 || dh <= 0 || du <= 0 || dv <= 0) continue;
            // Java's default nine_slice tiles its inner/edge strips. Incomplete
            // repeats crop the UVs rather than stretching a whole sprite.
            const float tw = col == 1 && !tex->stretchInner ? du * sw * scale : dw;
            const float th = row == 1 && !tex->stretchInner ? dv * sh * scale : dh;
            for (float dy = 0; dy < dh; dy += th) {
                for (float dx = 0; dx < dw; dx += tw) {
                    const float cw = std::min(tw, dw - dx), ch = std::min(th, dh - dy);
                    DrawQuad(tex, xs[col] + dx, ys[row] + dy, cw, ch,
                             us[col], vs[row], us[col] + du * cw / tw,
                             vs[row] + dv * ch / th, shade);
                }
            }
        }
    }
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

void GuiRenderer::DrawPlayerPreview(float cx, float feetY, float scale, int mouseX, int mouseY) {
    if (!texSkin.srv || texSkin.w != 64 || texSkin.h != 64 || scale <= 0) return;
    struct Point { float x, y, z; };
    struct Face {
        float p[4][2];
        float u0, v0, u1, v1, depth, shade;
    };
    std::vector<Face> faces;
    faces.reserve(72);
    const auto yaw = [](Point p, float a) -> Point {
        const float c = cosf(a), s = sinf(a);
        return {c * p.x + s * p.z, p.y, -s * p.x + c * p.z};
    };
    const auto pitch = [](Point p, float a) -> Point {
        const float c = cosf(a), s = sinf(a);
        return {p.x, c * p.y - s * p.z, s * p.y + c * p.z};
    };
    const float headYaw = atanf((float(mouseX) - cx) / (scale * 24)) * 0.65f;
    const float headPitch = atanf((float(mouseY) - (feetY - 28 * scale)) / (scale * 24)) * 0.35f;
    const auto cube = [&](float x, float y, float z, float w, float h, float d,
                          float u, float v, float inflate, bool head) {
        const float x0 = x - w / 2 - inflate, x1 = x + w / 2 + inflate;
        const float y0 = y - h / 2 - inflate, y1 = y + h / 2 + inflate;
        const float z0 = z - d / 2 - inflate, z1 = z + d / 2 + inflate;
        // Front, anatomical right, anatomical left, back, top, bottom.
        // UV corners are TL/TR/BR/BL on each face of the Java cube net.
        const Point points[6][4] = {
            {{x0,y1,z1}, {x1,y1,z1}, {x1,y0,z1}, {x0,y0,z1}},
            {{x0,y1,z0}, {x0,y1,z1}, {x0,y0,z1}, {x0,y0,z0}},
            {{x1,y1,z1}, {x1,y1,z0}, {x1,y0,z0}, {x1,y0,z1}},
            {{x1,y1,z0}, {x0,y1,z0}, {x0,y0,z0}, {x1,y0,z0}},
            {{x0,y1,z0}, {x1,y1,z0}, {x1,y1,z1}, {x0,y1,z1}},
            {{x0,y0,z1}, {x1,y0,z1}, {x1,y0,z0}, {x0,y0,z0}}
        };
        const float uv[6][4] = {
            {u+d, v+d, u+d+w, v+d+h},
            {u, v+d, u+d, v+d+h},
            {u+d+w, v+d, u+2*d+w, v+d+h},
            {u+2*d+w, v+d, u+2*d+2*w, v+d+h},
            {u+d, v, u+d+w, v+d},
            {u+d+w, v+d, u+d+2*w, v} // Underside runs back-to-front in the skin net.
        };
        for (int side = 0; side < 6; ++side) {
            Face face = {};
            Point transformed[4];
            for (int i = 0; i < 4; ++i) {
                Point p = points[side][i];
                if (head) {
                    p.y -= 24;
                    p = yaw(pitch(p, headPitch), headYaw);
                    p.y += 24;
                }
                p = pitch(yaw(p, -0.32f), 0.14f);
                transformed[i] = p;
                face.p[i][0] = cx + p.x * scale;
                face.p[i][1] = feetY - p.y * scale;
                face.depth += p.z * 0.25f; // Positive Z points toward the viewer.
            }
            const float area = (face.p[1][0] - face.p[0][0]) * (face.p[2][1] - face.p[0][1]) -
                               (face.p[1][1] - face.p[0][1]) * (face.p[2][0] - face.p[0][0]);
            if (area <= 0.001f) continue; // CPU backface culling, before UI batching.
            const Point a = {transformed[1].x - transformed[0].x,
                             transformed[1].y - transformed[0].y,
                             transformed[1].z - transformed[0].z};
            const Point b = {transformed[3].x - transformed[0].x,
                             transformed[3].y - transformed[0].y,
                             transformed[3].z - transformed[0].z};
            const Point normal = {a.z*b.y - a.y*b.z, a.x*b.z - a.z*b.x, a.y*b.x - a.x*b.y};
            const float length = sqrtf(normal.x*normal.x + normal.y*normal.y + normal.z*normal.z);
            const float light = std::max(0.0f, (-0.3f*normal.x + 0.6f*normal.y + 0.74f*normal.z) / length);
            face.shade = 15 * (0.65f + 0.35f * light);
            face.u0 = uv[side][0] / 64; face.v0 = uv[side][1] / 64;
            face.u1 = uv[side][2] / 64; face.v1 = uv[side][3] / 64;
            faces.push_back(face);
        }
    };
    const float armW = mSkinSlim ? 3.0f : 4.0f;
    const float armY = mSkinSlim ? 17.5f : 18.0f;
    // Six base cuboids plus hat, jacket, sleeves and trouser overlays. Overlay
    // inflation changes geometry only: the underlying skin UVs stay unscaled.
    for (int layer = 0; layer < 2; ++layer) {
        const float grow = layer ? 0.25f : 0.0f;
        cube(0, 28, 0, 8, 8, 8, layer ? 32.0f : 0.0f, 0, layer ? 0.5f : 0.0f, true);
        cube(0, 18, 0, 8, 12, 4, 16, layer ? 32.0f : 16.0f, grow, false);
        cube(-4-armW/2, armY, 0, armW, 12, 4, 40, layer ? 32.0f : 16.0f, grow, false);
        cube(4+armW/2, armY, 0, armW, 12, 4, layer ? 48.0f : 32.0f, 48, grow, false);
        cube(-2, 6, 0, 4, 12, 4, 0, layer ? 32.0f : 16.0f, grow, false);
        cube(2, 6, 0, 4, 12, 4, layer ? 0.0f : 16.0f, 48, grow, false);
    }
    std::stable_sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) {
        return a.depth < b.depth;
    });
    for (const Face& f : faces)
        DrawQuadPts(&texSkin, f.p, f.u0, f.v0, f.u1, f.v1, f.shade);
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
    if (b.tab) tex = b.selected ? (hover ? &ui.texTabSelectedHi : &ui.texTabSelected)
                                : (hover ? &ui.texTabHi : &ui.texTab);
    ui.DrawNineSlice(tex, b.x, b.y, b.w, b.h, 2, 15);
    const float color = b.enabled ? 1.0f : 0.627f;
    ui.DrawTextCentered(b.label, b.x + b.w * 0.5f, b.y + (b.h - 16) * 0.5f,
                        2, 15, true, color, color, color);
    if (b.tab && b.selected) {
        const float underline = ui.TextWidth(b.label, 2);
        ui.DrawQuad(&ui.texWhite, b.x + (b.w - underline) * 0.5f, b.y + b.h - 8,
                    underline, 2, 0, 0, 1, 1, 15);
    }
}

// Vanilla text field: opaque black interior, #A0A0A0 border (white when
// focused), no text shadow, blinking cursor after the last character.
// HeaderAndFooterLayout: 33 GUI-unit header and footer bands.
constexpr float MENU_HEADER_H = 66.0f;
constexpr float MENU_FOOTER_Y = SCREEN_H - MENU_HEADER_H + 13.0f;

static void DrawMenuFrame(GuiRenderer& ui, const char* title) {
    DrawTiled(ui, ui.texMenuBg, 0, 0, SCREEN_W, SCREEN_H);
    DrawTiled(ui, ui.texHeaderSeparator, 0, MENU_HEADER_H - 4, SCREEN_W, 4);
    DrawTiled(ui, ui.texFooterSeparator, 0, SCREEN_H - MENU_HEADER_H, SCREEN_W, 4);
    ui.DrawTextCentered(title, SCREEN_W * 0.5f, (MENU_HEADER_H - 16) * 0.5f, 2, 15, true);
}

static void DrawTextField(GuiRenderer& ui, const std::string& text, float x, float y,
                          float w, float h, bool focused, float timeSec) {
    const float bc = focused ? 1.0f : 0.627f;
    ui.DrawQuad(&ui.texWhite, x, y, w, h, 0, 0, 1, 1, 15, bc, bc, bc);
    ui.DrawQuad(&ui.texWhite, x + 2, y + 2, w - 4, h - 4, 0, 0, 1, 1, 15, 0, 0, 0);
    size_t start = 0;
    while (start < text.size() && ui.TextWidth(text.substr(start), 2) > w - 24) ++start;
    const std::string shown = text.substr(start);
    const float ty = y + (h - 16) * 0.5f;
    ui.DrawText(shown, x + 8, ty, 2, 15, false, 0.878f, 0.878f, 0.878f);
    if (focused && fmodf(timeSec * 2.0f, 1.0f) < 0.5f) {
        const float cxPos = x + 8 + ui.TextWidth(shown, 2);
        ui.DrawQuad(&ui.texWhite, cxPos, ty, 2, 16, 0, 0, 1, 1, 15, 0.816f, 0.816f, 0.816f);
    }
}

void MenuSystem::Draw(GuiRenderer& ui, Screen screen, Settings& settings, WorldCreateInfo& create,
                      const std::string& typed, int mouseX, int mouseY, bool leftDown,
                      const std::string& splash, float timeSec, const PlayerProfile& profile) {
    (void)typed; // The caller edits create/profile state, including backspace.
    mButtons.clear();
    mSliders.clear();
    mWorldListVisible = screen == Screen::SelectWorld;
    const float cx = SCREEN_W * 0.5f;
    char label[96];

    if (screen == Screen::Title) {
        if (ui.texPanoramaOverlay.srv)
            ui.DrawQuad(&ui.texPanoramaOverlay, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 1, 1, 15);
        // Vanilla LogoRenderer: 256x44 GUI units from the top 44/64 of the logo,
        // edition strip 128x14 from the top 14/16, both at GUI scale 2.
        ui.DrawQuad(&ui.texLogo, cx - 256, 60, 512, 88, 0, 0, 1, 44.0f / 64.0f, 15);
        if (ui.texEdition.srv)
            ui.DrawQuad(&ui.texEdition, cx - 128, 134, 256, 28, 0, 0, 1, 14.0f / 16.0f, 15);
        const float pulse = 1.8f - fabsf(sinf(fmodf(timeSec, 1.0f) * 6.2831853f)) * 0.1f;
        const float fit = pulse * 100.0f / (ui.TextWidth(splash, 1) + 32.0f);
        ui.DrawTextRotated(splash, cx + 246, 138, 2.0f * fit, -0.349f, 15, true, 1, 1, 0);
        mButtons.push_back({cx - 200, 288, 400, 40, "Singleplayer", ACT_SINGLEPLAYER, true});
        mButtons.push_back({cx - 200, 336, 400, 40, "Multiplayer", ACT_NONE, false});
        mButtons.push_back({cx - 200, 384, 400, 40, "Local Profile...", ACT_PROFILE, true});
        mButtons.push_back({cx - 200, 456, 196, 40, "Options...", ACT_OPTIONS, true});
        mButtons.push_back({cx + 4, 456, 196, 40, "Quit Game", ACT_QUIT, true});
        ui.DrawText("Minecraft Potato Edition (fan project)", 4, SCREEN_H - 20, 2, 15, true);
        const char* tag = "DX10 / Feature Level 10_0";
        ui.DrawText(tag, SCREEN_W - ui.TextWidth(tag, 2) - 4, SCREEN_H - 20, 2, 15, true);
    } else if (screen == Screen::SelectWorld) {
        DrawMenuFrame(ui, "Select World");
        if (worlds.empty())
            ui.DrawTextCentered("No saved worlds. Create your first world below.", cx, 140, 2, 15, true);
        for (int row = 0; row < 5; ++row) {
            const int index = worldPage * 5 + row;
            if (index >= int(worlds.size())) break;
            const float y = 84.0f + row * 104.0f;
            const bool selected = index == selectedWorld;
            ui.DrawQuad(&ui.texWhite, cx - 400, y, 800, 92, 0, 0, 1, 1, 15,
                        selected ? 0.627f : 0.16f, selected ? 0.627f : 0.16f,
                        selected ? 0.627f : 0.16f);
            ui.DrawQuad(&ui.texWhite, cx - 398, y + 2, 796, 88, 0, 0, 1, 1, 15, 0, 0, 0);
            std::string name = worlds[index].name;
            while (!name.empty() && ui.TextWidth(name, 2) > 744) name.pop_back();
            ui.DrawText(name, cx - 378, y + 16, 2, 15, true);
            ui.DrawText(worlds[index].details, cx - 378, y + 46, 2, 15, true,
                        0.627f, 0.627f, 0.627f);
        }
        const int pages = std::max(1, (int(worlds.size()) + 4) / 5);
        snprintf(label, sizeof(label), "Page %d / %d", worldPage + 1, pages);
        ui.DrawTextCentered(label, cx, 628, 2, 15, true);
        mButtons.push_back({cx - 400, 612, 120, 40, "<", ACT_WORLD_PREV, worldPage > 0});
        mButtons.push_back({cx + 280, 612, 120, 40, ">", ACT_WORLD_NEXT, worldPage + 1 < pages});
        mButtons.push_back({cx - 400, MENU_FOOTER_Y, 260, 40, "Play Selected World",
                            ACT_LOAD_WORLD, selectedWorld >= 0 && selectedWorld < int(worlds.size())});
        mButtons.push_back({cx - 130, MENU_FOOTER_Y, 260, 40, "Create New World", ACT_NEW_WORLD, true});
        mButtons.push_back({cx + 140, MENU_FOOTER_Y, 260, 40, "Cancel", ACT_CANCEL_CREATE, true});
    } else if (screen == Screen::CreateWorld) {
        // This screen supplies its modern background over the caller's scene.
        DrawTiled(ui, ui.texMenuBg, 0, 0, SCREEN_W, SCREEN_H);
        DrawTiled(ui, ui.texHeaderSeparator, 0, 96, SCREEN_W, 4);
        DrawTiled(ui, ui.texFooterSeparator, 0, 584, SCREEN_W, 4);
        ui.DrawTextCentered("Create New World", cx, 20, 2, 15, true);
        const float fx = cx - 200, fw = 400, fh = 40;
        const char* tabs[] = {"Game", "World", "More"};
        const int tabActions[] = {ACT_CREATE_TAB_GAME, ACT_CREATE_TAB_WORLD, ACT_CREATE_TAB_MORE};
        for (int i = 0; i < 3; ++i)
            mButtons.push_back({cx - 390 + i * 260, 48, 260, 48, tabs[i], tabActions[i],
                                true, true, create.tab == i});
        static const char* MODES[] = {"Survival", "Creative"};
        static const char* DIFF[] = {"Peaceful", "Easy", "Normal", "Hard"};
        if (create.tab == 0) {
            ui.DrawText("World Name", fx, 104, 2, 15, true, 0.627f, 0.627f, 0.627f);
            DrawTextField(ui, create.name, fx, 126, fw, fh, create.focus == 1, timeSec);
            snprintf(label, sizeof(label), "Game Mode: %s", MODES[create.gameMode & 1]);
            mButtons.push_back({fx, 186, fw, fh, label, ACT_CYCLE_GAMEMODE, true});
            const char* desc = (create.gameMode & 1)
                ? "Unlimited resources, free flying and destroy blocks instantly"
                : "Gather resources, craft, manage health and hunger";
            ui.DrawTextCentered(desc, cx, 232, 2, 15, true, 0.627f, 0.627f, 0.627f);
            snprintf(label, sizeof(label), "Difficulty: %s", DIFF[create.difficulty & 3]);
            mButtons.push_back({fx, 262, fw, fh, label, ACT_CYCLE_DIFF, true});
            static const char* DDESC[] = {
                "Health regenerates. Hunger never depletes.",
                "Hunger depletes. Starvation stops at 5 hearts.",
                "Hunger depletes. Starvation stops at half a heart.",
                "Hunger depletes. Starvation can kill."};
            ui.DrawTextCentered(DDESC[create.difficulty & 3], cx, 308, 2, 15, true,
                                0.627f, 0.627f, 0.627f);
            mButtons.push_back({fx, 338, fw, fh, "Allow Cheats: Unavailable", ACT_NONE, false});
        } else if (create.tab == 1) {
            ui.DrawText("Seed for the world generator", fx, 104, 2, 15, true, 0.627f, 0.627f, 0.627f);
            DrawTextField(ui, create.seed, fx, 126, fw, fh, create.focus == 2, timeSec);
            ui.DrawText("Leave blank for a random seed", fx, 172, 2, 15, true, 0.627f, 0.627f, 0.627f);
            const char* type = create.generator == WorldGenerator::Flat ? "Superflat" :
                (create.generator == WorldGenerator::Climate ? "Default (Climate)" : "Legacy");
            snprintf(label, sizeof(label), "World Type: %s", type);
            mButtons.push_back({fx, 208, fw, fh, label, ACT_CYCLE_WORLD_TYPE, true});
            ui.DrawTextCentered("Potato terrain; not Java seed-compatible", cx, 266, 2, 15, true,
                                0.627f, 0.627f, 0.627f);
        } else {
            mButtons.push_back({fx, 126, fw, fh, "Game Rules: Unavailable", ACT_NONE, false});
            mButtons.push_back({fx, 186, fw, fh, "Data Packs: Unavailable", ACT_NONE, false});
            mButtons.push_back({fx, 246, fw, fh, "Experiments: Unavailable", ACT_NONE, false});
            ui.DrawTextCentered("These features are not implemented.", cx, 308, 2, 15, true,
                                0.627f, 0.627f, 0.627f);
        }
        mButtons.push_back({fx, 600, fw, fh, "Create New World", ACT_CREATE_WORLD, !create.name.empty()});
        mButtons.push_back({fx, 648, fw, fh, "Cancel", ACT_CANCEL_CREATE, true});
        if ((create.tab == 0 || create.tab == 1) && leftDown &&
            mouseX >= fx && mouseX < fx + fw && mouseY >= 126 && mouseY < 126 + fh)
            create.focus = create.tab == 0 ? 1 : 2;
    } else if (screen == Screen::Options) {
        DrawMenuFrame(ui, "Options");
        mButtons.push_back({cx - 310, 80, 300, 40, "Video Settings...", ACT_VIDEO_OPTIONS, true});
        mButtons.push_back({cx + 10, 80, 300, 40, "Sound Settings...", ACT_SOUND_OPTIONS, true});
        mButtons.push_back({cx - 310, 130, 300, 40, "Controls...", ACT_CONTROLS_OPTIONS, true});
        mButtons.push_back({cx + 10, 130, 300, 40, "Local Profile...", ACT_PROFILE, true});
        static const char* DIFF[] = {"Peaceful", "Easy", "Normal", "Hard"};
        snprintf(label, sizeof(label), "Difficulty: %s", DIFF[settings.difficulty & 3]);
        mButtons.push_back({cx - 310, 180, 300, 40, label, ACT_CYCLE_DIFF, true});
        mButtons.push_back({cx + 10, 180, 300, 40, "Resource Packs...", ACT_NONE, false});
        mButtons.push_back({cx - 200, MENU_FOOTER_Y, 400, 40, "Done", ACT_OPTIONS_DONE, true});
    } else if (screen == Screen::VideoOptions) {
        DrawMenuFrame(ui, "Video Settings");
        mSliders.push_back({SL_FOV, cx - 310, 80, 300, 40});
        mSliders.push_back({SL_RD, cx + 10, 80, 300, 40});
        mSliders.push_back({SL_BRIGHT, cx - 310, 130, 300, 40});
        snprintf(label, sizeof(label), "View Bobbing: %s", settings.viewBobbing ? "ON" : "OFF");
        mButtons.push_back({cx + 10, 130, 300, 40, label, ACT_TOGGLE_BOB, true});
        snprintf(label, sizeof(label), "VSync: %s", settings.vsync ? "ON" : "OFF");
        mButtons.push_back({cx - 310, 180, 300, 40, label, ACT_TOGGLE_VSYNC, true});
        snprintf(label, sizeof(label), "Fullscreen: %s", settings.fullscreen ? "ON" : "OFF");
        mButtons.push_back({cx + 10, 180, 300, 40, label, ACT_TOGGLE_FS, true});
        mButtons.push_back({cx - 200, MENU_FOOTER_Y, 400, 40, "Done", ACT_OPTIONS_DONE, true});
    } else if (screen == Screen::SoundOptions) {
        DrawMenuFrame(ui, "Sound Settings");
        mSliders.push_back({SL_SOUND, cx - 200, 80, 400, 40});
        ui.DrawTextCentered("Master volume controls implemented sound effects.", cx, 144,
                            2, 15, true, 0.627f, 0.627f, 0.627f);
        ui.DrawTextCentered("Music playback is not implemented.", cx, 172,
                            2, 15, true, 0.627f, 0.627f, 0.627f);
        mButtons.push_back({cx - 200, MENU_FOOTER_Y, 400, 40, "Done", ACT_OPTIONS_DONE, true});
    } else if (screen == Screen::ControlsOptions) {
        DrawMenuFrame(ui, "Controls");
        mSliders.push_back({SL_SENS, cx - 200, 80, 400, 40});
        ui.DrawTextCentered("Mouse sensitivity", cx, 144, 2, 15, true, 0.627f, 0.627f, 0.627f);
        mButtons.push_back({cx - 200, MENU_FOOTER_Y, 400, 40, "Done", ACT_OPTIONS_DONE, true});
    } else if (screen == Screen::Profile) {
        DrawMenuFrame(ui, "Local Profile");
        ui.DrawTextCentered("Local name and bundled appearance only.", cx, 74, 2, 15, true);
        ui.DrawTextCentered("Not a Microsoft account or multiplayer sign-in.", cx, 96,
                            2, 15, true, 0.627f, 0.627f, 0.627f);
        const float fx = cx - 310;
        ui.DrawText("Player Name", fx, 122, 2, 15, true);
        DrawTextField(ui, profile.name, fx, 144, 360, 40, true, timeSec);
        ui.DrawText("3-16 letters, digits or _", fx, 192, 2, 15, true, 0.627f, 0.627f, 0.627f);
        mButtons.push_back({fx, 224, 48, 40, "<", ACT_SKIN_PREV, true});
        mButtons.push_back({fx + 312, 224, 48, 40, ">", ACT_SKIN_NEXT, true});
        snprintf(label, sizeof(label), "%s (%d/9)", SkinName(profile.skin),
                 profile.skin >= 0 && profile.skin < SKIN_COUNT ? profile.skin + 1 : 0);
        ui.DrawTextCentered(label, fx + 180, 236, 2, 15, true);
        snprintf(label, sizeof(label), "Model: %s", profile.slim ? "Slim" : "Wide");
        mButtons.push_back({fx, 280, 360, 40, label, ACT_SKIN_MODEL, true});
        const bool validName = IsValidPlayerName(profile.name);
        if (!validName)
            ui.DrawText("Enter a valid player name.", fx, 344, 2, 15, true, 1, 0.45f, 0.45f);
        if (ui.texSkin.srv) ui.DrawPlayerPreview(cx + 222, 468, 8, mouseX, mouseY);
        else ui.DrawTextCentered("Skin unavailable", cx + 222, 320, 2, 15, true, 1, 0.65f, 0.4f);
        ui.DrawTextCentered("Preview", cx + 222, 488, 2, 15, true, 0.627f, 0.627f, 0.627f);
        mButtons.push_back({cx - 200, MENU_FOOTER_Y, 196, 40, "Done", ACT_PROFILE_DONE, validName});
        mButtons.push_back({cx + 4, MENU_FOOTER_Y, 196, 40, "Cancel", ACT_PROFILE_CANCEL, true});
    } else if (screen == Screen::Pause) {
        ui.DrawQuad(&ui.texBlack, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 1, 1, 15);
        ui.DrawTextCentered("Game Menu", cx, 60, 2, 15, true);
        mButtons.push_back({cx - 200, 180, 400, 40, "Back to Game", ACT_RESUME, true});
        mButtons.push_back({cx - 200, 228, 196, 40, "Options...", ACT_OPTIONS, true});
        mButtons.push_back({cx + 4, 228, 196, 40, "Advancements", ACT_NONE, false});
        mButtons.push_back({cx - 200, 276, 400, 40, "Save and Quit to Title", ACT_QUIT_TO_TITLE, true});
    } else if (screen == Screen::Death) {
        ui.DrawQuad(&ui.texBlack, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 1, 1, 15, 1.0f, 0.25f, 0.2f);
        ui.DrawTextCentered("You died!", cx, 120, 3, 15, true);
        ui.DrawTextCentered("Score: 0", cx, 200, 2, 15, true, 1, 1, 0);
        mButtons.push_back({cx - 200, 280, 400, 40, "Respawn", ACT_RESPAWN, true});
        mButtons.push_back({cx - 200, 328, 400, 40, "Title Screen", ACT_QUIT_TO_TITLE, true});
    }

    for (const Slider& s : mSliders) DrawSlider(ui, s, settings, mouseX, mouseY, leftDown);
    for (const Button& b : mButtons) DrawButton(ui, b, mouseX, mouseY);
}

static float SliderT(const Settings& s, int id) {
    switch (id) {
        case SL_FOV: return (s.fov - 30.0f) / 80.0f;
        case SL_RD: return (s.renderDistance - 2) / 14.0f;
        case SL_SENS: return s.sensitivity / 0.005f;
        case SL_SOUND: return s.sound ? s.soundVolume : 0.0f;
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
        case SL_SOUND: s.soundVolume = t; s.sound = t > 0.0f; break;
        case SL_MUSIC: s.musicVolume = t; break;
        default: s.brightness = t; break;
    }
}

static void SliderLabel(const Settings& s, int id, char* out, size_t n) {
    switch (id) {
        case SL_FOV: snprintf(out, n, "FOV: %d", int(s.fov + 0.5f)); break;
        case SL_RD: snprintf(out, n, "Render Distance: %d", s.renderDistance); break;
        case SL_SENS: snprintf(out, n, "Sensitivity: %d%%", int(s.sensitivity / 0.0025f * 100 + 0.5f)); break;
        case SL_SOUND: snprintf(out, n, "Master Volume: %d%%", s.sound ? int(s.soundVolume * 100 + 0.5f) : 0); break;
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
    ui.DrawNineSlice(hover ? &ui.texSliderHi : &ui.texSlider, s.x, s.y, s.w, s.h, 2, 15);
    const float hx = s.x + std::max(0.0f, std::min(1.0f, SliderT(settings, s.id))) * (s.w - 16.0f);
    ui.DrawNineSlice(hover ? &ui.texSliderHandleHi : &ui.texSliderHandle, hx, s.y, 16, s.h, 2, 15);
    char label[96];
    SliderLabel(settings, s.id, label, sizeof(label));
    ui.DrawTextCentered(label, s.x + s.w * 0.5f, s.y + (s.h - 16) * 0.5f, 2, 15, true);
}

int MenuSystem::HitTest(int mouseX, int mouseY) const {
    if (mWorldListVisible && mouseX >= SCREEN_W * 0.5f - 400 && mouseX < SCREEN_W * 0.5f + 400) {
        const int row = (mouseY - 84) / 104;
        const int index = worldPage * 5 + row;
        if (mouseY >= 84 && row < 5 && mouseY < 84 + row * 104 + 92 && index < int(worlds.size()))
            return ACT_WORLD_ROW + index;
    }
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

static void DrawItemIcon(GuiRenderer& ui, int id, float x, float y) {
    if (id <= BLOCK_AIR || !IsValidItem(id)) return;
    const GuiTexture& icon = ui.texItemIcon[id];
    if (!icon.srv || icon.w <= 0 || icon.h <= 0) return;
    const float frame = float(std::min(icon.w, icon.h));
    ui.DrawQuad(&icon, x, y, 32, 32, 0, 0, frame / icon.w, frame / icon.h, 15);
}

static void DrawStackCount(GuiRenderer& ui, float x, float y, const ItemSlot& s) {
    if (s.Empty()) return;
    if (const ToolDef* tool = FindTool(s.id); tool && s.damage > 0) {
        const float remaining = 1.0f - float(s.damage) / tool->durability;
        ui.DrawQuad(&ui.texWhite, x + 2, y + 26, 26, 4, 0, 0, 1, 1, 15, 0, 0, 0);
        ui.DrawQuad(&ui.texWhite, x + 2, y + 26, 26 * remaining, 2, 0, 0, 1, 1, 15,
                    1 - remaining, remaining, 0);
    }
    if (s.count <= 1) return;
    char n[12];
    snprintf(n, sizeof(n), "%d", s.count);
    ui.DrawText(n, x + 32 - ui.TextWidth(n, 2), y + 18, 2, 15, true);
}

void DrawHotbar(GuiRenderer& ui, int selectedSlot, const uint8_t* hotbar) {
    const float cx = SCREEN_W * 0.5f;
    const float hotbarX = cx - 182, hotbarY = SCREEN_H - 44.0f;
    ui.DrawSprite(&ui.texHotbar, hotbarX, hotbarY, 2, 15);
    ui.DrawSprite(&ui.texHotbarSel, hotbarX + selectedSlot * 40 - 2, hotbarY - 2, 2, 15);
    for (int i = 0; i < 9; ++i) {
        DrawItemIcon(ui, hotbar[i], hotbarX + i * 40 + 4, hotbarY + 6);
    }
}

void DrawHUD(GuiRenderer& ui, float attackStrength, int selectedSlot,
             const uint8_t* hotbar, int health, int hunger, const std::string& actionBar,
             bool creative, const ItemSlot* stacks) {
    const float cx = SCREEN_W * 0.5f, cy = SCREEN_H * 0.5f;
    ui.DrawSprite(&ui.texCrosshair, cx - 15, cy - 15, 2, 15);

    // 1.9+ attack recharge indicator under the crosshair (hidden when full).
    // Creative has no attack cooldown, so the indicator never shows there.
    if (!creative && attackStrength < 1.0f) {
        ui.DrawSprite(&ui.texAttackBg, cx - 16, cy + 22, 2, 15);
        ui.DrawQuad(&ui.texAttackProgress, cx - 16, cy + 22, 32 * attackStrength, 8,
                    0, 0, attackStrength, 1, 15);
    }

    if (!creative) {
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
    }

    DrawHotbar(ui, selectedSlot, hotbar);
    if (stacks) {
        const float hotbarX = cx - 182, hotbarY = SCREEN_H - 44.0f;
        for (int i = 0; i < 9; ++i)
            DrawStackCount(ui, hotbarX + i * 40 + 4, hotbarY + 6, stacks[i]);
    }
    if (!actionBar.empty()) {
        ui.DrawTextCentered(actionBar, cx, SCREEN_H - 112.0f, 2, 15, true);
    }
}

// ---------------------------------------------------------------------------
// Creative inventory picker
// ---------------------------------------------------------------------------

namespace {
constexpr int INV_COLS = 9;
constexpr int INV_CELL = 40;
constexpr int INV_ROWS = (BLOCK_COUNT - 1 + 34 + INV_COLS - 1) / INV_COLS;
constexpr float INV_PAD = 12;
constexpr float INV_W = INV_COLS * INV_CELL + INV_PAD * 2;
constexpr float INV_TITLE_H = 40;
constexpr float INV_HINT_H = 36;
constexpr float INV_H = INV_TITLE_H + INV_ROWS * INV_CELL + INV_HINT_H;
constexpr float INV_X = (SCREEN_W - INV_W) * 0.5f;
constexpr float INV_Y = (SCREEN_H - INV_H) * 0.5f - 20;
constexpr float INV_GX = INV_X + INV_PAD;
constexpr float INV_GY = INV_Y + INV_TITLE_H;

const std::vector<uint8_t>& CreativeItems() {
    static const std::vector<uint8_t> items = [] {
        std::vector<uint8_t> result;
        for (int i = 1; i < 256; ++i) if (IsValidItem(i)) result.push_back(uint8_t(i));
        return result;
    }();
    return items;
}

void CellRect(int i, float& x, float& y) {
    x = INV_GX + (i % INV_COLS) * INV_CELL;
    y = INV_GY + (i / INV_COLS) * INV_CELL;
}
} // namespace

int InventoryHitTest(int mouseX, int mouseY) {
    for (int i = 0; i < int(CreativeItems().size()); ++i) {
        const uint8_t b = CreativeItems()[i];
        float x, y;
        CellRect(i, x, y);
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
    for (int i = 0; i < int(CreativeItems().size()); ++i) {
        const uint8_t b = CreativeItems()[i];
        float x, y;
        CellRect(i, x, y);
        ui.DrawQuad(&ui.texWhite, x + 2, y + 2, INV_CELL - 4, INV_CELL - 4, 0, 0, 1, 1, 15, 0.55f, 0.55f, 0.55f);
        ui.DrawQuad(&ui.texWhite, x + 2, y + 2, INV_CELL - 4, 1, 0, 0, 1, 1, 15, 0.35f, 0.35f, 0.35f);
        ui.DrawQuad(&ui.texWhite, x + 2, y + 2, 1, INV_CELL - 4, 0, 0, 1, 1, 15, 0.35f, 0.35f, 0.35f);
        if (b == hovered)
            ui.DrawQuad(&ui.texHotbarSel, x, y, INV_CELL, INV_CELL, 0, 0, 1, 1, 15);
        DrawItemIcon(ui, b, x + 4, y + 4);
    }

    ui.DrawText("Click an item to select it", INV_X + INV_PAD, INV_Y + INV_H - 26, 2, 15, false, 0.3f, 0.3f, 0.3f);

    if (hovered > 0) {
        std::string name = ItemName(uint8_t(hovered));
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

// ---------------------------------------------------------------------------
// Survival inventory + crafting
// ---------------------------------------------------------------------------

namespace {
constexpr float INV_S = 2.0f; // GUI scale
constexpr float SURV_INV_W = 176 * INV_S, SURV_INV_H = 166 * INV_S;

// Slot item positions copied from InventoryMenu / CraftingMenu (176x166 GUI).
void SlotPos(int index, bool table, float& x, float& y) {
    if (index >= 45) {
        x = table ? 124 : 154; y = table ? 35 : 28;
    } else if (index >= 36) {
        const int g = index - 36;
        x = (table ? 30 : 98) + (g % 3) * 18;
        y = (table ? 17 : 18) + (g / 3) * 18;
    } else if (index >= 27) {
        x = 8 + (index - 27) * 18; y = 142;
    } else {
        x = 8 + (index % 9) * 18; y = 84 + (index / 9) * 18;
    }
    x *= INV_S; y *= INV_S;
}

void DrawStack(GuiRenderer& ui, float x, float y, const ItemSlot& s) {
    if (s.Empty()) return;
    DrawItemIcon(ui, s.id, x, y);
    DrawStackCount(ui, x, y, s);
}
} // namespace

int SurvivalInvHitTest(int mouseX, int mouseY, bool table) {
    const float px = (SCREEN_W - SURV_INV_W) * 0.5f;
    const float py = (SCREEN_H - SURV_INV_H) * 0.5f;
    for (int i = 0; i < 46; ++i) {
        if (i >= 36 && i < 45 && !table) {
            const int g = i - 36;
            if (g % 3 == 2 || g / 3 == 2) continue; // 2x2 hides third row/col
        }
        float x, y; SlotPos(i, table, x, y);
        x += px - INV_S; y += py - INV_S; // 18px cell around the 16px item
        if (mouseX >= x && mouseX < x + 18 * INV_S &&
            mouseY >= y && mouseY < y + 18 * INV_S) return i;
    }
    return -1;
}

void DrawSurvivalInventory(GuiRenderer& ui, const InventoryState& model, bool table,
                           int mouseX, int mouseY, const std::string& message) {
    ui.DrawQuad(&ui.texBlack, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 1, 1, 15);
    const float px = (SCREEN_W - SURV_INV_W) * 0.5f;
    const float py = (SCREEN_H - SURV_INV_H) * 0.5f;
    const GuiTexture& bg = table ? ui.texCraftBg : ui.texInvBg;
    ui.DrawQuad(&bg, px, py, SURV_INV_W, SURV_INV_H, 0, 0, 176.0f / bg.w, 166.0f / bg.h, 15);
    if (!table) ui.DrawPlayerPreview(px + 51 * INV_S, py + 75 * INV_S, 3.4f, mouseX, mouseY);
    ui.DrawText("Crafting", px + (table ? 28 : 97) * INV_S, py + (table ? 6 : 8) * INV_S,
                INV_S, 15, false, 0.25f, 0.25f, 0.25f);
    if (table)
        ui.DrawText("Inventory", px + 8 * INV_S, py + 72 * INV_S,
                    INV_S, 15, false, 0.25f, 0.25f, 0.25f);
    if (!message.empty())
        ui.DrawTextCentered(message, SCREEN_W * 0.5f, py + SURV_INV_H + 12,
                            INV_S, 15, true, 1.0f, 0.8f, 0.4f);

    const ItemSlot result = model.Result(table);
    const int hovered = SurvivalInvHitTest(mouseX, mouseY, table);
    for (int i = 0; i < 46; ++i) {
        if (i >= 36 && i < 45 && !table) {
            const int g = i - 36;
            if (g % 3 == 2 || g / 3 == 2) continue;
        }
        float x, y; SlotPos(i, table, x, y);
        x += px; y += py;
        if (i == hovered)
            ui.DrawQuad(&ui.texWhite, x, y, 16 * INV_S, 16 * INV_S, 0, 0, 1, 1, 15,
                        0.85f, 0.85f, 0.85f);
        const ItemSlot& s = i == 45 ? result : (i >= 36 ? model.craft[i - 36] : model.slots[i]);
        DrawStack(ui, x, y, s);
    }
    if (model.cursor.Empty() && hovered >= 0) {
        const ItemSlot& s = hovered == 45 ? result :
                            (hovered >= 36 ? model.craft[hovered - 36] : model.slots[hovered]);
        if (!s.Empty() && IsValidItem(s.id)) {
            std::string name = ItemName(s.id);
            for (char& c : name) if (c == '_') c = ' ';
            const float tw = ui.TextWidth(name, INV_S);
            float tx = mouseX + 16, ty = mouseY - 12;
            if (tx + tw + 12 > SCREEN_W) tx = SCREEN_W - tw - 12;
            if (ty < 4) ty = 4;
            ui.DrawQuad(&ui.texBlack, tx - 6, ty - 6, tw + 12, 28, 0, 0, 1, 1, 15, 0.1f, 0.05f, 0.2f);
            ui.DrawText(name, tx, ty, INV_S, 15, true);
        }
    }
    DrawStack(ui, float(mouseX) - 8 * INV_S, float(mouseY) - 8 * INV_S, model.cursor);
}

} // namespace potato