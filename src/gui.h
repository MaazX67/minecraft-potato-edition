#pragma once
#include "assets.h"
#include "renderer.h"
#include "world.h"
#include <string>
#include <vector>

namespace potato {

struct GuiTexture {
    ID3D10ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
};

enum class Screen { Title, Options, Pause, Game, Inventory, Death };

struct Settings {
    int renderDistance = 8;
    float fov = 70.0f;
    float sensitivity = 0.0025f;
    bool sound = true;
};

// Orthographic textured-quad renderer. One dynamic vertex buffer, batches
// grouped by texture, drawn with PSMainUI (alpha blend, depth off). Vertex
// tint (cr,cg,cb) carries GUI/text colors; shade is the 0..15 light level.
class GuiRenderer {
public:
    bool Init(RenderContext& rc, AssetStore& store);
    void Shutdown();
    void BeginFrame();
    void Flush(RenderContext& rc);

    void DrawQuad(const GuiTexture* tex, float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1, float shade,
                  float cr = 1.0f, float cg = 1.0f, float cb = 1.0f);
    void DrawSprite(const GuiTexture* tex, float x, float y, float scale, float shade);
    void DrawText(const std::string& text, float x, float y, float scale, float shade, bool shadow,
                  float cr = 1.0f, float cg = 1.0f, float cb = 1.0f);
    void DrawTextCentered(const std::string& text, float cx, float y, float scale, float shade, bool shadow,
                          float cr = 1.0f, float cg = 1.0f, float cb = 1.0f);
    void DrawQuadPts(const GuiTexture* tex, const float p[4][2],
                     float u0, float v0, float u1, float v1, float shade,
                     float cr = 1.0f, float cg = 1.0f, float cb = 1.0f);
    void DrawTextRotated(const std::string& text, float cx, float cy, float scale,
                         float angle, float shade, bool shadow,
                         float cr = 1.0f, float cg = 1.0f, float cb = 1.0f);
    float TextWidth(const std::string& text, float scale) const;

    GuiTexture texButton, texButtonHi, texButtonDisabled, texLogo, texCrosshair;
    GuiTexture texHotbar, texHotbarSel, texFont, texDirt, texWhite, texBlack;
    GuiTexture texHeart[3];   // container, full, half
    GuiTexture texFood[3];    // empty, full, half
    GuiTexture texAttackBg, texAttackProgress;
    GuiTexture texPanoramaOverlay;
    GuiTexture texBlockIcon[BLOCK_COUNT];

private:
    struct Batch {
        const GuiTexture* tex = nullptr;
        std::vector<MeshVertex> verts;
    };
    GuiTexture LoadGui(RenderContext& rc, AssetStore& store, const char* name);
    void DrawTextRaw(const std::string& text, float x, float y, float scale, float shade,
                     float cr, float cg, float cb);
    std::vector<Batch> mBatches;
    ID3D10Buffer* mVB = nullptr;
    static constexpr UINT VB_VERTS = 65536;
};

enum MenuAction {
    ACT_NONE = 0, ACT_SINGLEPLAYER, ACT_OPTIONS, ACT_QUIT, ACT_RESUME,
    ACT_QUIT_TO_TITLE, ACT_OPTIONS_DONE, ACT_RESPAWN,
    ACT_CYCLE_RD, ACT_CYCLE_FOV, ACT_CYCLE_SENS, ACT_TOGGLE_SOUND
};

// Vanilla-style screens built from real GUI sprites. The 3D background
// (panorama skybox or frozen world) is rendered by the caller first.
class MenuSystem {
public:
    void Draw(GuiRenderer& ui, Screen screen, const Settings& settings,
              int mouseX, int mouseY, const std::string& splash, float timeSec);
    int HitTest(int mouseX, int mouseY) const;

private:
    struct Button {
        float x, y, w, h;
        std::string label;
        int action;
        bool enabled;
    };
    void DrawButton(GuiRenderer& ui, const Button& b, int mouseX, int mouseY);
    std::vector<Button> mButtons;
};

// Default creative hotbar layout; the live hotbar is a mutable copy in main.
extern const uint8_t HOTBAR_BLOCKS[9];

void DrawHotbar(GuiRenderer& ui, int selectedSlot, const uint8_t* hotbar);
void DrawHUD(GuiRenderer& ui, float attackStrength, int selectedSlot,
             const uint8_t* hotbar, int health, int hunger,
             const std::string& actionBar);

// Creative block picker (E). Returns the block id under the cursor, or -1.
void DrawInventory(GuiRenderer& ui, int mouseX, int mouseY, int selectedSlot,
                   const uint8_t* hotbar);
int InventoryHitTest(int mouseX, int mouseY);

} // namespace potato