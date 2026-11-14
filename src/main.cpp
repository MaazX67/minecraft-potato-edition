#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "engine.h"
#include "renderer.h"
#include "assets.h"
#include "world.h"
#include "sound.h"
#include "gui.h"
#include "registry.h"
#include "commands.h"
#include "shader_embed.h"
#include "json.hpp"

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
static PlayerProfile gProfile;
static PlayerProfile gProfileDraft;
static Screen gProfileReturn = Screen::Title;
static bool gProfileNameSelected = false;
static FlyCamera gCamera;
static PlayerBody gBody;
static SurvivalState gSurvival;
static AttackCooldown gCooldown;
static PlayerState gPlayer;
static ParticleSystem gParticles;
static RenderContext gRC;
static Atlas gAtlas;
static AssetStore gStore;
static std::string gAssetVersion = "unknown";
static BlockTiles gTiles;
static GuiRenderer gGui;
static MenuSystem gMenu;
static SoundEngine gSound;
static World* gWorld = nullptr;
static std::unordered_map<uint64_t, GpuChunk> gGpu;
static uint64_t gSweepCursor = 0;
static ID3D10ShaderResourceView* gParticleSRV = nullptr;
static ID3D10ShaderResourceView* gBubbleSRV = nullptr;
static ID3D10Buffer* gParticleVB = nullptr;
static ID3D10ShaderResourceView* gWhiteSRV = nullptr;
static ID3D10Buffer* gOutlineVB = nullptr;
static ID3D10Buffer* gZombieVB = nullptr;
static ID3D10Buffer* gHeldVB = nullptr;
static uint8_t gItemPixels[256][ATLAS_TILE * ATLAS_TILE * 4];
static bool gItemHasPixels[256] = {};
static std::vector<Zombie> gZombies;
static FurnaceState gFurnace;
static int gDimension = 0; // 0 overworld, 1 nether
static WorldGenerator gOverworldGen = WorldGenerator::Climate;
static EditMap gDimEdits[2];
static float gDimPos[2][3] = {{8.5f, 90.0f, 8.5f}, {8.5f, 90.0f, 8.5f}};
static bool gDimVisited[2] = {true, false};
static float gPortalCooldown = 0;
static float gElytraWear = 0;
static std::unordered_map<uint64_t, ChestState> gChests;
static uint64_t gChestKey = 0;
static uint64_t ChestKey(int x, int z) {
    return (uint64_t(uint32_t(x)) << 32) | uint64_t(uint32_t(z));
}
static void BuildDimensionWorld(int dim, EditMap edits);
static HurtResistance gPlayerResistance;
static float gMobSpawnTimer = 0;
static uint64_t gCombatHits = 0, gCriticalHits = 0, gZombieKills = 0;
static uint8_t gAttackItem = 0;
static bool gToolsTest = false, gCombatTest = false;
static int gHotbarSlot = 0;
static uint8_t gHotbar[9];
static std::string gActionBar;
static float gActionBarUntil = 0;
static float gNowMs = 0;
static float gTimeSec = 0;
static float gFps = 0;
static bool gRunning = true;
static bool gDebug = false;
static bool gHideHud = false;
static int gPerspective = 0; // 0 first person, 1 third person back, 2 third person front
static float gZoom = 1.0f;    // C-key spyglass-style zoom factor
static bool gFlyHook = false;
static bool gSpawned = false;
static float gFallStartY = 0;
static float gLastSpaceMs = -1000;
static bool gHaveTarget = false;
static float gMiningProgress = 0, gMiningElapsed = 0, gMiningSound = 0, gMiningDelay = 0;
static int gMiningX = 0, gMiningY = -1, gMiningZ = 0;
static uint8_t gMiningItem = 0, gMiningBlock = 0;
static uint64_t gBrokenBlocks = 0;
static int gTargetX = 0, gTargetY = 0, gTargetZ = 0;
static float gStepTimer = 0.3f;
static bool gWasInWater = false;
static std::string gSplash = "As seen on TV!";
static WorldCreateInfo gCreate;
static int gGameMode = 0; // 0 survival, 1 creative
static bool gAllowCheats = false;
static bool gKeepInventory = false;
static bool gStructuresEnabled = true;
static bool gBonusChestPending = false;
static uint64_t gWorldSeed = 1337;
static InventoryState gInventory;
static bool gCraftTable = false;
static bool gInventoryTest = false;
static bool gAutomation = false;
static bool gFocused = true;
static bool gCursorCaptured = false;
static std::string gInventoryMessage;
static std::string gSavePath;
static std::string gWorldName;
static bool gResumePending = false;
static Screen gResumeScreen = Screen::Game;
static WorldClock gWorldClock;
static SkyTimeline gSkyTimeline;
static bool gFreezeTime = false;
static bool gScreenshotRequested = false;

struct ChatUIState {
    bool open = false;
    std::string input;
    std::vector<ChatEntry> entries;
};
static ChatUIState gChat;
static ChatHistory gChatHistory;
static int gWeather = 0;             // 0 clear, 1 rain, 2 thunder
static float gWeatherTicksLeft = 0;  // command-set duration countdown
static float gWeatherTimer = 2400.0f;
static float gRainLevel = 0, gThunderLevel = 0;
static float gRainSpawnTimer = 0;
static bool gRuleDaylightCycle = true, gRuleMobSpawning = true, gRuleWeatherCycle = true;
static bool gRuleMobGriefing = true, gRuleFireTick = true;
static int gRuleRandomTick = 3;
static std::string gEditName;
static int gEditingBind = -1;
static int gLastWorldClickIndex = -1;
static float gLastWorldClickMs = -1000.0f;
static int gLastInvClickIndex = -1;
static float gLastInvClickMs = -1000.0f;
static float gScrollAccum = 0.0f;
static PerlinNoise* gWeatherNoise = nullptr;

static void SetScreen(HWND hwnd, Screen s);
static void UpdateCursorCapture(HWND hwnd);
static void SyncHotbar();

