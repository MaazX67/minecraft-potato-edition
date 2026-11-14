#pragma once
#include "assets.h"
#include "engine.h"
#include "renderer.h"
#include "world.h"
#include <string>
#include <vector>

namespace potato {

struct GuiTexture {
    ID3D10ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
    // Logical sprite dimensions/borders from its gui.scaling mcmeta.
    int sliceW = 0, sliceH = 0;
    int borderLeft = 0, borderTop = 0, borderRight = 0, borderBottom = 0;
    bool stretchInner = false;
};

enum class Screen {
    Title, CreateWorld, Options, Pause, Game, Inventory, Death,
    VideoOptions, SoundOptions, ControlsOptions, Profile, SelectWorld, Furnace, Chest,
    ConfirmDelete, EditWorld, GameRules
};

struct PlayerProfile {
    std::string name = "Player";
    int skin = 0;
    bool slim = false;
};

bool IsValidPlayerName(const std::string& name);
// 0 Steve, 1 Alex, 2 Ari, 3 Efe, 4 Kai, 5 Makena, 6 Noor, 7 Sunny, 8 Zuri.
const char* SkinName(int skin);

struct Settings {
    int renderDistance = 8;
    float fov = 70.0f;
    float sensitivity = 0.0025f;
    bool sound = true;
    float soundVolume = 1.0f;
    float musicVolume = 0.5f;
    float brightness = 0.5f;
    bool viewBobbing = true;
    bool vsync = false;
    bool fullscreen = false;
    int difficulty = 2; // 0 peaceful, 1 easy, 2 normal, 3 hard
    // Video
    bool fancyGraphics = true;
    bool smoothLighting = true;
    int maxFramerate = 120; // 0 = unlimited
    int guiScale = 0;       // 0 = auto, 1..4
    int clouds = 2;         // 0 off, 1 fast, 2 fancy
    int particles = 2;      // 0 minimal, 1 decreased, 2 all
    bool entityShadows = true;
    // Controls
    bool invertMouse = false;
    float scrollSensitivity = 1.0f;
    // Sound categories: master, music, jukebox, weather, blocks, hostile,
    // friendly, players, ambient.
    float categoryVolume[9] = {1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    // Key binds, default vanilla layout.
    int bindForward = 'W', bindBack = 'S', bindLeft = 'A', bindRight = 'D';
    int bindJump = VK_SPACE, bindSneak = VK_SHIFT, bindSprint = VK_CONTROL;
    int bindInventory = 'E', bindDrop = 'Q', bindChat = 'T', bindCommand = VK_OEM_2;
    int bindPerspective = VK_F5;
};

enum SliderId {
    SL_FOV = 1, SL_RD, SL_SENS, SL_SOUND, SL_MUSIC, SL_BRIGHT, SL_SCROLL,
    SL_CAT_MASTER = 100, SL_CAT_MUSIC, SL_CAT_JUKEBOX, SL_CAT_WEATHER, SL_CAT_BLOCKS,
    SL_CAT_HOSTILE, SL_CAT_FRIENDLY, SL_CAT_PLAYERS, SL_CAT_AMBIENT
};

// Orthographic textured-quad renderer. One dynamic vertex buffer, batches
// grouped by texture, drawn with PSMainUI (alpha blend, depth off). Vertex
// tint (cr,cg,cb) carries GUI/text colors; shade is the 0..15 light level.
class GuiRenderer {
public:
    bool Init(RenderContext& rc, AssetStore& store);
    bool LoadPlayerSkin(RenderContext& rc, AssetStore& store, const PlayerProfile& profile);
    void DrawPlayerPreview(float cx, float feetY, float scale, int mouseX, int mouseY);
    void Shutdown();
    void BeginFrame();
    void Flush(RenderContext& rc);

    void DrawQuad(const GuiTexture* tex, float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1, float shade,
                  float cr = 1.0f, float cg = 1.0f, float cb = 1.0f);
    void DrawNineSlice(const GuiTexture* tex, float x, float y, float w, float h,
                       float scale = 2.0f, float shade = 15.0f);
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
    GuiTexture CreateGuiTexture(RenderContext& rc, const uint8_t* rgba, int w, int h);