static bool IsWorldScreen(Screen s) {
    return s == Screen::Game || s == Screen::Pause || s == Screen::Inventory || s == Screen::Furnace || s == Screen::Chest || s == Screen::Death;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (gAutomation) {
        if (msg == WM_APP + 1) {
            gInput.mouseDX += float(int(wp));
            gInput.mouseDY += float(int(lp));
            return 0;
        }
        if (msg == WM_APP + 2) {
            gWorldClock.Set(uint64_t(wp) | (uint64_t(uint32_t(lp)) << 32));
            return 0;
        }
        if (msg == WM_APP + 3) {
            gFreezeTime = wp != 0;
            return 0;
        }
        if (msg == WM_APP + 6 && gWorld && gSpawned &&
            (gWorld->Generator() == WorldGenerator::Climate ||
             gWorld->Generator() == WorldGenerator::LargeBiomes ||
             gWorld->Generator() == WorldGenerator::Amplified)) {
            const int centerX = int(floorf(gBody.x)), centerZ = int(floorf(gBody.z));
            for (int dz = -48; dz <= 48; dz += 2) for (int dx = -48; dx <= 48; dx += 2) {
                const int x = centerX + dx, z = centerZ + dz;
                const int surface = gWorld->SurfaceY(x, z);
                if (surface < 0) continue;
                for (int y = 8; y < std::min(surface - 8, 60); ++y) {
                    if (!(BLOCK_FLAGS[gWorld->GetBlock(x, y - 1, z)] & BF_SOLID)) continue;
                    bool open = true;
                    for (int forward = 0; forward <= 2; ++forward)
                        for (int height = 0; height < 2; ++height)
                            if (gWorld->GetBlock(x, y + height, z - forward) != BLOCK_AIR) open = false;
                    if (!open) continue;
                    gBody.x = float(x) + 0.5f; gBody.y = float(y); gBody.z = float(z) + 0.5f;
                    gBody.vx = gBody.vy = gBody.vz = 0;
                    gBody.flying = false;
                    gCamera.yaw = 0; gCamera.pitch = -0.1f;
                    gFallStartY = gBody.y;
                    return 0;
                }
            }
            return 0;
        }
        if (msg == WM_APP + 5 && gCombatTest && gWorld && gSpawned) {
            if (wp <= 1) {
                gWorld->SetBlock(8, 4, 7, wp ? BLOCK_STONE : BLOCK_AIR);
                gWorld->SetBlock(8, 5, 7, wp ? BLOCK_STONE : BLOCK_AIR);
            } else if (wp == 2 && !gZombies.empty()) {
                gZombies[0].passive = false;
                gZombies[0].body.x = gBody.x;
                gZombies[0].body.y = gBody.y;
                gZombies[0].body.z = gBody.z - 1.6f;
            }
            return 0;
        }
        if (msg == WM_APP + 4) {
            gScreenshotRequested = true;
            return 0;
        }
        if (msg >= WM_APP + WM_KEYFIRST && msg <= WM_APP + WM_MOUSELAST) {
            msg -= WM_APP;
        } else if ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
                   (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)) {
            return 0;
        }
    }
    switch (msg) {
        case WM_ACTIVATEAPP:
            gFocused = wp != 0;
            if (!gFocused && !gAutomation) {
                gInput = InputState{};
                if (gScreen == Screen::Game) SetScreen(hwnd, Screen::Pause);
            }
            UpdateCursorCapture(hwnd);
            return 0;
        case WM_INPUT: {
            if (gAutomation || !gFocused || gScreen != Screen::Game)
                return DefWindowProcA(hwnd, msg, wp, lp);
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
            return DefWindowProcA(hwnd, msg, wp, lp);
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
        case WM_CHAR:
            if (wp < 256) gInput.typed += char(wp);
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
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void UpdateCursorCapture(HWND hwnd) {
    const bool capture = gScreen == Screen::Game && gFocused && !gAutomation && !gChat.open;
    if (capture == gCursorCaptured) return;
    gCursorCaptured = capture;
    ShowCursor(capture ? FALSE : TRUE);
    if (capture) {
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

static void SetScreen(HWND hwnd, Screen s) {
    gScreen = s;
    gMiningProgress = gMiningElapsed = 0;
    gMiningY = -1;
    gInput.mouseDX = gInput.mouseDY = 0;
    gInput.leftClick = gInput.rightClick = false;
    gInput.wheel = 0;
    UpdateCursorCapture(hwnd);
}

static void StartGame() {
    gSurvival = SurvivalState{};
    gCooldown = AttackCooldown{};
    gAttackItem = 255;
    gPlayerResistance = HurtResistance{};
    gBody = PlayerBody{};
    gBody.flying = false;
    gCamera.yaw = 0;
    gCamera.pitch = 0;
    gLastSpaceMs = -1000;
    gSpawned = false;
    gResumePending = false;
    gResumeScreen = Screen::Game;
    gActionBar.clear();
    gChat = ChatUIState{};
    gChatHistory = ChatHistory{};
    gWeather = 0;
    gWeatherTicksLeft = 0;
    gRainLevel = gThunderLevel = 0;
    gWeatherTimer = 2400.0f;
    SetScreen(gHwnd, Screen::Game);
}

// Java seed rules: blank -> random, all-digits -> the number itself,
// anything else -> Java String.hashCode (h = 31*h + c, 32-bit wrap).
static uint64_t ParseSeed(const std::string& text) {
    if (text.empty())
        return MixSeed64(GetTickCount64() ^ (uint64_t(rand()) << 32) ^ uint64_t(rand()));
    char* end = nullptr;
    const long long v = strtoll(text.c_str(), &end, 10);
    if (end != text.c_str() && *end == '\0') return uint64_t(v);
    // Java's String.hashCode stays 32-bit but is sign-extended into the seed.
    int32_t h = 0;
    for (char c : text) h = int32_t(31u * uint32_t(h) + uint32_t(uint8_t(c)));
    return uint64_t(int64_t(h));
}

static int ResolveTile(const char* tex) {
    if (!tex || !tex[0]) return 0;
    const int t = gAtlas.FindTile(std::string("minecraft/textures/block/") + tex + ".png");
    return t < 0 ? 0 : t;
}

static void SyncBinds() {
    gBinds.forward = gSettings.bindForward;
    gBinds.back = gSettings.bindBack;
    gBinds.left = gSettings.bindLeft;
    gBinds.right = gSettings.bindRight;
    gBinds.jump = gSettings.bindJump;
    gBinds.sneak = gSettings.bindSneak;
    gBinds.sprint = gSettings.bindSprint;
}

static void ApplySettings() {
    gWorld->SetRenderDistance(gSettings.renderDistance);
    gCamera.sensitivity = gSettings.sensitivity;
    gSound.SetEnabled(gSettings.sound);
    gSound.SetVolume(gSettings.soundVolume);
    SyncBinds();
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

static std::string PreferencesPath() {
    if (gAutomation) return "preferences.json";
    char appData[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableA("APPDATA", appData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    const std::string directory = std::string(appData) + "/MinecraftPotatoEdition";
    if (!CreateDirectoryA(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return {};
    return directory + "/preferences.json";
}

static bool SavePreferences() {
    const std::string path = PreferencesPath();
    if (path.empty()) return false;
    const nlohmann::json data = {
        {"name", gProfile.name}, {"skin", gProfile.skin}, {"slim", gProfile.slim},
        {"renderDistance", gSettings.renderDistance}, {"fov", gSettings.fov},
        {"sensitivity", gSettings.sensitivity}, {"sound", gSettings.sound},
        {"soundVolume", gSettings.soundVolume}, {"brightness", gSettings.brightness},
        {"viewBobbing", gSettings.viewBobbing}, {"vsync", gSettings.vsync},
        {"fullscreen", gSettings.fullscreen}, {"difficulty", gSettings.difficulty},
        {"fancyGraphics", gSettings.fancyGraphics}, {"smoothLighting", gSettings.smoothLighting},
        {"maxFramerate", gSettings.maxFramerate}, {"guiScale", gSettings.guiScale},
        {"clouds", gSettings.clouds}, {"particles", gSettings.particles},
        {"entityShadows", gSettings.entityShadows}, {"invertMouse", gSettings.invertMouse},
        {"scrollSensitivity", gSettings.scrollSensitivity},
        {"musicVolume", gSettings.musicVolume},
        {"categoryVolume", std::vector<float>(gSettings.categoryVolume, gSettings.categoryVolume + 9)},
        {"binds", std::vector<int>{gSettings.bindForward, gSettings.bindBack, gSettings.bindLeft,
                                   gSettings.bindRight, gSettings.bindJump, gSettings.bindSneak,
                                   gSettings.bindSprint, gSettings.bindInventory, gSettings.bindDrop,
                                   gSettings.bindChat, gSettings.bindCommand, gSettings.bindPerspective}}
    };
    const std::string pending = path + ".tmp";
    std::ofstream file(pending, std::ios::binary | std::ios::trunc);
    file << data.dump(2);
    file.close();
    return !file.fail() && MoveFileExA(pending.c_str(), path.c_str(),
                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static void LoadPreferences() {
    std::ifstream file(PreferencesPath(), std::ios::binary);
    if (!file) return;
    const nlohmann::json data = nlohmann::json::parse(file, nullptr, false);
    if (!data.is_object()) return;
    try {
        PlayerProfile profile;
        Settings settings;
        profile.name = data.value("name", profile.name);
        if (!IsValidPlayerName(profile.name)) profile.name = "Player";
        profile.skin = std::clamp(data.value("skin", 0), 0, 8);
        profile.slim = data.value("slim", false);
        settings.renderDistance = std::clamp(data.value("renderDistance", 8), 2, 16);
        settings.fov = std::clamp(data.value("fov", 70.0f), 30.0f, 110.0f);
        settings.sensitivity = std::clamp(data.value("sensitivity", 0.0025f), 0.0f, 0.005f);
        settings.sound = data.value("sound", true);
        settings.soundVolume = std::clamp(data.value("soundVolume", 1.0f), 0.0f, 1.0f);
        settings.brightness = std::clamp(data.value("brightness", 0.5f), 0.0f, 1.0f);
        settings.viewBobbing = data.value("viewBobbing", true);
        settings.vsync = data.value("vsync", false);
        settings.fullscreen = data.value("fullscreen", false);
        settings.difficulty = std::clamp(data.value("difficulty", 2), 0, 3);
        settings.fancyGraphics = data.value("fancyGraphics", true);
        settings.smoothLighting = data.value("smoothLighting", true);
        settings.maxFramerate = std::clamp(data.value("maxFramerate", 120), 0, 260);
        settings.guiScale = std::clamp(data.value("guiScale", 0), 0, 4);
        settings.clouds = std::clamp(data.value("clouds", 2), 0, 2);
        settings.particles = std::clamp(data.value("particles", 2), 0, 2);
        settings.entityShadows = data.value("entityShadows", true);
        settings.invertMouse = data.value("invertMouse", false);
        settings.scrollSensitivity = std::clamp(data.value("scrollSensitivity", 1.0f), 0.2f, 2.0f);
        settings.musicVolume = std::clamp(data.value("musicVolume", 0.5f), 0.0f, 1.0f);
        if (data.contains("categoryVolume") && data["categoryVolume"].is_array())
            for (int i = 0; i < 9 && i < int(data["categoryVolume"].size()); ++i)
                settings.categoryVolume[i] = std::clamp(data["categoryVolume"][i].get<float>(), 0.0f, 1.0f);
        if (data.contains("binds") && data["binds"].is_array() && data["binds"].size() == 12) {
            int* binds[12] = {&settings.bindForward, &settings.bindBack, &settings.bindLeft,
                              &settings.bindRight, &settings.bindJump, &settings.bindSneak,
                              &settings.bindSprint, &settings.bindInventory, &settings.bindDrop,
                              &settings.bindChat, &settings.bindCommand, &settings.bindPerspective};
            for (int i = 0; i < 12; ++i) *binds[i] = std::clamp(data["binds"][i].get<int>(), 1, 255);
        }
        gProfile = profile;
        gSettings = settings;
        SyncBinds();
    } catch (const nlohmann::json::exception&) {
        return;
    }
}

static std::string SavesDirectory() {
    const std::string preferences = PreferencesPath();
    if (preferences.empty()) return {};
    const size_t slash = preferences.find_last_of("/\\");
    const std::string directory = slash == std::string::npos ? "saves" : preferences.substr(0, slash) + "/saves";
    if (!CreateDirectoryA(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return {};
    return directory;
}

static nlohmann::json ReadWorldSave(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto size = file.tellg();
    if (size <= 0 || size > 64 * 1024 * 1024) return {};
    file.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) return {};
    return nlohmann::json::from_msgpack(bytes, true, false);
}

static std::string NewSavePath() {
    const std::string root = SavesDirectory();
    if (root.empty()) return {};
    for (unsigned i = 0; i < 100; ++i) {
        const std::string directory = root + "/world-" + std::to_string(GetTickCount64()) + "-" + std::to_string(i);
        if (CreateDirectoryA(directory.c_str(), nullptr)) return directory + "/world.potato";
        if (GetLastError() != ERROR_ALREADY_EXISTS) return {};
    }
    return {};
}

static std::string FormatSavedTime(uint64_t ft) {
    if (!ft) return "Never";
    FILETIME fileTime{uint32_t(ft & 0xFFFFFFFFu), uint32_t(ft >> 32)};
    FILETIME localTime;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&fileTime, &localTime) || !FileTimeToSystemTime(&localTime, &st))
        return "Unknown";
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute);
    return buf;
}

static void RefreshWorldList() {
    gMenu.worlds.clear();
    gMenu.selectedWorld = -1;
    gMenu.worldPage = 0;
    const std::string root = SavesDirectory();
    if (root.empty()) { gMenu.LoadWorldIcons(gRC); return; }
    WIN32_FIND_DATAA entry = {};
    HANDLE search = FindFirstFileA((root + "/world-*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) { gMenu.LoadWorldIcons(gRC); return; }
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const std::string path = root + "/" + entry.cFileName + "/world.potato";
        const auto save = ReadWorldSave(path);
        try {
            if (!save.is_object()) continue;
            const int format = save.at("format").get<int>();
            WorldGenerator generator;
            if ((format != 1 && format != 2 && format != 3) ||
                !ParseGenerator(save.at("generator").get<std::string>(), generator)) continue;
            SavedWorldEntry e;
            e.path = path;
            e.name = save.at("name").get<std::string>();
            e.folder = entry.cFileName;
            e.savedAt = save.at("savedAt").get<uint64_t>();
            e.savedText = FormatSavedTime(e.savedAt);
            e.mode = save.value("mode", 0);
            e.difficulty = save.value("difficulty", 2);
            e.cheats = save.value("cheats", false);
            e.seed = save.at("seed").get<uint64_t>();
            e.generator = save.at("generator").get<std::string>();
            e.details = std::string(e.mode == 1 ? "Creative Mode" : "Survival Mode") +
                ", Seed: " + std::to_string(e.seed);
            if (save.contains("icon") && save["icon"].is_binary()) {
                const auto& iconBytes = save["icon"].get_binary();
                e.iconW = save.value("iconW", 0);
                e.iconH = save.value("iconH", 0);
                if (e.iconW > 0 && e.iconH > 0 &&
                    size_t(e.iconW) * size_t(e.iconH) * 4 == iconBytes.size())
                    e.icon.assign(iconBytes.begin(), iconBytes.end());
            }
            gMenu.worlds.push_back(std::move(e));
        } catch (const nlohmann::json::exception&) {
            continue;
        }
    } while (FindNextFileA(search, &entry));
    FindClose(search);
    std::sort(gMenu.worlds.begin(), gMenu.worlds.end(), [](const SavedWorldEntry& a, const SavedWorldEntry& b) {
        return a.savedAt > b.savedAt;
    });
    if (!gMenu.worlds.empty()) gMenu.selectedWorld = 0;
    gMenu.LoadWorldIcons(gRC);
}

static void ResetWorld(uint64_t seed, EditMap edits = {}, WorldGenerator generator = WorldGenerator::Climate);

static void DeleteTree(const std::string& dir) {
    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA((dir + "/*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            const std::string p = dir + "/" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p);
            else DeleteFileA(p.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
}

static bool WriteWorldSaveJson(const std::string& path, const nlohmann::json& data) {
    const auto bytes = nlohmann::json::to_msgpack(data);
    const std::string pending = path + ".tmp";
    std::ofstream file(pending, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    file.close();
    return !file.fail() && MoveFileExA(pending.c_str(), path.c_str(),
                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static void DeleteSelectedWorld() {
    if (gMenu.selectedWorld < 0 || gMenu.selectedWorld >= int(gMenu.worlds.size())) return;
    const std::string path = gMenu.worlds[gMenu.selectedWorld].path;
    const size_t slash = path.find_last_of("/\\");
    const std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash);
    if (!folder.empty()) DeleteTree(folder);
    RefreshWorldList();
}

static void RenameSelectedWorld(const std::string& newName) {
    if (gMenu.selectedWorld < 0 || gMenu.selectedWorld >= int(gMenu.worlds.size())) return;
    if (newName.empty()) return;
    const std::string path = gMenu.worlds[gMenu.selectedWorld].path;
    auto data = ReadWorldSave(path);
    if (!data.is_object()) return;
    data["name"] = newName;
    WriteWorldSaveJson(path, data);
    const int keep = gMenu.selectedWorld;
    RefreshWorldList();
    if (!gMenu.worlds.empty()) gMenu.selectedWorld = std::min(keep, int(gMenu.worlds.size()) - 1);
}

static void ResetSelectedWorldIcon() {
    if (gMenu.selectedWorld < 0 || gMenu.selectedWorld >= int(gMenu.worlds.size())) return;
    const std::string path = gMenu.worlds[gMenu.selectedWorld].path;
    auto data = ReadWorldSave(path);
    if (!data.is_object()) return;
    data.erase("icon");
    data.erase("iconW");
    data.erase("iconH");
    WriteWorldSaveJson(path, data);
    const int keep = gMenu.selectedWorld;
    RefreshWorldList();
    if (!gMenu.worlds.empty()) gMenu.selectedWorld = std::min(keep, int(gMenu.worlds.size()) - 1);
}

static void RecreateSelectedWorld() {
    if (gMenu.selectedWorld < 0 || gMenu.selectedWorld >= int(gMenu.worlds.size())) return;
    const SavedWorldEntry& e = gMenu.worlds[gMenu.selectedWorld];
    WorldGenerator generator = WorldGenerator::Climate;
    ParseGenerator(e.generator, generator);
    const std::string path = NewSavePath();
    if (path.empty()) return;
    gStructuresEnabled = true;
    ResetWorld(e.seed, {}, generator);
    gSavePath = path;
    gWorldName = e.name + " (Re-Created)";
    gGameMode = e.mode;
    gAllowCheats = e.cheats;
    gSettings.difficulty = e.difficulty;
    gInventory = InventoryState{};
    gCraftTable = false;
    gHotbarSlot = 0;
    if (gGameMode == 1) memcpy(gHotbar, HOTBAR_BLOCKS, sizeof(gHotbar));
    else memset(gHotbar, 0, sizeof(gHotbar));
    StartGame();
}

static void OpenSelectedWorldFolder() {
    if (gMenu.selectedWorld < 0 || gMenu.selectedWorld >= int(gMenu.worlds.size())) return;
    const std::string path = gMenu.worlds[gMenu.selectedWorld].path;
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return;
    ShellExecuteA(nullptr, "open", path.substr(0, slash).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void ResetWorld(uint64_t seed, EditMap edits, WorldGenerator generator) {
    gWorldClock.Set(0);
    gZombies.clear();
    gMobSpawnTimer = 0;
    for (auto& kv : gGpu) {
        SafeRelease(kv.second.vb);
        SafeRelease(kv.second.ib);
    }
    gGpu.clear();
    gSweepCursor = 0;
    delete gWorld;
    gWorldSeed = seed;
    gWorld = new World(seed, gSettings.renderDistance, generator);
    gWorld->SetGenerateStructures(gStructuresEnabled);
    gWorld->SetBlockTiles(gTiles);
    gWorld->SetEdits(std::move(edits));
    gCraftTable = false;
    gInventoryMessage.clear();
    gResumePending = false;
    gOverworldGen = generator;
    gDimension = 0;
    gDimVisited[0] = true; gDimVisited[1] = false;
    gDimEdits[0].clear(); gDimEdits[1].clear();
    gPortalCooldown = 0;
    gFurnace = {};
    gChests.clear();
    gChestKey = 0;
    delete gWeatherNoise;
    gWeatherNoise = nullptr; // lazily re-seeded from the new world seed
    gWeather = 0;
    gWeatherTicksLeft = 0;
    gRainLevel = gThunderLevel = 0;
}

// Java's 64x64 world icon: nearest-downsample of the live back buffer.
static std::vector<uint8_t> CaptureWorldIcon(int& outW, int& outH) {
    outW = outH = 0;
    if (!gRC.device || !gRC.swapChain) return {};
    ID3D10Texture2D* back = nullptr;
    if (FAILED(gRC.swapChain->GetBuffer(0, __uuidof(ID3D10Texture2D),
                                        reinterpret_cast<void**>(&back))) || !back) return {};
    D3D10_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    D3D10_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D10_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    staging.MipLevels = 1;
    staging.ArraySize = 1;
    staging.SampleDesc.Count = 1;
    staging.SampleDesc.Quality = 0;
    ID3D10Texture2D* copy = nullptr;
    if (FAILED(gRC.device->CreateTexture2D(&staging, nullptr, &copy))) {
        back->Release();
        return {};
    }
    gRC.device->CopyResource(copy, back);
    std::vector<uint8_t> pixels;
    D3D10_MAPPED_TEXTURE2D mapped{};
    if (SUCCEEDED(copy->Map(0, D3D10_MAP_READ, 0, &mapped))) {
        outW = outH = 64;
        pixels.resize(64 * 64 * 4);
        const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                          desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        for (int y = 0; y < 64; ++y) {
            const int sy = int(size_t(y) * desc.Height / 64);
            const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(sy) * mapped.RowPitch;
            for (int x = 0; x < 64; ++x) {
                const int sx = int(size_t(x) * desc.Width / 64);
                const uint8_t* src = row + size_t(sx) * 4;
                uint8_t* dst = pixels.data() + (size_t(y) * 64 + x) * 4;
                dst[0] = bgra ? src[2] : src[0];
                dst[1] = src[1];
                dst[2] = bgra ? src[0] : src[2];
                dst[3] = 255;
            }
        }
        copy->Unmap(0);
    }
    copy->Release();
    back->Release();
    return pixels;
}

static bool SaveCurrentWorld() {
    if (gSavePath.empty()) return true;
    if (!gSpawned) return false;
    auto slot = [](const ItemSlot& s) { return nlohmann::json::array({s.id, s.count}); };
    nlohmann::json inventory = nlohmann::json::array(), craft = nlohmann::json::array();
    nlohmann::json toolDamage = {{"slots", nlohmann::json::array()}, {"craft", nlohmann::json::array()},
                                  {"cursor", gInventory.cursor.damage}};
    for (const auto& s : gInventory.slots) { inventory.push_back(slot(s)); toolDamage["slots"].push_back(s.damage); }
    for (const auto& s : gInventory.craft) { craft.push_back(slot(s)); toolDamage["craft"].push_back(s.damage); }
    nlohmann::json armor = nlohmann::json::array(), armorDamage = nlohmann::json::array();
    for (const auto& s : gInventory.armor) { armor.push_back(slot(s)); armorDamage.push_back(s.damage); }
    nlohmann::json chests = nlohmann::json::array();
    for (const auto& kv : gChests) {
        nlohmann::json slots = nlohmann::json::array();
        for (const auto& s : kv.second.slots) slots.push_back(slot(s));
        chests.push_back({{"x", int32_t(kv.first >> 32)}, {"z", int32_t(kv.first & 0xFFFFFFFFu)},
                          {"slots", slots}});
    }
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const uint64_t timestamp = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    const nlohmann::json data = {
        {"format", 3}, {"generator", GeneratorName(gOverworldGen)}, {"name", gWorldName},
        {"worldTime", gWorldClock.Ticks()},
        {"savedAt", timestamp}, {"seed", gWorldSeed}, {"mode", gGameMode},
        {"cheats", gAllowCheats}, {"keepInventory", gKeepInventory},
        {"difficulty", gSettings.difficulty}, {"position", {gBody.x, gBody.y, gBody.z}},
        {"rotation", {gCamera.yaw, gCamera.pitch}}, {"flying", gBody.flying},
        {"health", gSurvival.health}, {"hunger", gSurvival.hunger},
        {"saturation", gSurvival.saturation}, {"exhaustion", gSurvival.exhaustion},
        {"selected", gHotbarSlot}, {"hotbar", std::vector<uint8_t>(gHotbar, gHotbar + 9)},
        {"inventory", inventory}, {"craft", craft}, {"cursor", slot(gInventory.cursor)}, {"toolDamage", toolDamage},
        {"armor", armor}, {"armorDamage", armorDamage},
        {"offhand", slot(gInventory.offhand)}, {"offhandDamage", gInventory.offhand.damage},
        {"inventoryOpen", gScreen == Screen::Inventory}, {"craftTable", gCraftTable},
        {"dimension", gDimension}, {"weather", gWeather}, {"weatherTicks", int(gWeatherTicksLeft)},
        {"doDaylightCycle", gRuleDaylightCycle}, {"doMobSpawning", gRuleMobSpawning},
        {"doWeatherCycle", gRuleWeatherCycle}, {"mobGriefing", gRuleMobGriefing},
        {"doFireTick", gRuleFireTick}, {"randomTickSpeed", gRuleRandomTick},
        {"structures", gStructuresEnabled}, {"bonusChest", false},
        {"otherEdits", nlohmann::json::binary(EncodeEdits(gDimEdits[1 - gDimension]))},
        {"otherPos", {gDimPos[1 - gDimension][0], gDimPos[1 - gDimension][1], gDimPos[1 - gDimension][2]}},
        {"otherVisited", gDimVisited[1 - gDimension]},
        {"furnace", {{"input", slot(gFurnace.input)}, {"fuel", slot(gFurnace.fuel)},
                     {"output", slot(gFurnace.output)}, {"burn", gFurnace.burn},
                     {"burnTotal", gFurnace.burnTotal}, {"cook", gFurnace.cook},
                     {"x", gFurnace.x}, {"y", gFurnace.y}, {"z", gFurnace.z}}},
        {"chests", chests},
        {"edits", nlohmann::json::binary(EncodeEdits(gWorld->SnapshotEdits()))}
    };
    nlohmann::json dataWithIcon = data;
    int iconW = 0, iconH = 0;
    std::vector<uint8_t> icon = CaptureWorldIcon(iconW, iconH);
    if (!icon.empty()) {
        dataWithIcon["iconW"] = iconW;
        dataWithIcon["iconH"] = iconH;
        dataWithIcon["icon"] = nlohmann::json::binary(icon);
    }
    const auto bytes = nlohmann::json::to_msgpack(dataWithIcon);
    if (bytes.size() > 64 * 1024 * 1024) return false;
    const std::string pending = gSavePath + ".tmp";
    std::ofstream file(pending, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    file.close();
    return !file.fail() && MoveFileExA(pending.c_str(), gSavePath.c_str(),
                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static bool LoadWorldSave(const std::string& path) {
    const auto data = ReadWorldSave(path);
    try {
        if (!data.is_object()) return false;
        const int format = data.at("format").get<int>();
        WorldGenerator generator;
        if ((format != 1 && format != 2 && format != 3) ||
            !ParseGenerator(data.at("generator").get<std::string>(), generator)) return false;
        uint64_t worldTime = 0;
        if (format == 2) {
            if (!data.at("worldTime").is_number_unsigned()) return false;
            worldTime = data.at("worldTime").get<uint64_t>();
        }
        const uint64_t seed = data.at("seed").get<uint64_t>();
        const std::string name = data.at("name").get<std::string>();
        const int mode = data.at("mode").get<int>(), difficulty = data.at("difficulty").get<int>();
        const auto position = data.at("position").get<std::vector<float>>();
        const auto rotation = data.at("rotation").get<std::vector<float>>();
        if (mode < 0 || mode > 1 || difficulty < 0 || difficulty > 3 || position.size() != 3 ||
            rotation.size() != 2 || name.empty() || name.size() > 128) return false;
        for (float v : position) if (!std::isfinite(v) || fabsf(v) > 1000000) return false;
        for (float v : rotation) if (!std::isfinite(v) || fabsf(v) > 1000000) return false;
        if (position[1] < -64 || position[1] > 1024) return false;
        const bool legacy = format < 3;
        InventoryState inventory;
        auto readSlot = [legacy](const nlohmann::json& value, ItemSlot& s) {
            if (!value.is_array() || value.size() != 2) return false;
            int id = value.at(0).get<int>();
            const int count = value.at(1).get<int>();
            if (legacy && id >= 64 && id <= 104) id += 96;
            if (!IsValidItem(id) || count < 0 || count > ItemStackLimit(uint8_t(id)) || (id == 0) != (count == 0)) return false;
            s = {uint8_t(id), count};
            return true;
        };
        const auto& slots = data.at("inventory");
        const auto& craft = data.at("craft");
        if (!slots.is_array() || slots.size() != 36 || !craft.is_array() || craft.size() != 9) return false;
        for (int i = 0; i < 36; ++i) if (!readSlot(slots.at(i), inventory.slots[i])) return false;
        for (int i = 0; i < 9; ++i) if (!readSlot(craft.at(i), inventory.craft[i])) return false;
        if (!readSlot(data.at("cursor"), inventory.cursor)) return false;
        if (data.contains("toolDamage")) {
            const auto& damage = data.at("toolDamage");
            auto readDamage = [](const nlohmann::json& value, ItemSlot& slot) {
                if (!value.is_number_integer()) return false;
                const int d = value.get<int>();
                const ToolDef* tool = FindTool(slot.id);
                if (d < 0 || (tool ? d >= tool->durability : d != 0)) return false;
                slot.damage = uint16_t(d);
                return true;
            };
            if (!damage.at("slots").is_array() || damage.at("slots").size() != 36 ||
                !damage.at("craft").is_array() || damage.at("craft").size() != 9) return false;
            for (int i = 0; i < 36; ++i) if (!readDamage(damage.at("slots").at(i), inventory.slots[i])) return false;
            for (int i = 0; i < 9; ++i) if (!readDamage(damage.at("craft").at(i), inventory.craft[i])) return false;
            if (!readDamage(damage.at("cursor"), inventory.cursor)) return false;
        }
        if (data.contains("armor")) {
            const auto& armor = data.at("armor");
            if (!armor.is_array() || armor.size() != 4) return false;
            for (int i = 0; i < 4; ++i) if (!readSlot(armor.at(i), inventory.armor[i])) return false;
            if (data.contains("armorDamage")) {
                const auto& ad = data.at("armorDamage");
                if (!ad.is_array() || ad.size() != 4) return false;
                for (int i = 0; i < 4; ++i) {
                    const int d = ad.at(i).get<int>();
                    const ArmorDef* a = FindArmor(inventory.armor[i].id);
                    if (d < 0 || (a ? d >= a->durability : d != 0)) return false;
                    inventory.armor[i].damage = uint16_t(d);
                }
            }
        }
        if (data.contains("offhand")) {
            if (!readSlot(data.at("offhand"), inventory.offhand)) return false;
            if (data.contains("offhandDamage")) {
                const int d = data.at("offhandDamage").get<int>();
                const ToolDef* tool = FindTool(inventory.offhand.id);
                const ArmorDef* a = FindArmor(inventory.offhand.id);
                const int max = tool ? tool->durability : (a ? a->durability : 0);
                if (d < 0 || (max ? d >= max : d != 0)) return false;
                inventory.offhand.damage = uint16_t(d);
            }
        }
        auto hotbar = data.at("hotbar").get<std::vector<uint8_t>>();
        if (hotbar.size() != 9) return false;
        for (auto& slotId : hotbar) {
            int id = slotId;
            if (legacy && id >= 64 && id <= 104) id += 96;
            if (!IsValidItem(id)) return false;
            slotId = uint8_t(id);
        }
        const int selected = data.at("selected").get<int>();
        const int health = data.at("health").get<int>(), hunger = data.at("hunger").get<int>();
        const float saturation = data.at("saturation").get<float>(), exhaustion = data.at("exhaustion").get<float>();
        if (selected < 0 || selected > 8 || health < 0 || health > 20 || hunger < 0 || hunger > 20 ||
            !std::isfinite(saturation) || saturation < 0 || saturation > 20 ||
            !std::isfinite(exhaustion) || exhaustion < 0 || exhaustion > 4) return false;
        const bool flying = data.at("flying").get<bool>();
        const bool inventoryOpen = data.at("inventoryOpen").get<bool>();
        const bool craftTable = data.at("craftTable").get<bool>();
        const auto& bytes = data.at("edits").get_binary();
        EditMap edits;
        if (!DecodeEdits(bytes.data(), bytes.size(), edits)) return false;
        const int dimension = data.value("dimension", 0);
        EditMap otherEdits;
        float otherPos[3] = {8.5f, 90.0f, 8.5f};
        if (data.contains("otherEdits")) {
            const auto& ob = data.at("otherEdits").get_binary();
            DecodeEdits(ob.data(), ob.size(), otherEdits);
        }
        if (data.contains("otherPos")) {
            const auto& op = data.at("otherPos");
            otherPos[0] = op[0].get<float>(); otherPos[1] = op[1].get<float>(); otherPos[2] = op[2].get<float>();
        }
        const bool otherVisited = data.value("otherVisited", false);
        if (dimension == 1) {
            ResetWorld(seed, {}, generator);
            gDimEdits[0] = std::move(otherEdits);
            gDimPos[0][0] = otherPos[0]; gDimPos[0][1] = otherPos[1]; gDimPos[0][2] = otherPos[2];
            gDimVisited[0] = otherVisited;
            BuildDimensionWorld(1, std::move(edits));
            gDimVisited[1] = true;
        } else {
            ResetWorld(seed, std::move(edits), generator);
            gDimEdits[1] = std::move(otherEdits);
            gDimPos[1][0] = otherPos[0]; gDimPos[1][1] = otherPos[1]; gDimPos[1][2] = otherPos[2];
            gDimVisited[1] = otherVisited;
        }
        StartGame();
        gWorldClock.Set(worldTime);
        gWorldName = name;
        gSavePath = path;
        gGameMode = mode;
        gSettings.difficulty = difficulty;
        gAllowCheats = data.value("cheats", false);
        gKeepInventory = data.value("keepInventory", false);
        gWeather = data.value("weather", 0);
        gWeatherTicksLeft = float(data.value("weatherTicks", 0));
        gRuleDaylightCycle = data.value("doDaylightCycle", true);
        gRuleMobSpawning = data.value("doMobSpawning", true);
        gRuleWeatherCycle = data.value("doWeatherCycle", true);
        gRuleMobGriefing = data.value("mobGriefing", true);
        gRuleFireTick = data.value("doFireTick", true);
        gRuleRandomTick = std::clamp(data.value("randomTickSpeed", 3), 0, 20);
        gStructuresEnabled = data.value("structures", true);
        if (gWorld) gWorld->SetGenerateStructures(gStructuresEnabled);
        gInventory = inventory;
        gCraftTable = craftTable;
        if (data.contains("furnace")) {
            const auto& f = data.at("furnace");
            auto rd = [](const nlohmann::json& j) -> ItemSlot {
                ItemSlot s;
                s.id = uint8_t(j[0].get<int>());
                s.count = j[1].get<int>();
                if (!IsValidItem(s.id) || s.count < 0) return {};
                return s;
            };
            gFurnace.input = rd(f.at("input"));
            gFurnace.fuel = rd(f.at("fuel"));
            gFurnace.output = rd(f.at("output"));
            gFurnace.burn = f.value("burn", 0.0f);
            gFurnace.burnTotal = f.value("burnTotal", 0.0f);
            gFurnace.cook = f.value("cook", 0.0f);
            gFurnace.x = f.value("x", 0); gFurnace.y = f.value("y", 0); gFurnace.z = f.value("z", 0);
        }
        if (data.contains("chests")) {
            auto rd = [](const nlohmann::json& j) -> ItemSlot {
                ItemSlot s;
                s.id = uint8_t(j[0].get<int>());
                s.count = j[1].get<int>();
                if (!IsValidItem(s.id) || s.count < 0) return {};
                return s;
            };
            for (const auto& c : data.at("chests")) {
                const uint64_t key = ChestKey(c.value("x", 0), c.value("z", 0));
                ChestState& chest = gChests[key];
                const auto& slots = c.at("slots");
                for (int i = 0; i < 27 && i < int(slots.size()); ++i) chest.slots[i] = rd(slots[i]);
            }
        }
        gResumeScreen = health == 0 ? Screen::Death : (inventoryOpen ? Screen::Inventory : Screen::Game);
        gHotbarSlot = selected;
        if (mode == 0) SyncHotbar();
        else memcpy(gHotbar, hotbar.data(), sizeof(gHotbar));
        gSurvival.health = health;
        gSurvival.hunger = hunger;
        gSurvival.saturation = saturation;
        gSurvival.exhaustion = exhaustion;
        gSurvival.dead = health == 0;
        gBody.x = position[0]; gBody.y = position[1]; gBody.z = position[2];
        gBody.flying = mode == 1 && flying;
        gCamera.yaw = rotation[0]; gCamera.pitch = rotation[1];
        gCamera.x = gBody.x; gCamera.y = gBody.y + PlayerBody::EYE; gCamera.z = gBody.z;
        gSpawned = true;
        gResumePending = true;
        gFallStartY = gBody.y;
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

static void ProfileSkinChanged(const PlayerProfile& previous) {
    if (!gGui.LoadPlayerSkin(gRC, gStore, gProfileDraft)) {
        gProfileDraft = previous;
        gActionBar = "Selected skin could not be loaded from the local Java assets.";
        gActionBarUntil = gNowMs + 4000;
    }
}

static void HandleMenuAction(int action) {
    if (action != ACT_NONE) gSound.Play(SND_CLICK);
    if (action >= ACT_WORLD_ROW && action - ACT_WORLD_ROW < int(gMenu.worlds.size())) {
        const int index = action - ACT_WORLD_ROW;
        const bool doubleClick = gLastWorldClickIndex == index &&
                                 gNowMs - gLastWorldClickMs < 300.0f;
        gLastWorldClickIndex = index;
        gLastWorldClickMs = gNowMs;
        gMenu.selectedWorld = index;
        if (doubleClick) LoadWorldSave(gMenu.worlds[index].path);
        return;
    }
    if (action >= ACT_BIND_BASE && action < ACT_BIND_BASE + BIND_COUNT) {
        gEditingBind = action - ACT_BIND_BASE;
        return;
    }
    if (action >= ACT_RULE_TOGGLE_BASE && action < ACT_RULE_TOGGLE_BASE + RULE_COUNT) {
        gCreate.rules[action - ACT_RULE_TOGGLE_BASE] = !gCreate.rules[action - ACT_RULE_TOGGLE_BASE];
        return;
    }
    switch (action) {
        case ACT_WORLD_EDIT:
            if (gMenu.selectedWorld >= 0 && gMenu.selectedWorld < int(gMenu.worlds.size())) {
                gEditName = gMenu.worlds[gMenu.selectedWorld].name;
                SetScreen(gHwnd, Screen::EditWorld);
            }
            break;
        case ACT_WORLD_DELETE:
            if (gMenu.selectedWorld >= 0 && gMenu.selectedWorld < int(gMenu.worlds.size()))
                SetScreen(gHwnd, Screen::ConfirmDelete);
            break;
        case ACT_CONFIRM_DELETE:
            DeleteSelectedWorld();
            SetScreen(gHwnd, Screen::SelectWorld);
            break;
        case ACT_CANCEL_DELETE:
            SetScreen(gHwnd, Screen::SelectWorld);
            break;
        case ACT_EDIT_RENAME:
            RenameSelectedWorld(gEditName);
            gActionBar = "World renamed.";
            gActionBarUntil = gNowMs + 3000;
            break;
        case ACT_EDIT_FOLDER:
            OpenSelectedWorldFolder();
            break;
        case ACT_EDIT_RESET_ICON:
            ResetSelectedWorldIcon();
            gActionBar = "World icon reset.";
            gActionBarUntil = gNowMs + 3000;
            break;
        case ACT_EDIT_DONE:
            if (gMenu.selectedWorld >= 0 && gMenu.selectedWorld < int(gMenu.worlds.size()) &&
                !gEditName.empty() && gEditName != gMenu.worlds[gMenu.selectedWorld].name)
                RenameSelectedWorld(gEditName);
            SetScreen(gHwnd, Screen::SelectWorld);
            break;
        case ACT_EDIT_CANCEL:
            SetScreen(gHwnd, Screen::SelectWorld);
            break;
        case ACT_WORLD_RECREATE:
            RecreateSelectedWorld();
            break;
        case ACT_GAMERULES:
            SetScreen(gHwnd, Screen::GameRules);
            break;
        case ACT_GAMERULES_DONE:
            SetScreen(gHwnd, Screen::CreateWorld);
            break;
        case ACT_TOGGLE_STRUCTURES:
            gCreate.structures = !gCreate.structures;
            break;
        case ACT_TOGGLE_BONUS:
            gCreate.bonusChest = !gCreate.bonusChest;
            break;
        case ACT_CYCLE_RULE_RANDOMTICK:
            gCreate.randomTickSpeed = (gCreate.randomTickSpeed + 1) % 11;
            break;
        case ACT_DONE_VIDEO:
        case ACT_DONE_CONTROLS:
            SavePreferences();
            SetScreen(gHwnd, Screen::Options);
            break;
        case ACT_TOGGLE_FANCY:
            gSettings.fancyGraphics = !gSettings.fancyGraphics;
            if (!gSettings.fancyGraphics) gSettings.smoothLighting = false;
            break;
        case ACT_TOGGLE_SMOOTH:
            gSettings.smoothLighting = !gSettings.smoothLighting;
            break;
        case ACT_CYCLE_FPS: {
            static const int FPS[] = {30, 60, 120, 144, 240, 0};
            int idx = 2;
            for (int i = 0; i < 6; ++i) if (FPS[i] == gSettings.maxFramerate) idx = i;
            gSettings.maxFramerate = FPS[(idx + 1) % 6];
            break;
        }
        case ACT_CYCLE_GUISCALE:
            gSettings.guiScale = (gSettings.guiScale + 1) % 5;
            break;
        case ACT_CYCLE_CLOUDS:
            gSettings.clouds = (gSettings.clouds + 1) % 3;
            break;
        case ACT_CYCLE_PARTICLES:
            gSettings.particles = (gSettings.particles + 1) % 3;
            break;
        case ACT_TOGGLE_SHADOWS:
            gSettings.entityShadows = !gSettings.entityShadows;
            break;
        case ACT_TOGGLE_INVERT:
            gSettings.invertMouse = !gSettings.invertMouse;
            break;
        case ACT_SINGLEPLAYER:
            RefreshWorldList();
            SetScreen(gHwnd, Screen::SelectWorld);
            break;
        case ACT_NEW_WORLD:
            gCreate = WorldCreateInfo{};
            gCreate.difficulty = gSettings.difficulty;
            SetScreen(gHwnd, Screen::CreateWorld);
            break;
        case ACT_WORLD_PREV:
            if (gMenu.worldPage > 0) --gMenu.worldPage;
            break;
        case ACT_WORLD_NEXT:
            if ((gMenu.worldPage + 1) * 5 < int(gMenu.worlds.size())) ++gMenu.worldPage;
            break;
        case ACT_LOAD_WORLD:
            if (gMenu.selectedWorld >= 0 && gMenu.selectedWorld < int(gMenu.worlds.size()) &&
                !LoadWorldSave(gMenu.worlds[gMenu.selectedWorld].path)) {
                gActionBar = "Could not load world: damaged or unsupported save.";
                gActionBarUntil = gNowMs + 5000;
            }
            break;
        case ACT_RESPAWN:
            StartGame();
            break;
        case ACT_CREATE_WORLD: {
            if (gCreate.name.empty()) break;
            const std::string path = NewSavePath();
            if (path.empty()) {
                gActionBar = "Could not create the world save folder.";
                gActionBarUntil = gNowMs + 5000;
                break;
            }
            gStructuresEnabled = gCreate.structures;
            gRuleDaylightCycle = gCreate.rules[RULE_DAYLIGHT];
            gRuleMobSpawning = gCreate.rules[RULE_MOBSPAWN];
            gRuleWeatherCycle = gCreate.rules[RULE_WEATHER];
            gRuleMobGriefing = gCreate.rules[RULE_MOBGRIEF];
            gRuleFireTick = gCreate.rules[RULE_FIRETICK];
            gRuleRandomTick = gCreate.randomTickSpeed;
            ResetWorld(ParseSeed(gCreate.seed), {}, gCreate.generator);
            gSavePath = path;
            gWorldName = gCreate.name;
            gGameMode = gCreate.gameMode & 1;
            gSettings.difficulty = gCreate.difficulty;
            gAllowCheats = gCreate.allowCheats;
            gKeepInventory = gCreate.keepInventory || gCreate.rules[RULE_KEEPINV];
            gBonusChestPending = gCreate.bonusChest;
            gInventory = InventoryState{};
            gCraftTable = false;
            gInventoryMessage.clear();
            gHotbarSlot = 0;
            if (gGameMode == 1) memcpy(gHotbar, HOTBAR_BLOCKS, sizeof(gHotbar));
            else memset(gHotbar, 0, sizeof(gHotbar));
            StartGame();
            break;
        }
        case ACT_CANCEL_CREATE:
            if (gScreen == Screen::CreateWorld) {
                RefreshWorldList();
                SetScreen(gHwnd, Screen::SelectWorld);
            } else SetScreen(gHwnd, Screen::Title);
            break;
        case ACT_CYCLE_WORLD_TYPE: {
            static const WorldGenerator ORDER[] = {
                WorldGenerator::Climate, WorldGenerator::Flat,
                WorldGenerator::LargeBiomes, WorldGenerator::Amplified};
            int idx = 0;
            for (int i = 0; i < 4; ++i) if (ORDER[i] == gCreate.generator) idx = i;
            gCreate.generator = ORDER[(idx + 1) % 4];
            break;
        }
        case ACT_CYCLE_GAMEMODE:
            gCreate.gameMode ^= 1;
            break;
        case ACT_TOGGLE_CHEATS:
            gCreate.allowCheats = !gCreate.allowCheats;
            break;
        case ACT_TOGGLE_KEEPINV:
            gCreate.keepInventory = !gCreate.keepInventory;
            break;
        case ACT_CREATE_TAB_GAME:
            gCreate.tab = 0; gCreate.focus = 1;
            break;
        case ACT_CREATE_TAB_WORLD:
            gCreate.tab = 1; gCreate.focus = 2;
            break;
        case ACT_CREATE_TAB_MORE:
            gCreate.tab = 2; gCreate.focus = 0;
            break;
        case ACT_RESUME:
            SetScreen(gHwnd, Screen::Game);
            break;
        case ACT_OPTIONS:
            gOptionsReturn = gScreen;
            SetScreen(gHwnd, Screen::Options);
            break;
        case ACT_OPTIONS_DONE:
            if (!SavePreferences()) {
                gActionBar = "Could not save preferences.";
                gActionBarUntil = gNowMs + 4000;
            }
            SetScreen(gHwnd, gScreen == Screen::Options ? gOptionsReturn : Screen::Options);
            break;
        case ACT_VIDEO_OPTIONS:
            SetScreen(gHwnd, Screen::VideoOptions);
            break;
        case ACT_SOUND_OPTIONS:
            SetScreen(gHwnd, Screen::SoundOptions);
            break;
        case ACT_CONTROLS_OPTIONS:
            SetScreen(gHwnd, Screen::ControlsOptions);
            break;
        case ACT_PROFILE:
            gProfileReturn = gScreen;
            gProfileDraft = gProfile;
            gProfileNameSelected = false;
            SetScreen(gHwnd, Screen::Profile);
            break;
        case ACT_PROFILE_DONE: {
            if (!IsValidPlayerName(gProfileDraft.name)) break;
            const PlayerProfile previous = gProfile;
            gProfile = gProfileDraft;
            if (!SavePreferences()) {
                gProfile = previous;
                gActionBar = "Could not save the local profile.";
                gActionBarUntil = gNowMs + 4000;
                break;
            }
            SetScreen(gHwnd, gProfileReturn);
            break;
        }
        case ACT_PROFILE_CANCEL:
            gProfileDraft = gProfile;
            gGui.LoadPlayerSkin(gRC, gStore, gProfile);
            SetScreen(gHwnd, gProfileReturn);
            break;
        case ACT_SKIN_PREV:
        case ACT_SKIN_NEXT:
        case ACT_SKIN_MODEL: {
            const PlayerProfile previous = gProfileDraft;
            if (action == ACT_SKIN_MODEL) gProfileDraft.slim = !gProfileDraft.slim;
            else gProfileDraft.skin = (gProfileDraft.skin + (action == ACT_SKIN_NEXT ? 1 : 8)) % 9;
            ProfileSkinChanged(previous);
            break;
        }
        case ACT_QUIT:
            gRunning = false;
            break;
        case ACT_QUIT_TO_TITLE:
            if (!SaveCurrentWorld()) {
                gActionBar = "World save failed. Your world is still open; please retry.";
                gActionBarUntil = gNowMs + 5000;
                break;
            }
            gSavePath.clear();
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
            if (gScreen == Screen::CreateWorld) gCreate.difficulty = (gCreate.difficulty + 1) & 3;
            else gSettings.difficulty = (gSettings.difficulty + 1) & 3;
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
        static int logged = 0;
        if (logged < 40) {
            ++logged;
            int qy = 0, qx = 0, qz = 0;
            for (size_t q = 0; q + 3 < md.vertices.size(); q += 4) {
                const MeshVertex& a = md.vertices[q];
                const MeshVertex& b = md.vertices[q + 1];
                const MeshVertex& c = md.vertices[q + 2];
                if (a.y == b.y && b.y == c.y) ++qy;
                else if (a.x == b.x && b.x == c.x) ++qx;
                else ++qz;
            }
            FILE* f = fopen("meshlog.txt", "a");
            if (f) {
                fprintf(f, "chunk %d %d: %zu verts %u opaque %u liquid Y=%d X=%d Z=%d\n",
                        KeyCX(key), KeyCZ(key), md.vertices.size(), gc.opaqueIndexCount,
                        gc.indexCount - gc.opaqueIndexCount, qy, qx, qz);
                if (logged <= 6) {
                    for (size_t q = 0; q + 3 < md.vertices.size() && q < 16; q += 4) {
                        const MeshVertex& v0 = md.vertices[q];
                        fprintf(f, "  quad %zu: (%.1f,%.1f,%.1f)(%.1f,%.1f,%.1f)(%.1f,%.1f,%.1f)(%.1f,%.1f,%.1f) tile %.3f,%.3f light %.0f\n",
                                q / 4,
                                double(md.vertices[q].x), double(md.vertices[q].y), double(md.vertices[q].z),
                                double(md.vertices[q+1].x), double(md.vertices[q+1].y), double(md.vertices[q+1].z),
                                double(md.vertices[q+2].x), double(md.vertices[q+2].y), double(md.vertices[q+2].z),
                                double(md.vertices[q+3].x), double(md.vertices[q+3].y), double(md.vertices[q+3].z),
                                double(v0.tu), double(v0.tv), double(v0.light));
                    }
                }
                fclose(f);
            }
        }
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

static void SaveTestState(const char* screenshot) {
    if (!gAutomation) return;
    auto slotJson = [](const ItemSlot& slot) {
        return nlohmann::json{{"item", slot.Empty() ? "air" : ItemName(slot.id)},
                              {"count", slot.count}, {"damage", slot.damage}};
    };
    nlohmann::json state = {
        {"screen", int(gScreen)}, {"table", gCraftTable}, {"selected", gHotbarSlot},
        {"cursor", slotJson(gInventory.cursor)}, {"result", slotJson(gInventory.Result(gCraftTable))},
        {"position", {gBody.x, gBody.y, gBody.z}}, {"yaw", gCamera.yaw}, {"pitch", gCamera.pitch},
        {"draftDifficulty", gCreate.difficulty}, {"difficulty", gSettings.difficulty},
        {"tab", gCreate.tab}, {"seed", gCreate.seed}, {"profileName", gProfile.name},
        {"draftName", gProfileDraft.name}, {"skin", gProfileDraft.skin}, {"slim", gProfileDraft.slim},
        {"fov", gSettings.fov}, {"renderDistance", gSettings.renderDistance},
        {"volume", gSettings.soundVolume}, {"sensitivity", gSettings.sensitivity},
        {"assetVersion", gAssetVersion}, {"skinLoaded", gGui.texSkin.srv != nullptr},
        {"worldName", gWorldName}, {"worldSeed", gWorldSeed}, {"mode", gGameMode}, {"worldCount", gMenu.worlds.size()},
        {"worldPage", gMenu.worldPage}, {"selectedWorld", gMenu.selectedWorld}, {"resumePending", gResumePending},
        {"health", gSurvival.health}, {"hunger", gSurvival.hunger},
        {"worldTime", gWorldClock.Ticks()}, {"daylight", gSkyTimeline.Sample(gWorldClock.Ticks()).daylight},
        {"sunAngle", gSkyTimeline.Sample(gWorldClock.Ticks()).sunAngle},
        {"stars", gSkyTimeline.Sample(gWorldClock.Ticks()).stars},
        {"moonPhase", gSkyTimeline.Sample(gWorldClock.Ticks()).moonPhase},
        {"timelineLoaded", gSkyTimeline.Loaded()}, {"sunLoaded", gRC.sunSRV != nullptr},
        {"moonLoaded", gRC.moonSRV[gSkyTimeline.Sample(gWorldClock.Ticks()).moonPhase] != nullptr},
        {"fixtureBlock", gWorld ? gWorld->GetBlock(8, 64, 6) : 0},
        {"fixtureAbove", gWorld ? gWorld->GetBlock(8, 65, 6) : 0}, {"actionBar", gActionBar},
        {"generator", gWorld ? GeneratorName(gWorld->Generator()) : "none"},
        {"surfaceHeight", gWorld ? gWorld->SurfaceY(int(floorf(gBody.x)), int(floorf(gBody.z))) : -1},
        {"draftGenerator", GeneratorName(gCreate.generator)},
        {"miningProgress", gMiningProgress}, {"brokenBlocks", gBrokenBlocks},
        {"targetBlock", gHaveTarget ? int(gWorld->GetBlock(gTargetX, gTargetY, gTargetZ)) : 0},
        {"soundsPlayed", gSound.Played()}, {"stoneSounds", gSound.Variants(SND_STONE)},
        {"cracksLoaded", gGui.texDestroy[0].srv != nullptr},
        {"toolTexturesLoaded", gGui.texItemIcon[ITEM_WOOD_PICKAXE].srv != nullptr},
        {"zombieTextureLoaded", gGui.texMob[int(MobType::Zombie)].srv != nullptr},
        {"mobTexturesLoaded", [&] { int n = 0; for (const GuiTexture& t : gGui.texMob) n += t.srv != nullptr; return n; }()},
        {"zombieBufferLoaded", gZombieVB != nullptr},
        {"combatHits", gCombatHits}, {"criticalHits", gCriticalHits}, {"zombieKills", gZombieKills},
        {"attackStrength", gCooldown.Strength(gNowMs)}, {"onGround", gBody.onGround}, {"verticalSpeed", gBody.vy}
    };
    state["zombies"] = nlohmann::json::array();
    for (const Zombie& zombie : gZombies)
        state["zombies"].push_back({{"type", MobId(zombie.type)}, {"health", zombie.health},
            {"position", {zombie.body.x, zombie.body.y, zombie.body.z}}, {"hurtTime", zombie.resistance.remaining}});
    state["slots"] = nlohmann::json::array();
    for (const auto& slot : gInventory.slots) state["slots"].push_back(slotJson(slot));
    state["craft"] = nlohmann::json::array();
    for (const auto& slot : gInventory.craft) state["craft"].push_back(slotJson(slot));
    std::ofstream file(std::string(screenshot) + ".json", std::ios::binary);
    file << state.dump(2);
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
            snprintf(path, sizeof(path), "screenshots/shot_%llu_%03d.bmp",
                     static_cast<unsigned long long>(GetTickCount64()), shotIdx++);
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
                const bool bgra = bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                                  bd.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
                for (uint32_t y = h; y-- > 0;) {
                    const uint8_t* src = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch;
                    for (uint32_t x = 0; x < w; ++x) {
                        row[x * 3 + 0] = src[x * 4 + (bgra ? 0 : 2)];
                        row[x * 3 + 1] = src[x * 4 + 1];
                        row[x * 3 + 2] = src[x * 4 + (bgra ? 2 : 0)];
                    }
                    fwrite(row.data(), 1, rowBytes, f);
                }
                fclose(f);
                SaveTestState(path);
                gActionBar = "Saved screenshot";
                gActionBarUntil = gNowMs + 2000;
            }
            stage->Unmap(0);
        }
        stage->Release();
    }
    back->Release();
}

static void DumpAtlas() {
    FILE* f = fopen("atlas.bmp", "wb");
    if (!f) return;
    const uint32_t w = ATLAS_SIZE, h = ATLAS_SIZE;
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
    const uint8_t* px = gAtlas.Pixels();
    std::vector<uint8_t> row(rowBytes);
    for (uint32_t y = h; y-- > 0;) {
        for (uint32_t x = 0; x < w; ++x) {
            row[x * 3 + 0] = px[(size_t(y) * w + x) * 4 + 2];
            row[x * 3 + 1] = px[(size_t(y) * w + x) * 4 + 1];
            row[x * 3 + 2] = px[(size_t(y) * w + x) * 4 + 0];
        }
        fwrite(row.data(), 1, rowBytes, f);
    }
    fclose(f);
}

static void SyncHotbar() {
    for (int i = 0; i < 9; ++i) {
        const ItemSlot& slot = gInventory.slots[27 + i];
        gHotbar[i] = slot.Empty() ? uint8_t(BLOCK_AIR) : slot.id;
    }
}

static void CloseCreativeInventory() {
    // Creative picker stacks are unlimited, so a held cursor stack is simply
    // discarded instead of blocking the close like the survival bag.
    gInventory.cursor = ItemSlot{};
    gInventoryMessage.clear();
    SyncHotbar();
    SetScreen(gHwnd, Screen::Game);
}

static void CloseInventory() {
    if (!gInventory.Close()) {
        gInventoryMessage = "Make room for the cursor and crafting items before closing.";
        return;
    }
    gInventoryMessage.clear();
    SyncHotbar();
    gCraftTable = false;
    SetScreen(gHwnd, Screen::Game);
}

static void PlayerDied() {
    gSurvival.dead = true;
    if (!gKeepInventory) {
        gInventory = InventoryState{};
        gCraftTable = false;
        gInventoryMessage.clear();
        if (gGameMode == 0) SyncHotbar();
    }
    SetScreen(gHwnd, Screen::Death);
}

static void HandleInventoryClick(int index, bool right) {
    const bool dbl = !right && !gInput.keys[VK_SHIFT] && index >= 0 &&
                     index == gLastInvClickIndex && (gTimeSec - gLastInvClickMs) < 0.35f;
    if (index >= 0) { gLastInvClickIndex = index; gLastInvClickMs = gTimeSec; }
    const bool changed = dbl ? gInventory.DoubleClickCollect(index, gCraftTable)
                             : gInventory.Click(index, right, gCraftTable, gInput.keys[VK_SHIFT]);
    if (changed) {
        gInventoryMessage.clear();
        if (gGameMode == 0) SyncHotbar();
    }
}

static uint8_t SmeltResult(uint8_t id) {
    switch (id) {
        case BLOCK_IRON_ORE: case ITEM_RAW_IRON: return ITEM_IRON_INGOT;
        case BLOCK_GOLD_ORE: case ITEM_RAW_GOLD: return ITEM_GOLD_INGOT;
        case BLOCK_SAND: return BLOCK_GLASS;
        case BLOCK_COBBLE: return BLOCK_STONE;
        case BLOCK_CLAY: return BLOCK_BRICK;
        default: return 0;
    }
}

static float FuelSeconds(uint8_t id) {
    switch (id) {
        case ITEM_COAL: return 80.0f;
        case ITEM_STICK: return 5.0f;
        case BLOCK_PLANKS: case BLOCK_SPRUCE_PLANKS: case BLOCK_BIRCH_PLANKS:
        case BLOCK_LOG: case BLOCK_SPRUCE_LOG: case BLOCK_BIRCH_LOG:
        case BLOCK_CRAFTING_TABLE: case BLOCK_BOOKSHELF: return 15.0f;
        default: return 0.0f;
    }
}

static void TickFurnace(float dt) {
    const uint8_t result = SmeltResult(gFurnace.input.id);
    const bool canCook = result != 0 &&
        (gFurnace.output.Empty() ||
         (gFurnace.output.id == result && gFurnace.output.count < ItemStackLimit(result)));
    if (gFurnace.burn > 0) {
        gFurnace.burn -= dt;
        if (gFurnace.burn < 0) gFurnace.burn = 0;
    }
    if (gFurnace.burn == 0 && canCook && FuelSeconds(gFurnace.fuel.id) > 0) {
        gFurnace.burnTotal = FuelSeconds(gFurnace.fuel.id);
        gFurnace.burn = gFurnace.burnTotal;
        if (--gFurnace.fuel.count <= 0) gFurnace.fuel = {};
    }
    if (gFurnace.burn > 0 && canCook) {
        gFurnace.cook += dt;
        if (gFurnace.cook >= 10.0f) {
            gFurnace.cook = 0;
            if (--gFurnace.input.count <= 0) gFurnace.input = {};
            if (gFurnace.output.Empty()) gFurnace.output = {result, 1};
            else ++gFurnace.output.count;
            gSound.PlayAt(SND_CLICK, float(gFurnace.x) + 0.5f, float(gFurnace.y) + 0.5f,
                          float(gFurnace.z) + 0.5f, 0.5f, 1.2f);
        }
    } else if (gFurnace.cook > 0) {
        gFurnace.cook -= dt * 2;
        if (gFurnace.cook < 0) gFurnace.cook = 0;
    }
}

static void CloseFurnace() {
    if (!gInventory.Close()) {
        gInventoryMessage = "Make room for the cursor items before closing.";
        return;
    }
    gInventoryMessage.clear();
    SyncHotbar();
    SetScreen(gHwnd, Screen::Game);
}

static void HandleFurnaceClick(int index, bool right) {
    if (index < 0) return;
    ItemSlot& cursor = gInventory.cursor;
    auto takeOutput = [&]() {
        if (gFurnace.output.Empty()) return;
        if (cursor.Empty()) { cursor = gFurnace.output; gFurnace.output = {}; }
        else if (cursor.id == gFurnace.output.id &&
                 cursor.count + gFurnace.output.count <= ItemStackLimit(cursor.id)) {
            cursor.count += gFurnace.output.count;
            gFurnace.output = {};
        }
    };
    if (index == 102) {
        if (gInput.keys[VK_SHIFT]) {
            const int left = gInventory.Add(gFurnace.output.id, gFurnace.output.count);
            if (left == 0) gFurnace.output = {};
            else gFurnace.output.count = left;
        } else takeOutput();
        SyncHotbar();
        return;
    }
    ItemSlot& slot = index == 100 ? gFurnace.input : index == 101 ? gFurnace.fuel : gInventory.slots[index];
    if (gInput.keys[VK_SHIFT]) {
        if (slot.Empty()) return;
        if (index >= 100) {
            const int left = gInventory.Add(slot.id, slot.count);
            if (left == 0) slot = {};
            else slot.count = left;
        } else {
            const int target = FuelSeconds(slot.id) > 0 ? 101 : 100;
            ItemSlot& dst = target == 101 ? gFurnace.fuel : gFurnace.input;
            if (dst.Empty()) { dst = slot; slot = {}; }
            else if (dst.id == slot.id && dst.count < ItemStackLimit(dst.id)) {
                const int move = std::min(slot.count, ItemStackLimit(dst.id) - dst.count);
                dst.count += move; slot.count -= move;
                if (slot.count <= 0) slot = {};
            }
        }
        SyncHotbar();
        return;
    }
    if (right) {
        if (cursor.Empty() && !slot.Empty()) {
            const int half = (slot.count + 1) / 2;
            cursor = {slot.id, half};
            slot.count -= half;
            if (slot.count <= 0) slot = {};
        } else if (!cursor.Empty() && (slot.Empty() || slot.id == cursor.id) &&
                   slot.count < ItemStackLimit(cursor.id)) {
            if (slot.Empty()) slot = {cursor.id, 0};
            ++slot.count;
            if (--cursor.count <= 0) cursor = {};
        }
    } else {
        if (cursor.Empty()) { cursor = slot; slot = {}; }
        else if (slot.Empty()) { slot = cursor; cursor = {}; }
        else if (slot.id == cursor.id && slot.count < ItemStackLimit(slot.id)) {
            const int move = std::min(cursor.count, ItemStackLimit(slot.id) - slot.count);
            slot.count += move; cursor.count -= move;
            if (cursor.count <= 0) cursor = {};
        } else std::swap(cursor, slot);
    }
    SyncHotbar();
}

static void CloseChest() {
    if (!gInventory.Close()) {
        gInventoryMessage = "Make room for the cursor items before closing.";
        return;
    }
    gInventoryMessage.clear();
    SyncHotbar();
    SetScreen(gHwnd, Screen::Game);
}

static void HandleChestClick(int index, bool right) {
    if (index < 0) return;
    ItemSlot& cursor = gInventory.cursor;
    ChestState& chest = gChests[gChestKey];
    ItemSlot& slot = index >= 100 ? chest.slots[index - 100] : gInventory.slots[index];
    if (gInput.keys[VK_SHIFT]) {
        if (slot.Empty()) return;
        if (index >= 100) {
            const int left = gInventory.Add(slot.id, slot.count);
            if (left == 0) slot = {};
            else slot.count = left;
        } else {
            int target = -1;
            for (int i = 0; i < 27; ++i) {
                ItemSlot& dst = chest.slots[i];
                if (dst.id == slot.id && dst.count < ItemStackLimit(dst.id)) { target = i; break; }
            }
            if (target < 0) for (int i = 0; i < 27; ++i) if (chest.slots[i].Empty()) { target = i; break; }
            if (target >= 0) {
                ItemSlot& dst = chest.slots[target];
                if (dst.Empty()) { dst = slot; slot = {}; }
                else {
                    const int move = std::min(slot.count, ItemStackLimit(dst.id) - dst.count);
                    dst.count += move; slot.count -= move;
                    if (slot.count <= 0) slot = {};
                }
            }
        }
        SyncHotbar();
        return;
    }
    if (right) {
        if (cursor.Empty() && !slot.Empty()) {
            const int half = (slot.count + 1) / 2;
            cursor = {slot.id, half};
            slot.count -= half;
            if (slot.count <= 0) slot = {};
        } else if (!cursor.Empty() && (slot.Empty() || slot.id == cursor.id) &&
                   slot.count < ItemStackLimit(cursor.id)) {
            if (slot.Empty()) slot = {cursor.id, 0};
            ++slot.count;
            if (--cursor.count <= 0) cursor = {};
        }
    } else {
        if (cursor.Empty()) { cursor = slot; slot = {}; }
        else if (slot.Empty()) { slot = cursor; cursor = {}; }
        else if (slot.id == cursor.id && slot.count < ItemStackLimit(slot.id)) {
            const int move = std::min(cursor.count, ItemStackLimit(slot.id) - slot.count);
            slot.count += move; cursor.count -= move;
            if (cursor.count <= 0) cursor = {};
        } else std::swap(cursor, slot);
    }
    SyncHotbar();
}

static void BuildDimensionWorld(int dim, EditMap edits) {
    for (auto& kv : gGpu) {
        SafeRelease(kv.second.vb);
        SafeRelease(kv.second.ib);
    }
    gGpu.clear();
    gSweepCursor = 0;
    delete gWorld;
    gDimension = dim;
    const WorldGenerator gen = dim == 1 ? WorldGenerator::Nether : gOverworldGen;
    gWorld = new World(gWorldSeed, gSettings.renderDistance, gen);
    gWorld->SetBlockTiles(gTiles);
    gWorld->SetEdits(std::move(edits));
    gZombies.clear();
    gMobSpawnTimer = 0;
}

static void EnterDimension(int dim) {
    if (dim == gDimension || !gWorld) return;
    gDimEdits[gDimension] = gWorld->SnapshotEdits();
    gDimPos[gDimension][0] = gBody.x;
    gDimPos[gDimension][1] = gBody.y;
    gDimPos[gDimension][2] = gBody.z;
    gDimVisited[gDimension] = true;
    float tx, tz;
    if (gDimVisited[dim]) {
        tx = gDimPos[dim][0]; tz = gDimPos[dim][2];
    } else {
        const float scale = dim == 1 ? 0.125f : 8.0f;
        tx = gBody.x * scale; tz = gBody.z * scale;
    }
    BuildDimensionWorld(dim, gDimEdits[dim]);
    gBody.x = tx; gBody.z = tz;
    gBody.y = dim == 1 ? 100.0f : 120.0f;
    gBody.vx = gBody.vy = gBody.vz = 0;
    gBody.onGround = false;
    gFallStartY = gBody.y;
    gPortalCooldown = 3.0f; // grace period: no fall damage while settling
    gWorld->Update(gBody.x, gBody.z);
    gSound.Play(SND_CLICK);
}

static void LightPortal(int hx, int hy, int hz, int nx, int nz) {
    const int ax = hx + nx, az = hz + nz;
    const int tx = nz, tz = nx; // horizontal tangent to the clicked face
    for (int w = 0; w < 2; ++w)
        for (int h = 0; h < 3; ++h) {
            const int px = ax + tx * w, py = hy + h, pz = az + tz * w;
            if (py >= 0 && py < CHUNK_Y && gWorld->GetBlock(px, py, pz) == BLOCK_AIR)
                gWorld->SetBlock(px, py, pz, BLOCK_PORTAL);
        }
    gSound.PlayAt(SND_CLICK, float(hx), float(hy), float(hz), 0.8f, 0.7f);
}

// ---- Chat and slash commands ---------------------------------------------

class GameCommandHost : public CommandHost {
public:
    int GameMode() const override { return gGameMode; }
    void SetGameMode(int mode) override {
        if (mode == gGameMode) return;
        if (mode == 0) {
            // Java keeps the items on a mode switch; the creative hotbar only
            // stores ids, so rebuild real stacks for survival.
            for (int i = 0; i < 9; ++i)
                gInventory.slots[27 + i] = {gHotbar[i], gHotbar[i] == BLOCK_AIR ? 0 : ItemStackLimit(gHotbar[i])};
            SyncHotbar();
        } else {
            for (int i = 0; i < 9; ++i)
                gHotbar[i] = gInventory.slots[27 + i].Empty() ? BLOCK_AIR : gInventory.slots[27 + i].id;
        }
        gGameMode = mode;
    }
    void PlayerPos(float& x, float& y, float& z) const override { x = gBody.x; y = gBody.y; z = gBody.z; }
    void Teleport(float x, float y, float z) override {
        gBody.x = x; gBody.y = y; gBody.z = z;
        gBody.vx = gBody.vy = gBody.vz = 0;
        gFallStartY = y;
        if (gWorld) gWorld->Update(x, z);
    }
    uint64_t TimeTicks() const override { return gWorldClock.Ticks(); }
    void SetTimeTicks(uint64_t ticks) override { gWorldClock.Set(ticks); }
    uint64_t WorldSeed() const override { return gWorldSeed; }
    bool LocateStructure(const std::string& structure, int& x, int& z, int& distance) override {
        StructureType type;
        if (!ParseStructure(structure, type)) return false;
        return FindStructure(gWorldSeed, int(floorf(gBody.x)), int(floorf(gBody.z)), type,
                             x, z, distance);
    }
    void SetWeather(int weather, int durationTicks) override {
        gWeather = weather;
        gWeatherTicksLeft = float(durationTicks > 0 ? durationTicks
                                                    : (weather == 2 ? 3600 : (weather == 1 ? 12000 : 0)));
        gWeatherTimer = gWeatherTicksLeft > 0 ? gWeatherTicksLeft : 2400.0f;
    }
    int Weather() const override { return gWeather; }
    bool GiveItem(const std::string& item, int count, std::string& givenName) override {
        const uint8_t id = ItemIdFromResourceName(item);
        if (id == BLOCK_AIR) return false;
        givenName = id == BLOCK_GRASS ? "Grass Block" : std::string();
        if (givenName.empty()) {
            std::string name = ResourceNameForItem(id);
            const size_t colon = name.find(':');
            if (colon != std::string::npos) name = name.substr(colon + 1);
            for (char c : name) {
                if (c == '_') givenName += ' ';
                else if (givenName.empty() || givenName.back() == ' ')
                    givenName += (c >= 'a' && c <= 'z') ? char(c - 32) : c;
                else givenName += c;
            }
        }
        gInventory.Add(id, count);
        SyncHotbar();
        return true;
    }
    bool LocateBiome(const std::string& biome, int& x, int& z, int& distance) override {
        Biome want = Biome::Plains;
        if (biome == "plains") want = Biome::Plains;
        else if (biome == "forest") want = Biome::Forest;
        else if (biome == "birch_forest") want = Biome::BirchForest;
        else if (biome == "taiga") want = Biome::Taiga;
        else if (biome == "desert") want = Biome::Desert;
        else if (biome == "snowy_plains") want = Biome::Snowy;
        else if (biome == "mountains") want = Biome::Mountains;
        else if (biome == "river") want = Biome::River;
        else if (biome == "ocean") want = Biome::Ocean;
        else if (biome == "cherry_grove") want = Biome::CherryGrove;
        else return false;
        const PerlinNoise noise(gWorldSeed);
        const int px = int(floorf(gBody.x)), pz = int(floorf(gBody.z));
        for (int radius = 0; radius <= 512; radius += 12) {
            for (int step = 0; step < 24; ++step) {
                const float angle = float(step) * 0.2617994f;
                const int sx = px + int(cosf(angle) * float(radius));
                const int sz = pz + int(sinf(angle) * float(radius));
                if (SampleTerrain(noise, sx, sz).biome != want) continue;
                x = sx; z = sz;
                distance = int(sqrtf(float((sx - px) * (sx - px) + (sz - pz) * (sz - pz))));
                return true;
            }
        }
        return false;
    }
    int Summon(const std::string& entity, int count, std::string& spawnedName) override {
        MobType type;
        if (!ParseMobId(entity, type)) return -1;
        spawnedName = MobName(type);
        for (int i = 0; i < count; ++i) {
            Mob mob;
            mob.SetType(type);
            mob.body.x = gBody.x + float(i) * 0.9f;
            mob.body.y = gBody.y;
            mob.body.z = gBody.z + 0.5f;
            gZombies.push_back(mob);
        }
        return count;
    }
    int KillEntities(const std::string& target) override {
        if (target == "player" || target == "@s") {
            if (gSurvival.health <= 0) return 0;
            gSurvival.health = 0;
            return 1;
        }
        MobType type;
        const bool all = target == "@e";
        if (!all && !ParseMobId(target, type)) return -1;
        int killed = 0;
        for (Zombie& zombie : gZombies)
            if (zombie.health > 0 && (all || zombie.type == type)) { zombie.health = 0; ++killed; }
        return killed;
    }
    int ClearInventory() override {
        int removed = 0;
        for (ItemSlot& slot : gInventory.slots) if (!slot.Empty()) { slot = {}; ++removed; }
        for (ItemSlot& slot : gInventory.armor) if (!slot.Empty()) { slot = {}; ++removed; }
        if (!gInventory.offhand.Empty()) { gInventory.offhand = {}; ++removed; }
        if (!gInventory.cursor.Empty()) { gInventory.cursor = {}; ++removed; }
        SyncHotbar();
        return removed;
    }
    bool GetGameRule(const std::string& rule, std::string& value) const override {
        bool current = false;
        if (rule == "keepInventory") current = gKeepInventory;
        else if (rule == "doDaylightCycle") current = gRuleDaylightCycle;
        else if (rule == "doMobSpawning") current = gRuleMobSpawning;
        else if (rule == "doWeatherCycle") current = gRuleWeatherCycle;
        else return false;
        value = current ? "true" : "false";
        return true;
    }
    bool SetGameRule(const std::string& rule, const std::string& value) override {
        const bool enabled = value == "true";
        if (rule == "keepInventory") gKeepInventory = enabled;
        else if (rule == "doDaylightCycle") gRuleDaylightCycle = enabled;
        else if (rule == "doMobSpawning") gRuleMobSpawning = enabled;
        else if (rule == "doWeatherCycle") gRuleWeatherCycle = enabled;
        else return false;
        return true;
    }
    std::string PlayerName() const override { return gProfile.name; }
};
static GameCommandHost gCommandHost;

static void PushChat(const std::string& text, bool error) {
    gChat.entries.push_back({text, error, 0.0f});
    if (gChat.entries.size() > 100) gChat.entries.erase(gChat.entries.begin());
}

static void OpenChat(bool slash) {
    gChat.open = true;
    gChat.input = slash ? "/" : "";
    gChatHistory.Reset();
    UpdateCursorCapture(gHwnd);
}

static void CloseChat() {
    gChat.open = false;
    gChat.input.clear();
    UpdateCursorCapture(gHwnd);
}

static void SubmitChat() {
    const std::string line = gChat.input;
    CloseChat();
    if (line.empty()) return;
    gChatHistory.Push(line);
    if (line[0] == '/') {
        const CommandResult result = ExecuteCommand(line, gCommandHost);
        for (const std::string& text : result.lines) PushChat(text, result.error);
    } else {
        PushChat("<" + gProfile.name + "> " + line, false);
    }
}

static void CompleteChat() {
    const std::vector<std::string> options = CommandCompletions(gChat.input);
    if (options.empty()) return;
    gChat.input = CommonPrefix(options);
    if (options.size() == 1) {
        gChat.input += " ";
    } else {
        std::string line = "Options:";
        for (size_t i = 0; i < options.size() && i < 8; ++i) line += " " + options[i];
        PushChat(line, false);
    }
}

static void HandleChatInput() {
    for (char c : gInput.typed) {
        if (c == 13) { SubmitChat(); return; }
        if (c == 8) { if (!gChat.input.empty()) gChat.input.pop_back(); }
        else if (c >= 32 && c < 127 && gChat.input.size() < 200) gChat.input += c;
    }
    if (gInput.KeyPressed(VK_TAB)) CompleteChat();
    if (gInput.KeyPressed(VK_UP)) {
        std::string line;
        if (gChatHistory.Prev(line)) gChat.input = line;
    }
    if (gInput.KeyPressed(VK_DOWN)) {
        std::string line;
        if (gChatHistory.Next(line)) gChat.input = line;
    }
}

static bool WeatherIsSnowy(int x, int z) {
    if (!gWeatherNoise) gWeatherNoise = new PerlinNoise(gWorldSeed);
    return SampleTerrain(*gWeatherNoise, x, z).temperature < 0.24f;
}

static void UpdateWeather(float dt) {
    if (gFreezeTime) return;
    const float rainTarget = gWeather != 0 ? 1.0f : 0.0f;
    const float thunderTarget = gWeather == 2 ? 1.0f : 0.0f;
    const float blend = std::min(1.0f, dt * 0.12f);
    gRainLevel += (rainTarget - gRainLevel) * blend;
    gThunderLevel += (thunderTarget - gThunderLevel) * blend;
    if (gRainLevel < 0.001f) gRainLevel = 0;
    if (gRainLevel > 0.999f) gRainLevel = 1;
    if (gWeatherTicksLeft > 0) {
        gWeatherTicksLeft -= dt * 20.0f;
        if (gWeatherTicksLeft <= 0) {
            gWeatherTicksLeft = 0;
            gWeather = 0;
            gWeatherTimer = 2400.0f;
        }
    } else if (gRuleWeatherCycle && !gAutomation) {
        gWeatherTimer -= dt;
        if (gWeatherTimer <= 0) {
            // Occasional natural storm, deterministic from world time.
            const bool storm = (gWorldClock.Ticks() % 7) == 3;
            gWeather = storm ? 1 : 0;
            gWeatherTicksLeft = storm ? 6000.0f : 0;
            gWeatherTimer = storm ? 0 : 2400.0f;
        }
    }
    if (gRainLevel <= 0.12f || gDimension != 0 || !gSpawned) {
        gRainSpawnTimer = 0;
        return;
    }
    const float particleScale = gSettings.particles == 0 ? 0.0f : (gSettings.particles == 1 ? 0.5f : 1.0f);
    gRainSpawnTimer += dt * 70.0f * gRainLevel * particleScale;
    const bool snow = WeatherIsSnowy(int(floorf(gCamera.x)), int(floorf(gCamera.z)));
    while (gRainSpawnTimer >= 1.0f) {
        gRainSpawnTimer -= 1.0f;
        const float px = gCamera.x - 14.0f + float(rand() % 2800) * 0.01f;
        const float pz = gCamera.z - 14.0f + float(rand() % 2800) * 0.01f;
        const float py = gCamera.y + 7.0f + float(rand() % 500) * 0.01f;
        gParticles.SpawnRain(px, py, pz, snow);
    }
}

static void DropMobLoot(const Mob& mob) {
    if (gGameMode != 0) return;
    const MobStats& stats = GetMobStats(mob.type);
    auto give = [](const char* name, int lo, int hi) {
        if (!name || !name[0] || hi <= 0) return;
        const uint8_t id = ItemIdFromResourceName(name);
        if (id == BLOCK_AIR) return;
        const int count = lo + (hi > lo ? int(rand()) % (hi - lo + 1) : 0);
        if (count > 0) gInventory.Add(id, count);
    };
    give(stats.drop, stats.dropMin, stats.dropMax);
    give(stats.drop2, stats.drop2Min, stats.drop2Max);
    SyncHotbar();
}

static void HandleGameFrame(float dt) {
    gCamera.Look(gInput.mouseDX, gSettings.invertMouse ? -gInput.mouseDY : gInput.mouseDY);
    const float cp = cosf(gCamera.pitch);
    gCamera.fwdX = sinf(gCamera.yaw) * cp;
    gCamera.fwdY = sinf(gCamera.pitch);
    gCamera.fwdZ = -cosf(gCamera.yaw) * cp;
    gCamera.rightX = cosf(gCamera.yaw);
    gCamera.rightZ = sinf(gCamera.yaw);

    auto getBlock = [&](int x, int y, int z) -> uint8_t { return gWorld->GetBlock(x, y, z); };

    if (gResumePending) {
        gWorld->Update(gBody.x, gBody.z);
        const int cx = FloorDiv16(int(floorf(gBody.x))), cz = FloorDiv16(int(floorf(gBody.z)));
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
                if (gWorld->SurfaceY((cx + dx) * 16, (cz + dz) * 16) < 0) return;
        gResumePending = false;
        if (gResumeScreen != Screen::Game) {
            SetScreen(gHwnd, gResumeScreen);
            return;
        }
    }
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
        if (gInventoryTest) {
            gWorld->SetBlock(8, sy + 1, 6, BLOCK_CRAFTING_TABLE);
            gCamera.pitch = -0.3f;
        }
        if (gToolsTest) {
            gWorld->SetBlock(8, sy + 1, 6, BLOCK_STONE);
            gWorld->SetBlock(8, sy + 2, 6, BLOCK_STONE);
            gCamera.pitch = 0;
        }
        if (gCombatTest) {
            Zombie zombie;
            zombie.body.x = 8.5f; zombie.body.y = float(sy + 1); zombie.body.z = 6.0f;
            zombie.passive = true;
            gZombies.push_back(zombie);
            gCamera.pitch = 0;
        }
        gFallStartY = gBody.y;
        gSpawned = true;
        if (gBonusChestPending) {
            gBonusChestPending = false;
            const int bx = 10, bz = 8;
            const int by = gWorld->SurfaceY(bx, bz);
            if (by >= 0 && gGameMode == 0) {
                gWorld->SetBlock(bx, by + 1, bz, BLOCK_CHEST);
                ChestState& chest = gChests[ChestKey(bx, bz)];
                chest = ChestState{};
                chest.slots[0] = {ITEM_WOOD_PICKAXE, 1, 0};
                chest.slots[1] = {ITEM_WOOD_AXE, 1, 0};
                chest.slots[2] = {ITEM_STICK, 4, 0};
                chest.slots[3] = {ITEM_COAL, 3, 0};
            }
        }
    }

    // Vanilla creative double-tap-space toggles flight; survival can never fly.
    if (gGameMode == 1 && gInput.KeyPressed(VK_SPACE)) {
        if (gNowMs - gLastSpaceMs < 300.0f) { gBody.flying = !gBody.flying; gBody.vy = 0; }
        gLastSpaceMs = gNowMs;
    }
    static float sLastWMs = 0;
    if (gInput.KeyPressed('W')) {
        if (gNowMs - sLastWMs < 300.0f) gBody.sprintLatch = true;
        sLastWMs = gNowMs;
    }
    if (!gInput.keys['W'] || gBody.sneaking) gBody.sprintLatch = false;

    const bool wasGround = gBody.onGround;
    gBody.elytraEquipped = !gInventory.armor[1].Empty() && gInventory.armor[1].id == ITEM_ELYTRA;
    if (gPortalCooldown > 0) gPortalCooldown -= dt;
    gBody.Update(gInput, dt, gCamera, getBlock);
    gCamera.x = gBody.x;
    gCamera.y = gBody.y + PlayerBody::EYE;
    gCamera.z = gBody.z;
    if (gPortalCooldown <= 0) {
        const int fx = int(floorf(gBody.x)), fz = int(floorf(gBody.z));
        const uint8_t atFeet = gWorld->GetBlock(fx, int(floorf(gBody.y + 0.2f)), fz);
        const uint8_t atHead = gWorld->GetBlock(fx, int(floorf(gBody.y + 1.4f)), fz);
        if (atFeet == BLOCK_PORTAL || atHead == BLOCK_PORTAL) {
            EnterDimension(1 - gDimension);
            return;
        }
    }
    if (gBody.onGround) {
        if (!wasGround && !gBody.flying && !gBody.inWater && gGameMode == 0 && gPortalCooldown <= 0) {
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
    if (gGameMode == 0) {
        gSurvival.Update(dt, moving, gBody.sprinting, gSettings.difficulty);
        const uint8_t feet = gWorld->GetBlock(int(floorf(gBody.x)),
                                            int(floorf(gBody.y + 0.2f)), int(floorf(gBody.z)));
        if (feet == BLOCK_LAVA) {
            static float sLavaTimer = 0;
            sLavaTimer += dt;
            if (sLavaTimer >= 0.5f) { sLavaTimer -= 0.5f; gSurvival.health -= 2; gSound.Play(SND_HURT); }
        }
        if (gBody.gliding) {
            gElytraWear += dt;
            if (gElytraWear >= 1.0f) { gElytraWear -= 1.0f; WearTool(gInventory.armor[1], 1); }
        }
    }
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
    static float sBubbleTimer = 0.10f;
    if (gBody.inWater) {
        sBubbleTimer += dt;
        if (sBubbleTimer >= 0.10f) {
            sBubbleTimer = 0;
            gParticles.SpawnBubbles(gCamera.x + gCamera.fwdX * 0.25f, gCamera.y - 0.12f,
                                    gCamera.z + gCamera.fwdZ * 0.25f);
        }
    } else {
        sBubbleTimer = 0.10f;
    }
    if (gSettings.viewBobbing && moving && gBody.onGround && !gBody.flying)
        gCamera.y += sinf(gTimeSec * 9.0f) * 0.05f;
    if (gSurvival.dead) { PlayerDied(); return; }

    gWorld->Update(gCamera.x, gCamera.z);
    gPlayerResistance.Advance(dt);
    for (const Zombie& mob : gZombies) {
        if (mob.health > 0) continue;
        DropMobLoot(mob);
        const SoundCat sound = mob.type == MobType::Zombie ? SND_ZOMBIE_DEATH : SND_HIT;
        gSound.PlayAt(sound, mob.body.x, mob.body.y, mob.body.z);
    }
    gZombies.erase(std::remove_if(gZombies.begin(), gZombies.end(), [](const Zombie& zombie) {
        const float dx = zombie.body.x - gBody.x, dz = zombie.body.z - gBody.z;
        return zombie.health <= 0 || dx * dx + dz * dz > 96 * 96;
    }), gZombies.end());
    static float sSayTimer = 0;
    sSayTimer += dt;
    if (sSayTimer >= 6 && !gZombies.empty()) {
        sSayTimer = 0;
        const Zombie& z = gZombies[gWorldClock.Ticks() % gZombies.size()];
        if (z.type == MobType::Zombie)
            gSound.PlayAt(SND_ZOMBIE_SAY, z.body.x, z.body.y, z.body.z);
    }
    if (!gAutomation && gRuleMobSpawning && gSettings.difficulty != 0) {
        gMobSpawnTimer += dt;
        if (gMobSpawnTimer >= 4 && gZombies.size() < 24) {
            gMobSpawnTimer = 0;
            int hostile = 0, passive = 0;
            for (const Zombie& mob : gZombies)
                (GetMobStats(mob.type).hostile ? hostile : passive)++;
            const bool night = gSkyTimeline.Sample(gWorldClock.Ticks()).stars > 0.1f;
            const bool wantHostile = hostile < (night ? 14 : 6);
            const bool wantPassive = passive < 10;
            if (wantHostile || wantPassive) {
                static const MobType kHostile[] = {MobType::Zombie, MobType::Creeper, MobType::Skeleton,
                                                   MobType::Spider, MobType::Enderman};
                static const MobType kPassive[] = {MobType::Pig, MobType::Cow, MobType::Sheep, MobType::Chicken};
                const uint64_t tick = gWorldClock.Ticks();
                const float angle = float(tick % 6283) * 0.001f;
                const int x = int(floorf(gBody.x + sinf(angle) * 26)), z = int(floorf(gBody.z + cosf(angle) * 26));
                const int y = gWorld->SurfaceY(x, z);
                const uint8_t ground = y >= 0 ? gWorld->GetBlock(x, y, z) : BLOCK_AIR;
                if (y >= 0 && ground != BLOCK_AIR && ground != BLOCK_WATER &&
                    gWorld->GetBlock(x, y + 1, z) == BLOCK_AIR && gWorld->GetBlock(x, y + 2, z) == BLOCK_AIR) {
                    Mob mob;
                    const bool spawnHostile = wantHostile && (night || !wantPassive);
                    const MobType type = spawnHostile
                        ? kHostile[(tick / 7) % (sizeof(kHostile) / sizeof(kHostile[0]))]
                        : kPassive[(tick / 11) % (sizeof(kPassive) / sizeof(kPassive[0]))];
                    mob.SetType(type);
                    mob.body.x = float(x) + 0.5f; mob.body.y = float(y + 1); mob.body.z = float(z) + 0.5f;
                    gZombies.push_back(mob);
                }
            }
        }
    }
    for (Zombie& zombie : gZombies) {
        zombie.Update(dt, gBody, getBlock);
        const float dx = gBody.x - zombie.body.x, dz = gBody.z - zombie.body.z;
        const float distance2 = dx * dx + dz * dz;
        if (zombie.type == MobType::Creeper && zombie.fuse >= 0 && zombie.fuse <= 0 && zombie.health > 0) {
            zombie.health = 0;
            gParticles.SpawnCrit(zombie.body.x, zombie.body.y + 1, zombie.body.z);
            gSound.Play(SND_HURT);
            const float blast = sqrtf(distance2);
            if (blast < 6.0f && gGameMode == 0) {
                const float raw = 22.0f * (1.0f - blast / 6.0f);
                const float damage = gPlayerResistance.Apply(raw);
                if (damage > 0) {
                    gSurvival.health = std::max(0, gSurvival.health - int(ceilf(damage)));
                    const float scale = blast > 0.001f ? 6.0f / blast : 0;
                    gBody.impulseX = dx * scale; gBody.impulseZ = dz * scale; gBody.vy = 6; gBody.onGround = false;
                }
            }
            continue;
        }
        if (GetMobStats(zombie.type).attackDamage > 0 && gGameMode == 0 && gSettings.difficulty != 0 &&
            zombie.attackDelay == 0 && distance2 < 1.8f * 1.8f && fabsf(gBody.y - zombie.body.y) < 1.5f) {
            int bx, by, bz, normalX, normalY, normalZ;
            const float distance = sqrtf(distance2);
            if (distance > 0.001f && RaycastDDA(getBlock, zombie.body.x, zombie.body.y + 1.4f, zombie.body.z,
                dx / distance, 0, dz / distance, distance, bx, by, bz, normalX, normalY, normalZ)) continue;
            zombie.attackDelay = 1;
            const float raw = GetMobStats(zombie.type).attackDamage *
                (gSettings.difficulty == 3 ? 1.5f : (gSettings.difficulty == 1 ? 0.7f : 1.0f));
            const int defense = gInventory.Defense();
            const float toughness = gInventory.Toughness();
            float armored = raw;
            if (defense > 0) {
                const float reduction = std::min(20.0f, std::max(defense / 5.0f, defense - raw / (2.0f + toughness / 4.0f)));
                armored = raw * (1.0f - reduction / 25.0f);
            }
            const float damage = gPlayerResistance.Apply(armored);
            if (damage > 0) {
                gSurvival.health = std::max(0, gSurvival.health - int(ceilf(damage)));
                const int wear = std::max(1, int(raw / 4));
                for (ItemSlot& s : gInventory.armor) {
                    const ArmorDef* a = FindArmor(s.id);
                    if (!a) continue;
                    s.damage = uint16_t(std::min(a->durability, int(s.damage) + wear));
                    if (s.damage >= a->durability) s = {};
                }
                const float scale = distance > 0.001f ? 4.0f / distance : 0;
                gBody.impulseX = dx * scale; gBody.impulseZ = dz * scale; gBody.vy = 4; gBody.onGround = false;
                gSound.Play(SND_HURT);
                if (gSurvival.health == 0) { PlayerDied(); return; }
            }
        }
    }
    gPlayer.UpdateCombat(gCooldown, gNowMs);
    gParticles.Update(dt);
    gSound.SetListener(gCamera.x, gCamera.y, gCamera.z,
                       gCamera.fwdX, gCamera.fwdY, gCamera.fwdZ,
                       gCamera.rightX, gCamera.rightZ);

    if (gInput.KeyPressed('F') && gGameMode == 0) {
        std::swap(gInventory.slots[27 + gHotbarSlot], gInventory.offhand);
        SyncHotbar();
    }
    for (int i = 0; i < 9; ++i) {
        if (gInput.KeyPressed('1' + i)) gHotbarSlot = i;
    }
    if (gInput.wheel != 0) {
        // Java's scroll sensitivity scales the wheel delta, which then
        // accumulates until it crosses a whole slot.
        gScrollAccum -= gInput.wheel * gSettings.scrollSensitivity;
        while (gScrollAccum >= 1.0f) { gHotbarSlot = (gHotbarSlot + 1) % 9; gScrollAccum -= 1.0f; }
        while (gScrollAccum <= -1.0f) { gHotbarSlot = (gHotbarSlot + 8) % 9; gScrollAccum += 1.0f; }
        gScrollAccum = std::clamp(gScrollAccum, -1.0f, 1.0f);
    }
    if (gInput.KeyPressed(gSettings.bindInventory)) {
        gCraftTable = false;
        gInventoryMessage.clear();
        ResetCreativeScroll();
        if (gGameMode == 1) {
            // Creative keeps the hotbar in gHotbar; mirror it so the storage
            // rows and hotbar row in the tabbed screen stay consistent.
            for (int i = 0; i < 9; ++i)
                gInventory.slots[27 + i] = {gHotbar[i], gHotbar[i] == BLOCK_AIR ? 0 : ItemStackLimit(gHotbar[i])};
        }
        SetScreen(gHwnd, Screen::Inventory);
        return;
    }
    if (gInput.KeyPressed(VK_F1)) gHideHud = !gHideHud;
    if (gInput.KeyPressed(VK_F3)) gDebug = !gDebug;
    if (gInput.KeyPressed(gSettings.bindPerspective)) gPerspective = (gPerspective + 1) % 3;
    const float zoomTarget = gInput.keys['C'] ? 0.22f : 1.0f;
    gZoom += (zoomTarget - gZoom) * std::min(1.0f, dt * 18.0f);

    int hx, hy, hz, nx, ny, nz;
    gHaveTarget = RaycastDDA(getBlock, gCamera.x, gCamera.y, gCamera.z,
                             gCamera.fwdX, gCamera.fwdY, gCamera.fwdZ, 5.0f,
                             hx, hy, hz, nx, ny, nz);
    if (gHaveTarget) { gTargetX = hx; gTargetY = hy; gTargetZ = hz; }

    const uint8_t held = gHotbar[gHotbarSlot];
    const ToolDef* tool = FindTool(held);
    gCooldown.attackSpeed = tool ? tool->attackSpeed : 4.0f;
    gCooldown.baseDamage = tool ? tool->damage : 1.0f;
    if (held != gAttackItem) { gCooldown.Reset(gNowMs); gAttackItem = held; }
    gPlayer.UpdateCombat(gCooldown, gNowMs);
    float nearest = 3.0f;
    if (gHaveTarget) {
        if (nx) nearest = std::min(nearest, (float(hx + (nx > 0 ? 1 : 0)) - gCamera.x) / gCamera.fwdX);
        else if (ny) nearest = std::min(nearest, (float(hy + (ny > 0 ? 1 : 0)) - gCamera.y) / gCamera.fwdY);
        else if (nz) nearest = std::min(nearest, (float(hz + (nz > 0 ? 1 : 0)) - gCamera.z) / gCamera.fwdZ);
        else nearest = 0;
    }
    Zombie* targetZombie = nullptr;
    for (Zombie& zombie : gZombies) {
        float distance = 0;
        if (zombie.health > 0 && RayHitsBody(gCamera, zombie.body, nearest, distance) && distance < nearest) {
            nearest = distance;
            targetZombie = &zombie;
        }
    }
    if (gInput.leftClick && targetZombie) {
        const float strength = gCooldown.Strength(gNowMs);
        const bool critical = strength > 0.9f && !gBody.onGround && gBody.vy < 0 &&
            !gBody.inWater && !gBody.flying && !gBody.sprinting;
        const float damage = gCooldown.Swing(gNowMs) * (critical ? 1.5f : 1.0f);
        const float knockback = strength > 0.9f && gBody.sprinting ? 8.0f : 4.0f;
        if (targetZombie->Hit(damage, sinf(gCamera.yaw) * knockback, -cosf(gCamera.yaw) * knockback) > 0) {
            ++gCombatHits;
            if (targetZombie->health == 0) ++gZombieKills;
            if (critical) {
                ++gCriticalHits;
                gParticles.SpawnCrit(targetZombie->body.x, targetZombie->body.y + 1, targetZombie->body.z);
            }
            gSound.PlayAt(critical ? SND_CRIT : SND_HIT, targetZombie->body.x, targetZombie->body.y + 1, targetZombie->body.z);
            gSound.PlayAt(SND_ZOMBIE_HURT, targetZombie->body.x, targetZombie->body.y + 1, targetZombie->body.z);
            if (gGameMode == 0) {
                if (WearTool(gInventory.slots[27 + gHotbarSlot], tool && tool->kind == ToolKind::Sword ? 1 : 2))
                    gSound.Play(SND_TOOL_BREAK);
                gSurvival.exhaustion = std::min(4.0f, gSurvival.exhaustion + 0.1f);
                SyncHotbar();
            }
        }
    }
    gMiningDelay = std::max(0.0f, gMiningDelay - dt);
    if (gInput.leftDown && gHaveTarget && !targetZombie && gMiningDelay == 0) {
        const uint8_t old = gWorld->GetBlock(hx, hy, hz);
        if (hx != gMiningX || hy != gMiningY || hz != gMiningZ || held != gMiningItem || old != gMiningBlock) {
            gMiningX = hx; gMiningY = hy; gMiningZ = hz;
            gMiningItem = held; gMiningBlock = old;
            gMiningElapsed = gMiningProgress = gMiningSound = 0;
        }
        const float required = BlockHardness(old) < 0 ? std::numeric_limits<float>::infinity() :
            (gGameMode == 1 ? 0 : MiningSeconds(old, held, gBody.onGround, gBody.inWater));
        gMiningElapsed += dt;
        gMiningProgress = required == 0 ? 1 : std::min(1.0f, gMiningElapsed / required);
        if (gGameMode == 0 && std::isfinite(required)) {
            gMiningSound -= dt;
            if (gMiningSound <= 0) {
                gSound.PlayAt(SoundEngine::StepSound(old), float(hx) + 0.5f, float(hy) + 0.5f,
                              float(hz) + 0.5f, 0.25f, 0.5f);
                gMiningSound = 0.2f;
            }
        }
        if (gMiningProgress >= 1) {
            const ItemSlot drop = gGameMode == 0 ? BlockDrops(old, held) : ItemSlot{};
            if (!drop.Empty() && gInventory.Capacity(drop.id) < drop.count) {
                gActionBar = "Inventory full";
                gActionBarUntil = gNowMs + 1500;
            } else {
                gWorld->SetBlock(hx, hy, hz, BLOCK_AIR);
                ++gBrokenBlocks;
                if (gGameMode == 0) {
                    if (WearTool(gInventory.slots[27 + gHotbarSlot], tool && tool->kind == ToolKind::Sword ? 2 : 1))
                        gSound.Play(SND_TOOL_BREAK);
                    if (!drop.Empty()) gInventory.Add(drop.id, drop.count);
                    gSurvival.exhaustion = std::min(4.0f, gSurvival.exhaustion + 0.005f);
                    SyncHotbar();
                }
                gSound.PlayAt(SoundEngine::BlockSound(old), float(hx) + 0.5f, float(hy) + 0.5f, float(hz) + 0.5f);
                gMiningDelay = 0.25f;
                gMiningElapsed = gMiningProgress = 0;
                gMiningY = -1;
            }
        }
    } else {
        gMiningElapsed = gMiningProgress = 0;
        gMiningY = -1;
    }
    if (gInput.leftClick && !gHaveTarget && !targetZombie) {
        // Java: swinging at empty air plays no sound, only the arm animation.
        gCooldown.Swing(gNowMs);
    }

    if (gInput.rightClick && gBody.gliding && gHotbar[gHotbarSlot] == ITEM_FIREWORK_ROCKET) {
        gBody.vx += gCamera.fwdX * 16.0f;
        gBody.vz += gCamera.fwdZ * 16.0f;
        gBody.vy += 7.0f;
        if (gGameMode == 0) {
            if (--gInventory.slots[27 + gHotbarSlot].count <= 0) gInventory.slots[27 + gHotbarSlot] = {};
            SyncHotbar();
        }
        gSound.Play(SND_CLICK);
        return;
    }

    if (gInput.rightClick && gHaveTarget && (nx || ny || nz)) {
        if (!gInput.keys[VK_SHIFT] && (nx || nz) && gHotbar[gHotbarSlot] == ITEM_FLINT_AND_STEEL &&
            gWorld->GetBlock(hx, hy, hz) == BLOCK_OBSIDIAN) {
            LightPortal(hx, hy, hz, nx, nz);
            if (gGameMode == 0) WearTool(gInventory.slots[27 + gHotbarSlot], 1);
            return;
        }
        if (!gInput.keys[VK_SHIFT] && gWorld->GetBlock(hx, hy, hz) == BLOCK_FURNACE) {
            gFurnace.x = hx; gFurnace.y = hy; gFurnace.z = hz;
            gInventoryMessage.clear();
            SetScreen(gHwnd, Screen::Furnace);
            return;
        }
        if (!gInput.keys[VK_SHIFT] && gWorld->GetBlock(hx, hy, hz) == BLOCK_CHEST) {
            gChestKey = ChestKey(hx, hz);
            gChests[gChestKey];
            gInventoryMessage.clear();
            SetScreen(gHwnd, Screen::Chest);
            return;
        }
        if (!gInput.keys[VK_SHIFT] && gWorld->GetBlock(hx, hy, hz) == BLOCK_CRAFTING_TABLE) {
            gCraftTable = true;
            if (gGameMode == 1) {
                for (int i = 0; i < 9; ++i)
                    gInventory.slots[27 + i] = {gHotbar[i], gHotbar[i] == BLOCK_AIR ? 0 : ItemStackLimit(gHotbar[i])};
            }
            gInventoryMessage.clear();
            SetScreen(gHwnd, Screen::Inventory);
            return;
        }
        const int px = hx + nx, py = hy + ny, pz = hz + nz;
        if (py >= 0 && py < CHUNK_Y && gWorld->HasChunk(FloorDiv16(px), FloorDiv16(pz)) &&
            gWorld->GetBlock(px, py, pz) == BLOCK_AIR) {
            const bool insideBody =
                px + 1 > gBody.x - PlayerBody::HALF && px < gBody.x + PlayerBody::HALF &&
                pz + 1 > gBody.z - PlayerBody::HALF && pz < gBody.z + PlayerBody::HALF &&
                py + 1 > gBody.y && py < gBody.y + PlayerBody::HEIGHT;
            if (!insideBody) {
                const uint8_t block = gHotbar[gHotbarSlot];
                if (block == BLOCK_AIR || block >= BLOCK_COUNT || (gGameMode == 0 && gInventory.slots[27 + gHotbarSlot].Empty())) return;
                gWorld->SetBlock(px, py, pz, block);
                if (gGameMode == 0) {
                    ItemSlot& s = gInventory.slots[27 + gHotbarSlot];
                    if (--s.count <= 0) s = ItemSlot{};
                    SyncHotbar();
                }
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
        1.85f, float(SCREEN_W) / float(SCREEN_H), 0.1f, 100.0f);
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

static void DrawBreakOverlay() {
    if (gMiningProgress <= 0 || !gHaveTarget || gMiningY < 0) return;
    const int stage = std::min(9, int(gMiningProgress * 10));
    ID3D10ShaderResourceView* texture = gGui.texDestroy[stage].srv;
    if (!texture) return;
    const float base[3] = {float(gTargetX), float(gTargetY), float(gTargetZ)};
    gRC.device->PSSetShader(gRC.psCrack);
    gRC.device->PSSetShaderResources(0, 1, &texture);
    gRC.device->OMSetBlendState(gRC.crackBlend, nullptr, 0xFFFFFFFF);
    gRC.device->OMSetDepthStencilState(gRC.depthReadOnly, 0);
    const UINT stride = UINT(sizeof(MeshVertex)), offset = 0;
    gRC.device->IASetVertexBuffers(0, 1, &gRC.celestialVB, &stride, &offset);
    gRC.device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const int order[6] = {0, 1, 2, 0, 2, 3};
    const float uv[4][2] = {{0, 0}, {0.999f, 0}, {0.999f, 0.999f}, {0, 0.999f}};
    for (int axis = 0; axis < 3; ++axis) for (int sign = 0; sign < 2; ++sign) {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        MeshVertex vertices[6] = {};
        for (int i = 0; i < 6; ++i) {
            const int c = order[i];
            float p[3];
            // Offset the face outward 0.002 along its normal to avoid z-fighting;
            // the face still spans the exact block edges on the other two axes.
            p[axis] = base[axis] + (sign ? 1.002f : -0.002f);
            p[u] = base[u] + ((c == 1 || c == 2) ? 1.0f : 0.0f);
            p[v] = base[v] + ((c >= 2) ? 1.0f : 0.0f);
            vertices[i] = {p[0], p[1], p[2], 0, 0, uv[c][0], uv[c][1], 15, 1, 1, 1};
        }
        void* data = nullptr;
        if (FAILED(gRC.celestialVB->Map(D3D10_MAP_WRITE_DISCARD, 0, &data))) break;
        memcpy(data, vertices, sizeof(vertices));
        gRC.celestialVB->Unmap();
        gRC.device->Draw(6, 0);
    }
    gRC.device->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    gRC.device->OMSetDepthStencilState(gRC.depthOn, 0);
}

static void DrawWorldSky(const XMMATRIX& view, const XMMATRIX& proj, const SkyState& sky) {
    const XMMATRIX vp = XMMatrixMultiply(view, proj);
    const UINT stride = UINT(sizeof(MeshVertex)), offset = 0;
    gRC.device->OMSetDepthStencilState(gRC.depthOff, 0);
    gRC.device->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    gRC.device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    SetViewProj(gRC, vp, 0, &sky);
    gRC.device->PSSetShader(gRC.psWorldSky);
    gRC.device->IASetVertexBuffers(0, 1, &gRC.skyVB, &stride, &offset);
    gRC.device->Draw(36, 0);

    gRC.device->PSSetShader(gRC.psCelestial);
    gRC.device->OMSetBlendState(gRC.additiveBlend, nullptr, 0xFFFFFFFF);
    if (sky.stars > 0 && gWhiteSRV) {
        SetViewProj(gRC, XMMatrixMultiply(XMMatrixRotationZ(sky.sunAngle), vp),
            0, &sky, sky.stars, sky.sunAngle);
        gRC.device->PSSetShaderResources(0, 1, &gWhiteSRV);
        gRC.device->IASetVertexBuffers(0, 1, &gRC.starsVB, &stride, &offset);
        gRC.device->Draw(gRC.starVertexCount, 0);
    }
    SetViewProj(gRC, vp, 0, &sky);
    auto celestial = [&](ID3D10ShaderResourceView* texture, float angle, float size) {
        if (!texture) return;
        MeshVertex vertices[6];
        const int corners[6] = {0, 1, 2, 0, 2, 3};
        const float uv[4][2] = {{0,0}, {1,0}, {1,1}, {0,1}};
        for (int i = 0; i < 6; ++i) {
            const int corner = corners[i];
            const float across = corner == 0 || corner == 3 ? -size : size;
            const float vertical = corner < 2 ? size : -size;
            vertices[i] = {-100.0f * sinf(angle) + vertical * cosf(angle),
                100.0f * cosf(angle) + vertical * sinf(angle), across,
                0, 0, uv[corner][0], uv[corner][1], 15, 1, 1, 1};
        }
        void* data = nullptr;
        if (FAILED(gRC.celestialVB->Map(D3D10_MAP_WRITE_DISCARD, 0, &data))) return;
        memcpy(data, vertices, sizeof(vertices));
        gRC.celestialVB->Unmap();
        gRC.device->PSSetShaderResources(0, 1, &texture);
        gRC.device->IASetVertexBuffers(0, 1, &gRC.celestialVB, &stride, &offset);
        gRC.device->Draw(6, 0);
    };
    celestial(gRC.sunSRV, sky.sunAngle, 30.0f);
    celestial(gRC.moonSRV[sky.moonPhase], sky.sunAngle + XM_PI, 20.0f);
    gRC.device->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    gRC.device->OMSetDepthStencilState(gRC.depthOn, 0);
}

struct MobPart {
    float x, y, z, w, h, d, u, v, pivot, rot;
};

// Box models use Java's 16 px/block model space and standard box unwrap UVs
// from the per-entity 64x32 / 64x64 texture.
static void MobParts(MobType type, float moving, std::vector<MobPart>& parts) {
    parts.clear();
    switch (type) {
    case MobType::Pig:
        parts = {{-4, 8, -14, 8, 8, 8, 0, 0, 12, 0},
                 {-5, 6, -8, 10, 8, 16, 28, 8, 10, 0},
                 {-2, 8, -15, 4, 3, 1, 16, 16, 10, 0},
                 {-3, 0, -6, 4, 6, 4, 0, 16, 6, moving},
                 {1, 0, -6, 4, 6, 4, 0, 16, 6, -moving},
                 {-3, 0, 4, 4, 6, 4, 0, 16, 6, -moving},
                 {1, 0, 4, 4, 6, 4, 0, 16, 6, moving}};
        break;
    case MobType::Cow:
        parts = {{-4, 8, -15, 8, 8, 8, 0, 0, 12, 0},
                 {-6, 5, -10, 12, 10, 18, 28, 8, 10, 0},
                 {-4, 0, -7, 4, 7, 4, 0, 16, 7, moving},
                 {2, 0, -7, 4, 7, 4, 0, 16, 7, -moving},
                 {-4, 0, 5, 4, 7, 4, 0, 16, 7, -moving},
                 {2, 0, 5, 4, 7, 4, 0, 16, 7, moving}};
        break;
    case MobType::Sheep:
        parts = {{-3, 8, -13, 6, 6, 6, 0, 0, 11, 0},
                 {-5, 7, -8, 10, 12, 16, 28, 8, 13, 0},
                 {-3, 0, -6, 4, 7, 4, 0, 16, 7, moving},
                 {1, 0, -6, 4, 7, 4, 0, 16, 7, -moving},
                 {-3, 0, 4, 4, 7, 4, 0, 16, 7, -moving},
                 {1, 0, 4, 4, 7, 4, 0, 16, 7, moving}};
        break;
    case MobType::Chicken:
        parts = {{-2, 9, -7, 4, 6, 3, 0, 13, 12, 0},
                 {-3, 5, -3, 6, 8, 6, 0, 9, 9, 0},
                 {-2, 9, -9, 4, 2, 2, 14, 0, 10, 0},
                 {-4, 6, -3, 1, 4, 6, 24, 13, 9, moving},
                 {3, 6, -3, 1, 4, 6, 24, 13, 9, -moving},
                 {-2, 0, -1, 1, 5, 1, 26, 0, 5, moving},
                 {1, 0, -1, 1, 5, 1, 26, 0, 5, -moving}};
        break;
    case MobType::Creeper:
        parts = {{-4, 18, -4, 8, 8, 8, 8, 0, 18, 0},
                 {-4, 6, -2, 8, 12, 4, 16, 16, 12, 0},
                 {-4, 0, -4, 4, 6, 4, 0, 16, 6, moving},
                 {0, 0, -4, 4, 6, 4, 0, 16, 6, -moving},
                 {-4, 0, 0, 4, 6, 4, 0, 16, 6, -moving},
                 {0, 0, 0, 4, 6, 4, 0, 16, 6, moving}};
        break;
    case MobType::Spider:
        parts = {{-4, 4, -9, 8, 8, 8, 32, 4, 8, 0},
                 {-3, 4, -3, 6, 6, 6, 0, 0, 7, 0},
                 {-5, 4, 3, 10, 8, 12, 0, 0, 8, 0},
                 {-15, 6, -6, 12, 1, 1, 0, 0, 6, moving},
                 {-15, 5, -3, 12, 1, 1, 0, 0, 5, -moving},
                 {-15, 5, 0, 12, 1, 1, 0, 0, 5, moving},
                 {-15, 6, 3, 12, 1, 1, 0, 0, 6, -moving},
                 {3, 6, -6, 12, 1, 1, 0, 0, 6, -moving},
                 {3, 5, -3, 12, 1, 1, 0, 0, 5, moving},
                 {3, 5, 0, 12, 1, 1, 0, 0, 5, -moving},
                 {3, 6, 3, 12, 1, 1, 0, 0, 6, moving}};
        break;
    case MobType::Enderman:
        parts = {{-4, 38, -4, 8, 8, 8, 0, 0, 42, 0},
                 {-4, 26, -2, 8, 12, 4, 16, 16, 32, 0},
                 {-6, 2, -2, 2, 34, 2, 40, 16, 36, moving},
                 {4, 2, -2, 2, 34, 2, 40, 16, 36, -moving},
                 {-2, 0, -2, 2, 26, 2, 0, 16, 13, moving},
                 {0, 0, -2, 2, 26, 2, 0, 16, 13, -moving}};
        break;
    case MobType::Skeleton:
    case MobType::Zombie:
    default:
        parts = {{-4, 24, -4, 8, 8, 8, 0, 0, 24, 0},
                 {-4, 12, -2, 8, 12, 4, 16, 16, 12, 0},
                 {-8, 12, -2, 4, 12, 4, 40, 16, 23, XM_PIDIV2},
                 {4, 12, -2, 4, 12, 4, 40, 16, 23, XM_PIDIV2},
                 {-4, 0, -2, 4, 12, 4, 0, 16, 12, moving},
                 {0, 0, -2, 4, 12, 4, 0, 16, 12, -moving}};
        break;
    }
}

static void DrawZombies() {
    if (gZombies.empty() || !gZombieVB) return;
    static std::vector<MeshVertex> vertices;
    static std::vector<MobPart> parts;
    vertices.clear();
    vertices.reserve(gZombies.size() * 216);
    const UINT stride = UINT(sizeof(MeshVertex)), offset = 0;
    gRC.device->PSSetShader(gRC.psEntity);
    for (int t = 0; t < int(MobType::Count); ++t) {
        const GuiTexture& tex = gGui.texMob[t];
        if (!tex.srv) continue;
        const float tw = float(tex.w > 0 ? tex.w : 64), th = float(tex.h > 0 ? tex.h : 32);
        vertices.clear();
        for (const Zombie& zombie : gZombies) {
            if (zombie.health <= 0 || zombie.type != MobType(t)) continue;
            const float cy = cosf(zombie.yaw), sy = sinf(zombie.yaw);
            const bool hurt = zombie.resistance.remaining > 0.5f;
            const float moving = fabsf(zombie.body.vx) + fabsf(zombie.body.vz) > 0.1f
                ? sinf(gTimeSec * 5) * 0.5f : 0.0f;
            MobParts(zombie.type, moving, parts);
            for (const MobPart& part : parts) {
                const float x = part.x, y = part.y, z = part.z, w = part.w, h = part.h, d = part.d;
                const float u = part.u, v = part.v, pivotY = part.pivot, rotation = part.rot;
                const float corners[8][3] = {{x,y,z},{x+w,y,z},{x+w,y+h,z},{x,y+h,z},
                    {x,y,z+d},{x+w,y,z+d},{x+w,y+h,z+d},{x,y+h,z+d}};
                const int faces[6][4] = {{0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{3,2,6,7},{4,5,1,0}};
                const float uv[6][4] = {{u+d,v+d,w,h},{u+2*d+w,v+d,w,h},{u,v+d,d,h},
                    {u+d+w,v+d,d,h},{u+d,v,w,d},{u+d+w,v,w,d}};
                const int triangles[6] = {0,1,2,0,2,3};
                const float cr = cosf(rotation), sr = sinf(rotation);
                for (int face = 0; face < 6; ++face) {
                    for (int index : triangles) {
                        const float* p = corners[faces[face][index]];
                        const float localY = pivotY + (p[1] - pivotY) * cr - p[2] * sr;
                        const float localZ = (p[1] - pivotY) * sr + p[2] * cr;
                        const float tu = uv[face][0] + (index == 1 || index == 2 ? uv[face][2] : 0);
                        const float tv = uv[face][1] + (index < 2 ? uv[face][3] : 0);
                        vertices.push_back({zombie.body.x + (cy * p[0] - sy * localZ) / 16,
                            zombie.body.y + localY / 16, zombie.body.z + (sy * p[0] + cy * localZ) / 16,
                            0, 0, tu / tw, tv / th, face == 4 ? 15.0f : 12.0f,
                            1, hurt ? 0.35f : 1.0f, hurt ? 0.35f : 1.0f});
                    }
                }
            }
        }
        if (vertices.empty()) continue;
        void* data = nullptr;
        if (FAILED(gZombieVB->Map(D3D10_MAP_WRITE_DISCARD, 0, &data))) return;
        memcpy(data, vertices.data(), vertices.size() * sizeof(MeshVertex));
        gZombieVB->Unmap();
        gRC.device->PSSetShaderResources(0, 1, &tex.srv);
        gRC.device->IASetVertexBuffers(0, 1, &gZombieVB, &stride, &offset);
        gRC.device->Draw(UINT(vertices.size()), 0);
    }
}

// Vanilla overworld clouds: a 256x256 alpha texture stretched over 12-block
// cells (3072 blocks per texture repeat), 4 blocks thick at y=192, drifting
// east at 0.03 blocks/tick. The point sampler uses CLAMP, so the plane is
// built from 2x2 aligned texture-repeat quads around the eye.
static void DrawClouds(float eyeX, float eyeY, float eyeZ) {
    if (gSettings.clouds == 0 || gDimension != 0 || !gGui.texClouds.srv || !gZombieVB ||
        eyeY < 0.0f) return;
    const float span = 3072.0f;
    const double ticks = double(gWorldClock.Ticks()) + double(gWorldClock.Fraction());
    const float drift = float(fmod(ticks * 0.03, double(span)));
    const float startX = floorf((eyeX - drift) / span) * span - span + drift;
    const float startZ = floorf(eyeZ / span) * span - span;
    static std::vector<MeshVertex> vertices;
    vertices.clear();
    vertices.reserve(96);
    const float bottom = 192.0f, top = 196.0f;
    for (int iz = 0; iz < 2; ++iz) {
        for (int ix = 0; ix < 2; ++ix) {
            const float x0 = startX + ix * span, x1 = x0 + span;
            const float z0 = startZ + iz * span, z1 = z0 + span;
            const float corners[4][2] = {{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}};
            const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            const int order[6] = {0, 1, 2, 0, 2, 3};
            const int layers = gSettings.clouds == 1 ? 1 : 2; // Fast = flat top only
            for (int face = 0; face < layers; ++face) {
                const float y = face == 0 ? top : bottom;
                const float shade = face == 0 ? 15.0f : 13.0f;
                for (int k : order) {
                    vertices.push_back({corners[k][0], y, corners[k][1], 0, 0,
                        uv[k][0], uv[k][1], shade, 1, 1, 1});
                }
            }
        }
    }
    void* data = nullptr;
    if (FAILED(gZombieVB->Map(D3D10_MAP_WRITE_DISCARD, 0, &data))) return;
    memcpy(data, vertices.data(), vertices.size() * sizeof(MeshVertex));
    gZombieVB->Unmap();
    const UINT stride = UINT(sizeof(MeshVertex)), offset = 0;
    gRC.device->PSSetShader(gRC.psEntity);
    gRC.device->PSSetShaderResources(0, 1, &gGui.texClouds.srv);
    gRC.device->IASetVertexBuffers(0, 1, &gZombieVB, &stride, &offset);
    gRC.device->OMSetBlendState(gRC.alphaBlend, nullptr, 0xFFFFFFFF);
    gRC.device->OMSetDepthStencilState(gRC.depthReadOnly, 0);
    gRC.device->Draw(UINT(vertices.size()), 0);
}

// Java renders a soft circular shadow under living entities. A simple dark
// quad projected onto the first solid block below the entity gives the same
// grounding cue with one draw call for every mob and the third-person player.
static void DrawEntityShadows() {
    if (!gSettings.entityShadows || !gWorld || !gZombieVB || !gGui.texWhite.srv) return;
    struct Shadow { float x, y, z, r; };
    static std::vector<Shadow> list;
    list.clear();
    for (const Zombie& z : gZombies) {
        if (z.health > 0) list.push_back({z.body.x + 0.5f, z.body.y, z.body.z + 0.5f, 0.45f});
    }
    if (gPerspective != 0) list.push_back({gBody.x, gBody.y, gBody.z, 0.45f});
    static std::vector<MeshVertex> verts;
    verts.clear();
    for (const Shadow& s : list) {
        int gy = -1;
        for (int y = int(floorf(s.y)) + 1; y > int(floorf(s.y)) - 8; --y) {
            const uint8_t b = gWorld->GetBlock(int(floorf(s.x)), y, int(floorf(s.z)));
            if (b != BLOCK_AIR && !(BLOCK_FLAGS[b] & BF_LIQUID)) { gy = y; break; }
        }
        if (gy < 0) continue;
        const float y = float(gy + 1) + 0.02f;
        const float r = s.r;
        verts.push_back({s.x - r, y, s.z - r, 0, 0, 0, 0, 15, 0.10f, 0.10f, 0.10f});
        verts.push_back({s.x + r, y, s.z - r, 0, 0, 1, 0, 15, 0.10f, 0.10f, 0.10f});
        verts.push_back({s.x + r, y, s.z + r, 0, 0, 1, 1, 15, 0.10f, 0.10f, 0.10f});
        verts.push_back({s.x - r, y, s.z - r, 0, 0, 0, 0, 15, 0.10f, 0.10f, 0.10f});
        verts.push_back({s.x + r, y, s.z + r, 0, 0, 1, 1, 15, 0.10f, 0.10f, 0.10f});
        verts.push_back({s.x - r, y, s.z + r, 0, 0, 0, 1, 15, 0.10f, 0.10f, 0.10f});
    }
    if (verts.empty()) return;
    void* data = nullptr;
    if (FAILED(gZombieVB->Map(D3D10_MAP_WRITE_DISCARD, 0, &data))) return;
    memcpy(data, verts.data(), verts.size() * sizeof(MeshVertex));
    gZombieVB->Unmap();
    const UINT stride = UINT(sizeof(MeshVertex)), offset = 0;
    gRC.device->PSSetShader(gRC.psEntity);
    gRC.device->PSSetShaderResources(0, 1, &gGui.texWhite.srv);
    gRC.device->IASetVertexBuffers(0, 1, &gZombieVB, &stride, &offset);
    gRC.device->OMSetBlendState(gRC.alphaBlend, nullptr, 0xFFFFFFFF);
    gRC.device->OMSetDepthStencilState(gRC.depthReadOnly, 0);
    gRC.device->Draw(UINT(verts.size()), 0);
    gRC.device->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    gRC.device->OMSetDepthStencilState(gRC.depthOn, 0);
}

static void DrawPlayer() {
    if (gPerspective == 0 || !gZombieVB || !gGui.texSkin.srv) return;
    std::vector<MeshVertex> vertices;
    vertices.reserve(216);
    const float cy = cosf(gCamera.yaw), sy = sinf(gCamera.yaw);
    auto box = [&](float x, float y, float z, float w, float h, float d,
                   float u, float v, float pivotY, float rotation) {
        const float corners[8][3] = {{x,y,z},{x+w,y,z},{x+w,y+h,z},{x,y+h,z},
            {x,y,z+d},{x+w,y,z+d},{x+w,y+h,z+d},{x,y+h,z+d}};
        const int faces[6][4] = {{0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{3,2,6,7},{4,5,1,0}};
        const float uv[6][4] = {{u+d,v+d,w,h},{u+2*d+w,v+d,w,h},{u,v+d,d,h},
            {u+d+w,v+d,d,h},{u+d,v,w,d},{u+d+w,v,w,d}};
        const int triangles[6] = {0,1,2,0,2,3};
        const float cr = cosf(rotation), sr = sinf(rotation);
        for (int face = 0; face < 6; ++face) {
            for (int index : triangles) {
                const float* p = corners[faces[face][index]];
                const float localY = pivotY + (p[1] - pivotY) * cr - p[2] * sr;
                const float localZ = (p[1] - pivotY) * sr + p[2] * cr;
                const float tu = uv[face][0] + (index == 1 || index == 2 ? uv[face][2] : 0);
                const float tv = uv[face][1] + (index < 2 ? uv[face][3] : 0);
                vertices.push_back({gBody.x + (cy * p[0] - sy * localZ) / 16,
                    gBody.y + localY / 16, gBody.z + (sy * p[0] + cy * localZ) / 16,
                    0, 0, tu / 64, tv / 64, face == 4 ? 15.0f : 12.0f, 1, 1, 1});
            }
        }
    };
    const float moving = fabsf(gBody.vx) + fabsf(gBody.vz) > 0.1f ? sinf(gTimeSec * 5) * 0.5f : 0;
    const float armSwing = gPlayer.attackCooldownProgress > 0 ? -1.0f : moving;
    box(-4,24,-4,8,8,8,0,0,24,0);          // head
    box(-4,12,-2,8,12,4,16,16,12,0);       // body
    box(-8,12,-2,4,12,4,40,16,23,armSwing);// right arm
    box(4,12,-2,4,12,4,32,48,23,-moving);  // left arm
    box(-4,0,-2,4,12,4,0,16,12,moving);    // right leg
    box(0,0,-2,4,12,4,16,48,12,-moving);   // left leg
    void* data = nullptr;
    if (FAILED(gZombieVB->Map(D3D10_MAP_WRITE_DISCARD, 0, &data))) return;
    memcpy(data, vertices.data(), vertices.size() * sizeof(MeshVertex));
    gZombieVB->Unmap();
    gRC.device->PSSetShader(gRC.psEntity);
    gRC.device->PSSetShaderResources(0, 1, &gGui.texSkin.srv);
    const UINT stride = UINT(sizeof(MeshVertex)), offset = 0;
    gRC.device->IASetVertexBuffers(0, 1, &gZombieVB, &stride, &offset);
    gRC.device->Draw(UINT(vertices.size()), 0);
}

// Held block: a 3D mini-cube textured from the block atlas (top/side/bottom).
// Local space, centered at origin; u/v span 0..1 so PSMain maps one full tile.
static void BuildHeldBlockCube(uint8_t block, std::vector<MeshVertex>& out) {
    const float h = 0.20f;
    auto tileUV = [](int tile, float& tu, float& tv) {
        tu = float((tile % ATLAS_COLS) * ATLAS_TILE) / float(ATLAS_SIZE);
        tv = float((tile / ATLAS_COLS) * ATLAS_TILE) / float(ATLAS_SIZE);
    };
    // face: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z
    const float n[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (int f = 0; f < 6; ++f) {
        int tile = gTiles.side[block];
        float light = 12.0f;
        if (f == 2) { tile = gTiles.top[block]; light = 15.0f; }
        else if (f == 3) { tile = gTiles.bottom[block]; light = 8.0f; }
        else if (f == 0 || f == 1) { light = 10.0f; }
        float tu, tv; tileUV(tile, tu, tv);
        // Build two tangent axes perpendicular to the face normal.
        float ax[3] = {0,0,0}, ay[3] = {0,0,0};
        if (f == 2 || f == 3) { ax[0] = 1; ay[2] = 1; }
        else if (f == 0 || f == 1) { ax[2] = 1; ay[1] = 1; }
        else { ax[0] = 1; ay[1] = 1; }
        const float cx = n[f][0] * h, cy = n[f][1] * h, cz = n[f][2] * h;
        const float corner[4][2] = {{-1,-1},{1,-1},{1,1},{-1,1}};
        const float uv[4][2] = {{0,1},{1,1},{1,0},{0,0}};
        const int tri[6] = {0,1,2,0,2,3};
        for (int idx : tri) {
            const float sx = corner[idx][0] * h, sy = corner[idx][1] * h;
            MeshVertex v;
            v.x = cx + ax[0] * sx + ay[0] * sy;
            v.y = cy + ax[1] * sx + ay[1] * sy;
            v.z = cz + ax[2] * sx + ay[2] * sy;
            v.tu = tu; v.tv = tv;
            v.u = uv[idx][0]; v.v = uv[idx][1];
            v.light = light; v.cr = v.cg = v.cb = 1.0f;
            out.push_back(v);
        }
    }
}

// First-person arm + held item (3D block cube or extruded 2D sprite).
static void DrawHand() {
    if (gPerspective != 0 || !gZombieVB || !gGui.texSkin.srv) return;
    std::vector<MeshVertex> vertices;
    vertices.reserve(36);
    const float swing = gPlayer.attackCooldownProgress > 0 ? sinf(gPlayer.attackCooldownProgress * 3.14159265f) : 0.0f;
    const float bob = (fabsf(gBody.vx) + fabsf(gBody.vz) > 0.1f && gBody.onGround) ? sinf(gTimeSec * 5.0f) * 0.4f : 0.0f;
    // Java renderPlayerArm: the arm enters from the bottom-right corner and
    // reaches forward-inward (45 deg yaw, ~30 deg pitch) rather than sideways.
    float armX = -0.45f, armY = 0.30f + swing * 0.95f, armZ = -0.85f;
    const float armLen = sqrtf(armX * armX + armY * armY + armZ * armZ);
    armX /= armLen; armY /= armLen; armZ /= armLen;
    float sideX = -armZ, sideZ = armX;
    const float sideLen = sqrtf(sideX * sideX + sideZ * sideZ);
    sideX /= sideLen; sideZ /= sideLen;
    const float upX = -sideZ * armY, upY = sideZ * armX - sideX * armZ, upZ = sideX * armY;
    // Java renders the first-person arm near the bottom-right corner: the
    // shoulder sits just outside the frame and the 12px arm reaches forward.
    // Keep the whole rig small and close to Java's hand depth so it does not
    // swallow the screen (old transform put a 0.25-block-thick arm at z=-0.42).
    const float armScale = 0.62f;
    const float shoulderX = 0.40f, shoulderY = -0.46f + bob * 0.010f, shoulderZ = -0.58f;
    const float x = 0, y = -12, z = -2, w = 4, h = 12, d = 4, u = 40, v = 16;
    const float corners[8][3] = {{x,y,z},{x+w,y,z},{x+w,y+h,z},{x,y+h,z},
        {x,y,z+d},{x+w,y,z+d},{x+w,y+h,z+d},{x,y+h,z+d}};
    const int faces[6][4] = {{0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{3,2,6,7},{4,5,1,0}};
    const float uv[6][4] = {{u+d,v+d,w,h},{u+2*d+w,v+d,w,h},{u,v+d,d,h},
        {u+d+w,v+d,d,h},{u+d,v,w,d},{u+d+w,v,w,d}};
    const int triangles[6] = {0,1,2,0,2,3};
    for (int face = 0; face < 6; ++face) {
        for (int index : triangles) {
            const float* p = corners[faces[face][index]];
            const float lx = (p[0] - 2.0f) / 16.0f * armScale;
            const float ly = (p[1] + 12.0f) / 16.0f * armScale;
            const float lz = p[2] / 16.0f * armScale;
            const float tu = uv[face][0] + (index == 1 || index == 2 ? uv[face][2] : 0);
            const float tv = uv[face][1] + (index < 2 ? uv[face][3] : 0);
            vertices.push_back({
                shoulderX + sideX * lx + armX * ly + upX * lz,
                shoulderY + armY * ly + upY * lz,
                shoulderZ + sideZ * lx + armZ * ly + upZ * lz,
                0, 0, tu / 64, tv / 64, 15, 1, 1, 1});
        }
    }
    void* data = nullptr;
    if (FAILED(gZombieVB->Map(D3D10_MAP_WRITE_DISCARD, 0, &data))) return;
    memcpy(data, vertices.data(), vertices.size() * sizeof(MeshVertex));
    gZombieVB->Unmap();
    const XMMATRIX proj = XMMatrixPerspectiveFovRH(
        gSettings.fov * gZoom * 0.01745329f, float(SCREEN_W) / float(SCREEN_H), 0.05f, 10.0f);
    SetViewProj(gRC, proj);
    gRC.device->PSSetShader(gRC.psEntity);
    gRC.device->PSSetShaderResources(0, 1, &gGui.texSkin.srv);
    const UINT stride = UINT(sizeof(MeshVertex)), offset = 0;
    gRC.device->IASetVertexBuffers(0, 1, &gZombieVB, &stride, &offset);
    gRC.device->Draw(UINT(vertices.size()), 0);

    // Held item: a 3D block cube or an extruded 2D sprite, swung with the arm.
    const uint8_t held = gHotbar[gHotbarSlot];
    if (held != BLOCK_AIR && gHeldVB) {
        std::vector<MeshVertex> local;
        const bool isBlock = held < BLOCK_COUNT;
        if (isBlock) {
            BuildHeldBlockCube(held, local);
        } else if (gItemHasPixels[held] && gGui.texItemIcon[held].srv) {
            BuildExtrudedItemMesh(gItemPixels[held], local);
        }
        if (!local.empty() && local.size() <= 20000) {
            // Java held-item display: 45 deg inward yaw, slight pitch toward the
            // camera so the top and two sides of a block read at the tool angle.
            // Scale the model down and place it at hand depth; the old scale put
            // a full 0.55-unit sprite 0.5 blocks from the eye, filling the view.
            const float itemScale = isBlock ? 0.80f : 0.62f;
            const float ip = -0.30f - swing * 0.45f;
            const float iy = 0.72f;
            const float cp = cosf(ip), sp = sinf(ip);
            const float cy = cosf(iy), sy = sinf(iy);
            for (MeshVertex& v : local) {
                const float sx = v.x * itemScale, syy = v.y * itemScale, sz = v.z * itemScale;
                const float rx = sx * cy + sz * sy;
                const float rz = -sx * sy + sz * cy;
                const float ry = syy * cp - rz * sp;
                const float rz2 = syy * sp + rz * cp;
                v.x = rx + 0.30f;
                v.y = ry - 0.38f + swing * 0.10f + bob * 0.012f;
                v.z = rz2 - 0.72f - swing * 0.08f;
            }
            void* hd = nullptr;
            if (SUCCEEDED(gHeldVB->Map(D3D10_MAP_WRITE_DISCARD, 0, &hd))) {
                memcpy(hd, local.data(), local.size() * sizeof(MeshVertex));
                gHeldVB->Unmap();
                gRC.device->IASetVertexBuffers(0, 1, &gHeldVB, &stride, &offset);
                gRC.device->OMSetDepthStencilState(gRC.depthOn, 0);
                if (isBlock) {
                    gRC.device->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                    gRC.device->PSSetShader(gRC.psWorld);
                    gRC.device->PSSetShaderResources(0, 1, &gRC.atlasSRV);
                } else {
                    gRC.device->OMSetBlendState(gRC.alphaBlend, nullptr, 0xFFFFFFFF);
                    gRC.device->PSSetShader(gRC.psUI);
                    gRC.device->PSSetShaderResources(0, 1, &gGui.texItemIcon[held].srv);
                }
                gRC.device->Draw(UINT(local.size()), 0);
                gRC.device->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            }
        }
    }
}

// LOD ring beyond the streaming radius: one heightfield mesh sampled from the
// generator (no chunk load, no palette, no meshing jobs). 8-block cells keep
// the ridge line recognizably Java-like at a fraction of full chunk cost.
static ID3D10Buffer* gLodVB = nullptr;
static constexpr int LOD_MAX_VERTS = 32768;

static void BuildLodMesh(float eyeX, float eyeZ, std::vector<MeshVertex>& out) {
    constexpr int cell = 8;
    constexpr int LOD_SEA_TOP = 64; // CLIMATE_SEA + 1
    const int rd = gSettings.renderDistance;
    const int rOut = (rd + 8) * CHUNK_X;
    const int rIn = (rd - 1) * CHUNK_X;
    const int cols = (2 * rOut) / cell + 1;
    const int baseX = int(floorf((eyeX - float(rOut)) / cell)) * cell;
    const int baseZ = int(floorf((eyeZ - float(rOut)) / cell)) * cell;
    std::vector<TerrainColumn> grid(size_t(cols) * size_t(cols));
    for (int j = 0; j < cols; ++j)
        for (int i = 0; i < cols; ++i)
            grid[size_t(j) * size_t(cols) + size_t(i)] =
                gWorld->LodColumn(baseX + i * cell, baseZ + j * cell);
    const BlockTiles& tiles = gWorld->Tiles();
    auto tileOrigin = [&](int tile, float& tu, float& tv) {
        tu = float((tile % ATLAS_COLS) * ATLAS_TILE) / float(ATLAS_SIZE);
        tv = float((tile / ATLAS_COLS) * ATLAS_TILE) / float(ATLAS_SIZE);
    };
    auto corner = [&](int i, int j) -> MeshVertex {
        const TerrainColumn& c = grid[size_t(j) * size_t(cols) + size_t(i)];
        const bool water = c.biome == Biome::Ocean || c.biome == Biome::River || c.height <= 62;
        const bool sandy = !water && (c.biome == Biome::Desert || c.height <= 65);
        const bool snowy = !water && !sandy && (c.temperature < 0.24f || c.height >= 155);
        float r = 1.0f, g = 1.0f, b = 1.0f;
        int tile;
        if (water) {
            // Distant ocean reads as water through tint; the sand tile is
            // opaque so the world pass's alpha clip never punches holes.
            r = 0.247f; g = 0.463f; b = 0.894f;
            tile = tiles.top[BLOCK_SAND];
        } else if (sandy) {
            tile = tiles.top[BLOCK_SAND];
        } else if (snowy) {
            tile = tiles.top[BLOCK_SNOW];
        } else if (c.biome == Biome::Taiga) {
            r = 0.525f; g = 0.718f; b = 0.514f;
            tile = tiles.top[BLOCK_GRASS];
        } else if (c.biome == Biome::Mountains) {
            r = 0.541f; g = 0.714f; b = 0.537f;
            tile = tiles.top[BLOCK_GRASS];
        } else {
            r = 0.568f; g = 0.741f; b = 0.349f; // Java plains grass 0x91BD59
            tile = tiles.top[BLOCK_GRASS];
        }
        MeshVertex v;
        v.x = float(baseX + i * cell);
        v.z = float(baseZ + j * cell);
        v.y = water ? float(LOD_SEA_TOP) : float(c.height + 1);
        tileOrigin(tile, v.tu, v.tv);
        v.u = v.x;
        v.v = v.z;
        v.light = 15.0f;
        v.sky = 15.0f;
        v.cr = r; v.cg = g; v.cb = b;
        return v;
    };
    for (int j = 0; j < cols - 1; ++j) {
        for (int i = 0; i < cols - 1; ++i) {
            const float cx = float(baseX + i * cell + cell / 2) - eyeX;
            const float cz = float(baseZ + j * cell + cell / 2) - eyeZ;
            if (cx * cx + cz * cz < float(rIn) * float(rIn)) continue;
            const MeshVertex q00 = corner(i, j), q10 = corner(i + 1, j);
            const MeshVertex q11 = corner(i + 1, j + 1), q01 = corner(i, j + 1);
            out.push_back(q00); out.push_back(q10); out.push_back(q11);
            out.push_back(q00); out.push_back(q11); out.push_back(q01);
        }
    }
}

static void DrawLodTerrain(float eyeX, float eyeZ, const XMMATRIX& vp, const SkyState& sky) {
    if (!gWorld || gDimension != 0) return;
    const WorldGenerator gen = gWorld->Generator();
    if (gen != WorldGenerator::Climate && gen != WorldGenerator::Flat &&
        gen != WorldGenerator::LargeBiomes && gen != WorldGenerator::Amplified) return;
    static std::vector<MeshVertex> verts;
    static float lastX = 1e9f, lastZ = 1e9f;
    static int lastRd = -1, lodCount = 0;
    if (fabsf(eyeX - lastX) > 16.0f || fabsf(eyeZ - lastZ) > 16.0f ||
        lastRd != gSettings.renderDistance) {
        lastX = eyeX; lastZ = eyeZ; lastRd = gSettings.renderDistance;
        verts.clear();
        verts.reserve(size_t(LOD_MAX_VERTS));
        BuildLodMesh(eyeX, eyeZ, verts);
        if (!gLodVB) {
            D3D10_BUFFER_DESC bd = {};
            bd.ByteWidth = UINT(LOD_MAX_VERTS * int(sizeof(MeshVertex)));
            bd.Usage = D3D10_USAGE_DYNAMIC;
            bd.BindFlags = D3D10_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
            if (FAILED(gRC.device->CreateBuffer(&bd, nullptr, &gLodVB))) return;
        }
        lodCount = std::min<int>(int(verts.size()), LOD_MAX_VERTS);
        if (lodCount > 0) {
            void* data = nullptr;
            if (FAILED(gLodVB->Map(D3D10_MAP_WRITE_DISCARD, 0, &data))) return;
            memcpy(data, verts.data(), size_t(lodCount) * sizeof(MeshVertex));
            gLodVB->Unmap();
        }
    }
    if (!gLodVB || lodCount <= 0) return;
    const float fogEnd = float((gSettings.renderDistance + 8) * CHUNK_X);
    SetViewProj(gRC, vp, fogEnd, &sky, 1.0f, 0.0f, 0.0f);
    gRC.device->PSSetShader(gRC.psWorld);
    gRC.device->PSSetShaderResources(0, 1, &gRC.atlasSRV);
    const UINT stride = UINT(sizeof(MeshVertex)), offset = 0;
    gRC.device->IASetVertexBuffers(0, 1, &gLodVB, &stride, &offset);
    gRC.device->RSSetState(gRC.rasterizerLod);
    gRC.device->Draw(UINT(lodCount), 0);
    gRC.device->RSSetState(gRC.rasterizer);
}

static void DrawWorld() {
    float eyeX = gCamera.x, eyeY = gCamera.y, eyeZ = gCamera.z;
    if (gPerspective != 0) {
        const float dir = gPerspective == 1 ? -1.0f : 1.0f;
        float dist = 4.0f;
        int hx, hy, hz, nx, ny, nz;
        if (RaycastDDA([](int x, int y, int z) -> uint8_t { return gWorld->GetBlock(x, y, z); },
                       gCamera.x, gCamera.y, gCamera.z,
                       gCamera.fwdX * dir, gCamera.fwdY * dir, gCamera.fwdZ * dir,
                       dist, hx, hy, hz, nx, ny, nz)) {
            const float t = nx ? (float(hx + (nx > 0 ? 1 : 0)) - gCamera.x) / (gCamera.fwdX * dir)
                          : ny ? (float(hy + (ny > 0 ? 1 : 0)) - gCamera.y) / (gCamera.fwdY * dir)
                          : (float(hz + (nz > 0 ? 1 : 0)) - gCamera.z) / (gCamera.fwdZ * dir);
            dist = std::max(0.5f, t - 0.3f);
        }
        eyeX += gCamera.fwdX * dir * dist;
        eyeY += gCamera.fwdY * dir * dist;
        eyeZ += gCamera.fwdZ * dir * dist;
    }
    const XMVECTOR eye = XMVectorSet(eyeX, eyeY, eyeZ, 1);
    const XMVECTOR at = XMVectorAdd(eye, XMVectorSet(gCamera.fwdX, gCamera.fwdY, gCamera.fwdZ, 0));
    const XMMATRIX view = XMMatrixLookAtRH(eye, at, XMVectorSet(0, 1, 0, 0));
    static float sFov = 70.0f;
    const float targetFov = gSettings.fov * (gBody.sprinting ? 1.12f : 1.0f) * gZoom;
    sFov += (targetFov - sFov) * 0.15f;
    const XMMATRIX proj = XMMatrixPerspectiveFovRH(
        sFov * 0.01745329f, float(SCREEN_W) / float(SCREEN_H), 0.1f, 1000.0f);
    const XMMATRIX vp = XMMatrixMultiply(view, proj);
    SkyState sky = gSkyTimeline.Sample(gWorldClock.Ticks(), gWorldClock.Fraction());
    if (gDimension == 1) {
        sky.skyColor = {0.22f, 0.05f, 0.04f};
        sky.fogColor = {0.35f, 0.09f, 0.05f};
        sky.lightColor = {1.0f, 0.62f, 0.5f};
        sky.daylight = 0.65f;
        sky.stars = 0.0f;
    }
    const bool underwater = gDimension == 0 &&
        gWorld->GetBlock(int(floorf(eyeX)), int(floorf(eyeY)), int(floorf(eyeZ))) == BLOCK_WATER;
    float fogDist = float(gSettings.renderDistance * CHUNK_X);
    if (underwater) {
        sky.skyColor = {0.05f, 0.18f, 0.45f};
        sky.fogColor = {0.05f, 0.18f, 0.45f};
        sky.stars = 0.0f;
        fogDist *= 0.35f;
    }
    if (gRainLevel > 0.001f || gThunderLevel > 0.001f) {
        // Java darkens the sky and sun light while precipitation is visible.
        const float dark = 1.0f - 0.22f * gRainLevel - 0.10f * gThunderLevel;
        sky.skyColor.x *= dark; sky.skyColor.y *= dark; sky.skyColor.z *= dark;
        sky.fogColor.x *= dark; sky.fogColor.y *= dark; sky.fogColor.z *= dark;
        const float lightDim = 1.0f - 0.18f * gRainLevel - 0.06f * gThunderLevel;
        sky.lightColor.x *= lightDim; sky.lightColor.y *= lightDim; sky.lightColor.z *= lightDim;
    }
    const float waterTime = float(gWorldClock.Ticks() % 64u) / 64.0f;
    const XMMATRIX skyView = XMMatrixLookAtRH(XMVectorZero(),
        XMVectorSet(gCamera.fwdX, gCamera.fwdY, gCamera.fwdZ, 0), XMVectorSet(0, 1, 0, 0));
    DrawWorldSky(skyView, proj, sky);
    Frustum frustum;
    ExtractFrustum(vp, frustum);
    SetViewProj(gRC, vp, fogDist, &sky, 1.0f, 0.0f, waterTime);
    gRC.device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gRC.device->OMSetDepthStencilState(gRC.depthOn, 0);
    gRC.device->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    gRC.device->PSSetShader(gRC.psWorld);
    gRC.device->PSSetShaderResources(0, 1, &gRC.atlasSRV);
    const UINT stride = UINT(sizeof(MeshVertex)), offset = 0;

    DrawLodTerrain(eyeX, eyeZ, vp, sky);
    SetViewProj(gRC, vp, fogDist, &sky, 1.0f, 0.0f, waterTime);

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
    DrawEntityShadows();
    DrawZombies();
    DrawPlayer();
    DrawHand();
    DrawClouds(eyeX, eyeY, eyeZ);
    SetViewProj(gRC, vp, fogDist, &sky, 1.0f, 0.0f, waterTime);
    gRC.device->PSSetShader(gRC.psWater);
    gRC.device->PSSetShaderResources(0, 1, &gRC.atlasSRV);
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

    // Crit sweep + underwater oxygen bubbles.
    const auto& parts = gParticles.List();
    if (!parts.empty() && gParticleVB) {
        static MeshVertex critVerts[MAX_PARTICLES * 6];
        static MeshVertex bubbleVerts[MAX_PARTICLES * 6];
        static MeshVertex rainVerts[MAX_PARTICLES * 6];
        static MeshVertex snowVerts[MAX_PARTICLES * 6];
        size_t cn = 0, bn = 0, rn = 0, sn = 0;
        auto quad = [](MeshVertex* dst, const ParticleSystem::Particle& p, float s,
                       float shade, float r, float g, float b) {
            const float rx = gCamera.rightX * s, rz = gCamera.rightZ * s;
            const float x0 = p.x - rx, z0 = p.z - rz;
            const float x1 = p.x + rx, z1 = p.z + rz;
            const float y0 = p.y - s, y1 = p.y + s;
            const MeshVertex q[6] = {
                {x0, y1, z0, 0, 0, 0, 0, shade, r, g, b},
                {x1, y1, z1, 0, 0, 1, 0, shade, r, g, b},
                {x1, y0, z1, 0, 0, 1, 1, shade, r, g, b},
                {x0, y1, z0, 0, 0, 0, 0, shade, r, g, b},
                {x1, y0, z1, 0, 0, 1, 1, shade, r, g, b},
                {x0, y0, z0, 0, 0, 0, 1, shade, r, g, b},
            };
            memcpy(dst, q, sizeof(q));
        };
        auto streak = [](MeshVertex* dst, const ParticleSystem::Particle& p, float w, float h,
                         float shade, float r, float g, float b) {
            const float rx = gCamera.rightX * w, rz = gCamera.rightZ * w;
            const float x0 = p.x - rx, z0 = p.z - rz;
            const float x1 = p.x + rx, z1 = p.z + rz;
            const float y0 = p.y - h, y1 = p.y + h;
            const MeshVertex q[6] = {
                {x0, y1, z0, 0, 0, 0, 0, shade, r, g, b},
                {x1, y1, z1, 0, 0, 1, 0, shade, r, g, b},
                {x1, y0, z1, 0, 0, 1, 1, shade, r, g, b},
                {x0, y1, z0, 0, 0, 0, 0, shade, r, g, b},
                {x1, y0, z1, 0, 0, 1, 1, shade, r, g, b},
                {x0, y0, z0, 0, 0, 0, 1, shade, r, g, b},
            };
            memcpy(dst, q, sizeof(q));
        };
        for (const ParticleSystem::Particle& p : parts) {
            const float fade = p.life / p.maxLife;
            if (p.bubble) {
                if (!gBubbleSRV || bn + 6 > MAX_PARTICLES * 6) continue;
                quad(bubbleVerts + bn, p, 0.05f, 15.0f * fade, 0.55f, 0.80f, 1.0f);
                bn += 6;
            } else if (p.rain) {
                if (!gParticleSRV || rn + 6 > MAX_PARTICLES * 6) continue;
                streak(rainVerts + rn, p, 0.012f, 0.45f, 11.0f * fade, 0.60f, 0.70f, 1.0f);
                rn += 6;
            } else if (p.snow) {
                if (!gParticleSRV || sn + 6 > MAX_PARTICLES * 6) continue;
                streak(snowVerts + sn, p, 0.035f, 0.035f, 14.0f * fade, 1.0f, 1.0f, 1.0f);
                sn += 6;
            } else {
                if (!gParticleSRV || cn + 6 > MAX_PARTICLES * 6) continue;
                quad(critVerts + cn, p, 0.15f, 15.0f * fade, 1.0f, 1.0f, 1.0f);
                cn += 6;
            }
        }
        gRC.device->PSSetShader(gRC.psUI);
        gRC.device->OMSetDepthStencilState(gRC.depthReadOnly, 0);
        gRC.device->OMSetBlendState(gRC.alphaBlend, nullptr, 0xFFFFFFFF);
        const UINT pstride = UINT(sizeof(MeshVertex)), poffset = 0;
        auto flush = [&](ID3D10ShaderResourceView* srv, const MeshVertex* verts, size_t count) {
            if (!srv || count == 0) return;
            gRC.device->PSSetShaderResources(0, 1, &srv);
            void* data = nullptr;
            if (FAILED(gParticleVB->Map(D3D10_MAP_WRITE_DISCARD, 0, &data))) return;
            memcpy(data, verts, count * sizeof(MeshVertex));
            gParticleVB->Unmap();
            gRC.device->IASetVertexBuffers(0, 1, &gParticleVB, &pstride, &poffset);
            gRC.device->Draw(UINT(count), 0);
        };
        flush(gParticleSRV, rainVerts, rn);
        flush(gParticleSRV, snowVerts, sn);
        flush(gParticleSRV, critVerts, cn);
        flush(gBubbleSRV, bubbleVerts, bn);
        gRC.device->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        gRC.device->OMSetDepthStencilState(gRC.depthOn, 0);
    }

    if (gScreen == Screen::Game) { DrawBreakOverlay(); DrawBlockOutline(); }
}

static void DrawDebug() {
    char line[160];
    gGui.DrawQuad(&gGui.texBlack, 2, 2, 360, 236, 0, 0, 1, 1, 15);
    int y = 6;
    auto row = [&](const char* s) { gGui.DrawText(s, 6, float(y), 2, 15, true); y += 20; };
    row("Minecraft 26.3 (Java Edition)");
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
             ItemName(gHotbar[gHotbarSlot]));
    row(line);
    snprintf(line, sizeof(line), "Mode: %s  Seed: %llu", gGameMode ? "Creative" : "Survival",
             static_cast<unsigned long long>(gWorldSeed));
    row(line);
    snprintf(line, sizeof(line), "Local player: %s  Assets: Java %s", gProfile.name.c_str(), gAssetVersion.c_str());
    row(line);
    const RegistryStats& reg = RegistryInfo();
    snprintf(line, sizeof(line), "Registry: %s  %d recipes  %d loot tables",
             reg.jarLoaded ? "jar" : "built-in", reg.recipes, reg.lootTables);
    row(line);
    snprintf(line, sizeof(line), "Names: %d blocks  %d items", reg.mappedBlockNames, reg.mappedItemNames);
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

    const bool menuBg = !IsWorldScreen(gScreen);
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
        if (!gHideHud)
            DrawHUD(gGui, gPlayer.attackCooldownProgress, gHotbarSlot, gHotbar,
                    gSurvival.health, gSurvival.hunger, gInventory.Defense(),
                    gNowMs < gActionBarUntil ? gActionBar : std::string(), gGameMode == 1,
                    gGameMode == 0 ? gInventory.slots + 27 : nullptr);
        DrawChat(gGui, gChat.entries, gChat.open, gChat.input, gTimeSec);
        if (gDebug) DrawDebug();
    } else if (gScreen == Screen::Inventory) {
        if (gGameMode == 0 || gCraftTable)
            DrawSurvivalInventory(gGui, gInventory, gCraftTable, gInput.mouseX,
                                  gInput.mouseY, gInventoryMessage);
        else
            DrawInventory(gGui, gInput.mouseX, gInput.mouseY, gHotbarSlot, gHotbar, gInventory);
    } else if (gScreen == Screen::Furnace) {
        DrawFurnace(gGui, gFurnace, gInventory, gInput.mouseX, gInput.mouseY);
    } else if (gScreen == Screen::Chest) {
        DrawChest(gGui, gChests[gChestKey], gInventory, gInput.mouseX, gInput.mouseY);
    } else {
        if (menuBg && !gRC.panoramaSRV) {
            gGui.DrawQuad(&gGui.texDirt, 0, 0, SCREEN_W, SCREEN_H, 0, 0,
                          SCREEN_W / 64.0f, SCREEN_H / 64.0f, 8);
        }
        gMenu.Draw(gGui, gScreen, gSettings, gCreate, gInput.typed,
                   gInput.mouseX, gInput.mouseY, gInput.leftDown, gSplash, gTimeSec, gProfileDraft,
                   gEditName, gEditingBind);
        if (gMenu.ConsumeChanged()) ApplySettings();
        if (gNowMs < gActionBarUntil) {
            const float messageY = gScreen == Screen::CreateWorld ? SCREEN_H - 68.0f : SCREEN_H - 96.0f;
            gGui.DrawTextCentered(gActionBar, SCREEN_W * 0.5f, messageY, 2, 15, true);
        }
    }
    gGui.Flush(gRC);
    if (gScreenshotRequested) {
        gScreenshotRequested = false;
        SaveScreenshot();
    }
    gRC.swapChain->Present(gSettings.vsync ? 1 : 0, 0);
    if (gSettings.maxFramerate > 0) {
        const auto budget = std::chrono::duration<double>(1.0 / double(gSettings.maxFramerate));
        const auto elapsed = std::chrono::steady_clock::now() - frameStart;
        if (elapsed < budget) std::this_thread::sleep_for(budget - elapsed);
    }
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
    gAutomation = strstr(GetCommandLineA(), "--automation") != nullptr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    LoadPreferences();
    gProfileDraft = gProfile;
    gCreate.difficulty = gSettings.difficulty;

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconA(hInst, MAKEINTRESOURCEA(101));
    wc.hIconSm = static_cast<HICON>(LoadImageA(hInst, MAKEINTRESOURCEA(101), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
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
    const bool registryOk = jarOk && LoadRegistry(gStore);
    if (jarOk && !registryOk) {
        gActionBar = "Recipe and loot data could not be read from the client jar.";
        gActionBarUntil = SystemTimeMs() + 6000;
    }
    if (jarOk) {
        ZipArchive versionArchive;
        std::vector<uint8_t> versionBytes;
        if (versionArchive.Open(jarPath) && versionArchive.Read("version.json", versionBytes)) {
            const auto version = nlohmann::json::parse(versionBytes.begin(), versionBytes.end(), nullptr, false);
            if (version.is_object() && version.contains("id") && version["id"].is_string())
                gAssetVersion = version["id"].get<std::string>();
            std::vector<uint8_t> dayBytes;
            if (versionArchive.Read("data/minecraft/timeline/day.json", dayBytes))
                gSkyTimeline.LoadDay(std::string(dayBytes.begin(), dayBytes.end()));
        }
    }
    const int tilesLoaded = (storeOk || jarOk) ? BuildBlockAtlas(gStore, gAtlas) : 0;
    if (tilesLoaded == 0) {
        gAtlas.AddCheckerTile("__missing");
        MessageBoxA(hwnd,
                    "Minecraft assets not found.\n"
                    "Expected the Migurinth launcher asset index and client jar under %APPDATA%.\n"
                    "Running with checker textures. Overrides: --assets <index.json> --jar <client.jar>",
                    "Potato Edition - assets missing", MB_ICONWARNING);
    }
    gRC.atlasSRV = CreateBlockAtlasSRV(gRC, gAtlas.Pixels(), ATLAS_SIZE, ATLAS_SIZE);
    if (strstr(GetCommandLineA(), "--dump-atlas")) DumpAtlas();
    for (int b = 1; b < BLOCK_COUNT; ++b) {
        gTiles.top[b] = ResolveTile(BLOCK_DEFS[b].top);
        gTiles.side[b] = ResolveTile(BLOCK_DEFS[b].side);
        gTiles.bottom[b] = ResolveTile(BLOCK_DEFS[b].bottom);
    }

    if (!gGui.Init(gRC, gStore)) return 1;
    for (int id = BLOCK_COUNT; id < 256; ++id) {
        if (IsValidItem(id) && LoadItemRGBA(gStore, uint8_t(id), gItemPixels[id]))
            gItemHasPixels[id] = true;
    }
    if (!gGui.LoadPlayerSkin(gRC, gStore, gProfile)) {
        gActionBar = "Player skin missing from the local Java assets.";
        gActionBarUntil = SystemTimeMs() + 4000;
    }
    gSound.Init(gStore);
    CreatePanorama(gRC, gStore); // optional; null falls back to dirt title
    if (!CreateCelestials(gRC, gStore) || !gSkyTimeline.Loaded()) {
        gActionBar = "Sky data missing: install the Java 26.3 client assets.";
        gActionBarUntil = SystemTimeMs() + 6000;
    }
    LoadSplash();
    if (gGameMode == 1) memcpy(gHotbar, HOTBAR_BLOCKS, sizeof(gHotbar));

    {
        std::vector<uint8_t> bytes, px;
        int pw = 0, ph = 0;
        if (gStore.ReadFile("minecraft/textures/particle/critical_hit.png", bytes) &&
            DecodePNGAny(bytes, px, pw, ph)) {
            gParticleSRV = CreateTextureSRV(gRC, px.data(), pw, ph);
        }
        bytes.clear(); px.clear(); pw = ph = 0;
        if (gStore.ReadFile("minecraft/textures/particle/bubble.png", bytes) &&
            DecodePNGAny(bytes, px, pw, ph)) {
            gBubbleSRV = CreateTextureSRV(gRC, px.data(), pw, ph);
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
        obd.ByteWidth = 8 * 6 * 36 * UINT(sizeof(MeshVertex));
        gRC.device->CreateBuffer(&obd, nullptr, &gZombieVB);
        obd.ByteWidth = 20000 * UINT(sizeof(MeshVertex));
        gRC.device->CreateBuffer(&obd, nullptr, &gHeldVB);
    }

    gToolsTest = gAutomation && strstr(GetCommandLineA(), "--tools-test");
    gCombatTest = gAutomation && strstr(GetCommandLineA(), "--combat-test");
    WorldGenerator generator = gAutomation ? WorldGenerator::Legacy : WorldGenerator::Climate;
    if (strstr(GetCommandLineA(), "--climate")) generator = WorldGenerator::Climate;
    if (strstr(GetCommandLineA(), "--flat") || gToolsTest || gCombatTest) generator = WorldGenerator::Flat;
    gWorld = new World(1337, gSettings.renderDistance, generator);
    gWorld->SetBlockTiles(gTiles);
    ApplySettings();
    if (gSettings.fullscreen && !gAutomation) ApplyFullscreen();
    gInventoryTest = strstr(GetCommandLineA(), "--inventory-test") != nullptr;
    if (gInventoryTest) {
        gInventory.slots[27] = {BLOCK_LOG, 1};
        gInventory.slots[28] = {BLOCK_COBBLE, 8};
        SyncHotbar();
    }
    if (gToolsTest || gCombatTest) {
        gInventory.slots[27] = {gToolsTest ? uint8_t(ITEM_WOOD_PICKAXE) : uint8_t(ITEM_DIAMOND_SWORD), 1};
        SyncHotbar();
    }
    if (gAutomation && strstr(GetCommandLineA(), "--save-test")) {
        gSavePath = NewSavePath();
        gWorldName = "Save regression";
    }
    gFreezeTime = gAutomation && strstr(GetCommandLineA(), "--freeze-time");
    if (strstr(GetCommandLineA(), "--autostart")) StartGame();
    if (strstr(GetCommandLineA(), "--show-worlds")) HandleMenuAction(ACT_SINGLEPLAYER);
    if (strstr(GetCommandLineA(), "--debug")) gDebug = true;
    if (strstr(GetCommandLineA(), "--fly")) gFlyHook = true;
    if (strstr(GetCommandLineA(), "--show-options")) {
        gOptionsReturn = Screen::Title;
        SetScreen(hwnd, Screen::Options);
    }
    if (strstr(GetCommandLineA(), "--show-inv")) SetScreen(hwnd, Screen::Inventory);
    if (strstr(GetCommandLineA(), "--show-create")) SetScreen(hwnd, Screen::CreateWorld);
    if (strstr(GetCommandLineA(), "--show-profile")) HandleMenuAction(ACT_PROFILE);

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

        const auto frameStart = std::chrono::steady_clock::now();
        const float dt = clock.Tick();
        gNowMs = SystemTimeMs();
        gTimeSec += dt;
        for (ChatEntry& entry : gChat.entries) entry.age += dt;

        if (gInput.KeyPressed(VK_ESCAPE)) {
            if (gScreen == Screen::Game && gChat.open) CloseChat();
            else if (gScreen == Screen::Game) SetScreen(hwnd, Screen::Pause);
            else if (gScreen == Screen::Pause) SetScreen(hwnd, Screen::Game);
            else if (gScreen == Screen::Inventory) {
                if (gGameMode == 0 || gCraftTable) CloseInventory();
                else SetScreen(hwnd, Screen::Game);
            }
            else if (gScreen == Screen::Furnace) CloseFurnace();
            else if (gScreen == Screen::Chest) CloseChest();
            else if (gScreen == Screen::Options || gScreen == Screen::VideoOptions ||
                     gScreen == Screen::SoundOptions || gScreen == Screen::ControlsOptions)
                HandleMenuAction(ACT_OPTIONS_DONE);
            else if (gScreen == Screen::Profile) HandleMenuAction(ACT_PROFILE_CANCEL);
            else if (gScreen == Screen::ConfirmDelete) HandleMenuAction(ACT_CANCEL_DELETE);
            else if (gScreen == Screen::EditWorld) HandleMenuAction(ACT_EDIT_CANCEL);
            else if (gScreen == Screen::GameRules) HandleMenuAction(ACT_GAMERULES_DONE);
            else if (gScreen == Screen::CreateWorld || gScreen == Screen::SelectWorld)
                HandleMenuAction(ACT_CANCEL_CREATE);
        }
        if (gInput.KeyPressed(VK_F2)) gScreenshotRequested = true;
        if (gInput.KeyPressed(VK_F11) && !gAutomation) {
            gSettings.fullscreen = !gSettings.fullscreen;
            ApplyFullscreen();
        }

        if (gScreen == Screen::Profile) {
            if (gInput.keys[VK_CONTROL] && gInput.KeyPressed('A')) gProfileNameSelected = true;
            for (char c : gInput.typed) {
                if (c == 13) { HandleMenuAction(ACT_PROFILE_DONE); break; }
                const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                     (c >= '0' && c <= '9') || c == '_';
                if (c != 8 && !allowed) continue;
                if (gProfileNameSelected) gProfileDraft.name.clear();
                else if (c == 8 && !gProfileDraft.name.empty()) gProfileDraft.name.pop_back();
                gProfileNameSelected = false;
                if (allowed && gProfileDraft.name.size() < 16) gProfileDraft.name += c;
            }
        }
        if (gScreen == Screen::EditWorld) {
            for (char c : gInput.typed) {
                if (c == 13) { HandleMenuAction(ACT_EDIT_DONE); break; }
                if (c == 8) { if (!gEditName.empty()) gEditName.pop_back(); }
                else if (c >= 32 && c != 127 && gEditName.size() < 40) gEditName += c;
            }
        }
        if (gScreen == Screen::ControlsOptions && gEditingBind >= 0) {
            int pressed = 0;
            for (int vk = 1; vk < 256; ++vk)
                if (gInput.KeyPressed(vk)) { pressed = vk; break; }
            if (pressed == VK_ESCAPE) {
                gEditingBind = -1;
            } else if (pressed) {
                int* binds[BIND_COUNT] = {
                    &gSettings.bindForward, &gSettings.bindBack, &gSettings.bindLeft,
                    &gSettings.bindRight, &gSettings.bindJump, &gSettings.bindSneak,
                    &gSettings.bindSprint, &gSettings.bindInventory, &gSettings.bindDrop,
                    &gSettings.bindChat, &gSettings.bindCommand, &gSettings.bindPerspective};
                if (gEditingBind >= 0 && gEditingBind < BIND_COUNT)
                    *binds[gEditingBind] = pressed;
                gEditingBind = -1;
                SyncBinds();
            }
        }
        if (gScreen == Screen::CreateWorld && !gInput.typed.empty()) {
            for (char c : gInput.typed) {
                if (c == 9) continue;
                if (gCreate.focus == 0) continue;
                if (c == 13) { HandleMenuAction(ACT_CREATE_WORLD); break; }
                std::string& field = gCreate.focus == 2 ? gCreate.seed : gCreate.name;
                if (c == 8) { if (!field.empty()) field.pop_back(); }
                else if (c >= 32 && field.size() < 32) field += c;
            }
        }

        if (gScreen == Screen::Game) {
            if (gChat.open) {
                HandleChatInput();
            } else if (gInput.KeyPressed(gSettings.bindChat)) {
                OpenChat(false);
            } else if (gInput.KeyPressed(gSettings.bindCommand)) {
                OpenChat(true);
            }
            if (gChat.open) {
                // Java freezes movement and mouse look while chat has focus.
                const InputState saved = gInput;
                gInput = InputState{};
                gInput.mouseX = saved.mouseX;
                gInput.mouseY = saved.mouseY;
                HandleGameFrame(dt);
                gInput = saved;
            } else {
                HandleGameFrame(dt);
            }
        } else if (gScreen == Screen::Inventory) {
            if (gInput.KeyPressed(gSettings.bindInventory)) {
                if (gGameMode == 0 || gCraftTable) CloseInventory();
                else CloseCreativeInventory();
            } else if (gGameMode == 0 || gCraftTable) {
                if (gInput.leftClick || gInput.rightClick)
                    HandleInventoryClick(
                        SurvivalInvHitTest(gInput.mouseX, gInput.mouseY, gCraftTable),
                        gInput.rightClick);
            } else {
                // Java's CreativeModeInventoryScreen: tab strip on top, live
                // search field, picker grid and player slots below the cursor.
                if (CreativeTab() == CTAB_SEARCH)
                    for (char c : gInput.typed) {
                        if (c == 8) CreativeSearchBackspace();
                        else CreativeSearchAppend(c);
                    }
                if (gInput.wheel != 0)
                    ScrollCreativeInventory(gInput.wheel > 0 ? -1 : 1);
                if (gInput.leftClick || gInput.rightClick) {
                    const int tab = CreativeTabHitTest(gInput.mouseX, gInput.mouseY);
                    if (tab >= 0) {
                        SetCreativeTab(tab);
                        gSound.Play(SND_CLICK);
                    } else if (CreativeSearchFieldHitTest(gInput.mouseX, gInput.mouseY)) {
                        // The search box is always focused while its tab is open.
                    } else if (const int b = InventoryHitTest(gInput.mouseX, gInput.mouseY); b > 0) {
                        const uint8_t id = uint8_t(b);
                        const int limit = ItemStackLimit(id);
                        if (gInput.keys[VK_SHIFT]) {
                            gInventory.Add(id, limit);
                            SyncHotbar();
                        } else {
                            // Java's creative picker fills the cursor with a
                            // full stack on either button.
                            gInventory.cursor = {id, limit, 0};
                        }
                        gSound.Play(SND_CLICK);
                    } else {
                        const int hot = CreativeHotbarHitTest(gInput.mouseX, gInput.mouseY);
                        const int slot = CreativeSlotHitTest(gInput.mouseX, gInput.mouseY);
                        const int index = hot >= 0 ? 27 + hot : slot;
                        if (index >= 0 && gInventory.Click(index, gInput.rightClick, false,
                                                           gInput.keys[VK_SHIFT])) {
                            SyncHotbar();
                            gSound.Play(SND_CLICK);
                        }
                    }
                }
            }
        } else if (gScreen == Screen::Furnace) {
            if (gInput.KeyPressed(gSettings.bindInventory)) CloseFurnace();
            else if (gInput.leftClick || gInput.rightClick)
                HandleFurnaceClick(FurnaceHitTest(gInput.mouseX, gInput.mouseY), gInput.rightClick);
        } else if (gScreen == Screen::Chest) {
            if (gInput.KeyPressed(gSettings.bindInventory)) CloseChest();
            else if (gInput.leftClick || gInput.rightClick)
                HandleChestClick(ChestHitTest(gInput.mouseX, gInput.mouseY), gInput.rightClick);
        } else if (gInput.leftClick) {
            HandleMenuAction(gMenu.HitTest(gInput.mouseX, gInput.mouseY));
        }

        if (gSpawned && !gResumePending && !gFreezeTime &&
            (gScreen == Screen::Game || gScreen == Screen::Inventory ||
             gScreen == Screen::Furnace || gScreen == Screen::Chest || gScreen == Screen::Death)) {
            if (gRuleDaylightCycle) gWorldClock.Advance(dt);
            UpdateWeather(dt);
            TickFurnace(dt);
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

    if (!SaveCurrentWorld())
        MessageBoxA(hwnd, "The world could not be saved. Check free space and folder permissions.",
                    "Minecraft Potato Edition - Save failed", MB_OK | MB_ICONERROR);
    for (auto& kv : gGpu) {
        SafeRelease(kv.second.vb);
        SafeRelease(kv.second.ib);
    }
    gGpu.clear();
    SetScreen(hwnd, Screen::Title);
    if (!gAutomation) SavePreferences();
    delete gWorld;
    delete gWeatherNoise;
    gWeatherNoise = nullptr;
    gSound.Shutdown();
    gGui.Shutdown();
    SafeRelease(gParticleSRV);
    SafeRelease(gBubbleSRV);
    SafeRelease(gParticleVB);
    SafeRelease(gWhiteSRV);
    SafeRelease(gOutlineVB);
    SafeRelease(gZombieVB);
    SafeRelease(gHeldVB);
    ShutdownRenderer(gRC);
    DestroyWindow(hwnd);
    CoUninitialize();
    return 0;
}