    GuiTexture texButton, texButtonHi, texButtonDisabled, texLogo, texEdition, texCrosshair;
    GuiTexture texHotbar, texHotbarSel, texFont, texDirt, texWhite, texBlack;
    GuiTexture texInvBg, texCraftBg, texFurnaceBg, texChestBg;
    GuiTexture texHeart[3];   // container, full, half
    GuiTexture texFood[3];    // empty, full, half
    GuiTexture texArmor[3];   // empty, full, half
    GuiTexture texAttackBg, texAttackProgress;
    GuiTexture texPanoramaOverlay;
    GuiTexture texSlider, texSliderHi, texSliderHandle, texSliderHandleHi;
    GuiTexture texMenuBg, texHeaderSeparator, texFooterSeparator;
    GuiTexture texTab, texTabHi, texTabSelected, texTabSelectedHi;
    GuiTexture texSkin;
    GuiTexture texItemIcon[256];
    GuiTexture texDestroy[10];
    GuiTexture texZombie;
    GuiTexture texMob[int(MobType::Count)];
    GuiTexture texClouds;

private:
    struct Batch {
        const GuiTexture* tex = nullptr;
        std::vector<MeshVertex> verts;
    };
    GuiTexture LoadGui(RenderContext& rc, AssetStore& store, const char* name);
    void DrawTextRaw(const std::string& text, float x, float y, float scale, float shade,
                     float cr, float cg, float cb);
    std::vector<Batch> mBatches;
    bool mSkinSlim = false;
    ID3D10Buffer* mVB = nullptr;
    static constexpr UINT VB_VERTS = 65536;
};

enum MenuAction {
    ACT_NONE = 0, ACT_SINGLEPLAYER, ACT_OPTIONS, ACT_QUIT, ACT_RESUME,
    ACT_QUIT_TO_TITLE, ACT_OPTIONS_DONE, ACT_RESPAWN,
    ACT_CYCLE_RD, ACT_CYCLE_FOV, ACT_CYCLE_SENS, ACT_TOGGLE_SOUND,
    ACT_TOGGLE_BOB, ACT_TOGGLE_VSYNC, ACT_TOGGLE_FS, ACT_CYCLE_DIFF,
    ACT_CREATE_WORLD, ACT_CANCEL_CREATE, ACT_CYCLE_GAMEMODE,
    ACT_CREATE_TAB_GAME, ACT_CREATE_TAB_WORLD,
    ACT_CREATE_TAB_MORE, ACT_VIDEO_OPTIONS, ACT_SOUND_OPTIONS, ACT_CONTROLS_OPTIONS,
    ACT_PROFILE, ACT_PROFILE_DONE, ACT_PROFILE_CANCEL,
    ACT_SKIN_PREV, ACT_SKIN_NEXT, ACT_SKIN_MODEL,
    ACT_NEW_WORLD, ACT_LOAD_WORLD, ACT_WORLD_PREV, ACT_WORLD_NEXT,
    ACT_CYCLE_WORLD_TYPE, ACT_TOGGLE_CHEATS, ACT_TOGGLE_KEEPINV,
    ACT_WORLD_EDIT, ACT_WORLD_DELETE, ACT_WORLD_RECREATE,
    ACT_CONFIRM_DELETE, ACT_CANCEL_DELETE,
    ACT_EDIT_RENAME, ACT_EDIT_FOLDER, ACT_EDIT_RESET_ICON, ACT_EDIT_DONE, ACT_EDIT_CANCEL,
    ACT_GAMERULES, ACT_GAMERULES_DONE,
    ACT_TOGGLE_STRUCTURES, ACT_TOGGLE_BONUS,
    ACT_VIDEO_MENU, ACT_SOUND_MENU, ACT_CONTROLS_MENU, ACT_DONE_VIDEO, ACT_DONE_CONTROLS,
    ACT_TOGGLE_FANCY, ACT_TOGGLE_SMOOTH, ACT_CYCLE_FPS, ACT_CYCLE_GUISCALE,
    ACT_CYCLE_CLOUDS, ACT_CYCLE_PARTICLES, ACT_TOGGLE_SHADOWS,
    ACT_TOGGLE_INVERT, ACT_CYCLE_RD_DOWN, ACT_CYCLE_RD_UP,
    ACT_CYCLE_RULE_RANDOMTICK,
    ACT_RULE_TOGGLE_BASE = 300,
    ACT_BIND_BASE = 400,
    ACT_WORLD_ROW = 1000
};

// Menu-configurable game rules (Create New World > More > Game Rules).
enum MenuRule {
    RULE_DAYLIGHT, RULE_MOBSPAWN, RULE_WEATHER, RULE_KEEPINV,
    RULE_MOBGRIEF, RULE_FIRETICK, RULE_COUNT
};
enum MenuKeyBind {
    BIND_FORWARD, BIND_BACK, BIND_LEFT, BIND_RIGHT, BIND_JUMP, BIND_SNEAK,
    BIND_SPRINT, BIND_INVENTORY, BIND_DROP, BIND_CHAT, BIND_COMMAND, BIND_PERSPECTIVE,
    BIND_COUNT
};
const char* DefaultBindName(int bind);
const char* KeyName(int vk);

// State of the Create New World form; main owns it and applies it on create.
struct WorldCreateInfo {
    std::string name = "New World";
    std::string seed;
    int gameMode = 0; // 0 survival, 1 creative
    int focus = 1;    // 1 name field, 2 seed field
    int tab = 0;      // 0 Game, 1 World, 2 More
    int difficulty = 2; // Independent of the current world's difficulty
    WorldGenerator generator = WorldGenerator::Climate;
    bool allowCheats = false;
    bool keepInventory = false;
    bool structures = true;
    bool bonusChest = false;
    bool rules[RULE_COUNT] = {true, true, true, false, true, true};
    int randomTickSpeed = 3;
};

// Vanilla-style screens built from real GUI sprites. The 3D background
// (panorama skybox or frozen world) is rendered by the caller first.
struct SavedWorldEntry {
    std::string path, name, details, folder, savedText;
    uint64_t savedAt = 0;
    int mode = 0;
    int difficulty = 2;
    bool cheats = false;
    uint64_t seed = 0;
    std::string generator;
    std::vector<uint8_t> icon;
    int iconW = 0, iconH = 0;
};

class MenuSystem {
public:
    std::vector<SavedWorldEntry> worlds;
    std::vector<GuiTexture> worldIcons;
    int selectedWorld = -1;
    int worldPage = 0;
    void LoadWorldIcons(RenderContext& rc);
    void Draw(GuiRenderer& ui, Screen screen, Settings& settings, WorldCreateInfo& create,
              const std::string& typed, int mouseX, int mouseY, bool leftDown,
              const std::string& splash, float timeSec, const PlayerProfile& profile,
              const std::string& editName, int editingBind);
    int HitTest(int mouseX, int mouseY) const;
    bool ConsumeChanged() { const bool c = mChanged; mChanged = false; return c; }

private:
    struct Button {
        float x, y, w, h;
        std::string label;
        int action;
        bool enabled;
        bool tab = false;
        bool selected = false;
    };
    struct Slider {
        int id;
        float x, y, w, h;
    };
    void DrawButton(GuiRenderer& ui, const Button& b, int mouseX, int mouseY);
    void DrawSlider(GuiRenderer& ui, const Slider& s, Settings& settings,
                    int mouseX, int mouseY, bool leftDown);
    std::vector<Button> mButtons;
    std::vector<Slider> mSliders;
    bool mChanged = false;
    bool mWorldListVisible = false;
};

// Default creative hotbar layout; the live hotbar is a mutable copy in main.
extern const uint8_t HOTBAR_BLOCKS[9];

// Java GUI scale 0=Auto, 1..4. The HUD was authored at the 2x sprite size, so
// Auto maps to 1.0 and explicit values map to 0.5, 1.0, 1.5, 2.0.
float GuiScaleFactor(int guiScale);

void DrawHotbar(GuiRenderer& ui, int selectedSlot, const uint8_t* hotbar, float scale = 1.0f);
void DrawHUD(GuiRenderer& ui, float attackStrength, int selectedSlot,
             const uint8_t* hotbar, int health, int hunger, int armorPoints,
             const std::string& actionBar, bool creative, const ItemSlot* stacks,
             float scale = 1.0f);

// Chat log bottom-left above the hotbar. Closed: last ten lines fade out over
// Java's ten-second window. Open: full recent log plus the input field.
struct ChatEntry {
    std::string text;
    bool error = false;
    float age = 0.0f;
};
void DrawChat(GuiRenderer& ui, const std::vector<ChatEntry>& entries, bool open,
              const std::string& input, float timeSec, float scale = 1.0f);

// Tabbed creative inventory (E in creative). The picker grid, player storage
// rows and tab strip are drawn here; input helpers below map mouse positions
// back onto tab ids, picker item ids and player slot indices.
void DrawInventory(GuiRenderer& ui, int mouseX, int mouseY, int selectedSlot,
                   const uint8_t* hotbar, const InventoryState& inv);
int InventoryHitTest(int mouseX, int mouseY);      // picker item id, or -1
int CreativeSlotHitTest(int mouseX, int mouseY);   // player slot 0..26, or -1
int CreativeHotbarHitTest(int mouseX, int mouseY); // hotbar slot 0..8, or -1
int CreativeTabHitTest(int mouseX, int mouseY);    // tab enum, or -1
bool CreativeSearchFieldHitTest(int mouseX, int mouseY);
void ScrollCreativeInventory(int deltaRows);
void ResetCreativeScroll();
void SetCreativeTab(int tab);
int CreativeTab();
void CreativeSearchAppend(char c);
void CreativeSearchBackspace();
const std::string& CreativeSearchQuery();
int FurnaceHitTest(int mouseX, int mouseY);
void DrawFurnace(GuiRenderer& ui, const FurnaceState& f, const InventoryState& inv,
                 int mouseX, int mouseY);
int ChestHitTest(int mouseX, int mouseY);
void DrawChest(GuiRenderer& ui, const ChestState& chest, const InventoryState& inv,
               int mouseX, int mouseY);

// Slot indices for the survival screen: 0..26 storage, 27..35 hotbar,
// 36..44 crafting grid (3x3; the 2x2 uses 36,37,39,40), 45 = craft result.
void DrawSurvivalInventory(GuiRenderer& ui, const InventoryState& model, bool table,
                           int mouseX, int mouseY, const std::string& message);
int SurvivalInvHitTest(int mouseX, int mouseY, bool table);

} // namespace potato