#pragma once

namespace potato {

// Java 26.3 creative inventory geometry, read from the client JAR with
// tools/parity/classdump.ps1 (dumps in run/parity/creative*.txt):
//   CreativeModeInventoryScreen.<init> : imageWidth 195, imageHeight 136
//   ItemPickerMenu.<init>              : 5 rows x 9 cols of CustomCreativeSlot
//                                        at (9 + col*18, 18 + row*18), then
//                                        addInventoryHotbarSlots(inventory, 9, 112)
//   AbstractContainerMenu.addInventoryHotbarSlots : 9 slots at (x + i*18, y)
//                                        -> the creative menu has NO storage rows
//   CreativeModeInventoryScreen.init   : search EditBox at (leftPos+82, topPos+6),
//                                        80x9, maxLength 50, borderless, white text
// AbstractContainerScreen.init centres the panel, so topPos = (height-136)/2.
// Everything below is in screen pixels at GUI scale 2 (1366x768 default).
struct CreativeLayout {
    static constexpr float GuiScale = 2.0f;
    static constexpr float Cell = 18.0f * GuiScale;
    static constexpr int Cols = 9;
    static constexpr int PickRows = 5;
    static constexpr float PanelW = 195.0f * GuiScale;
    static constexpr float PanelH = 136.0f * GuiScale;
    static constexpr float PickX = 9.0f * GuiScale;
    static constexpr float PickY = 18.0f * GuiScale;
    static constexpr float HotbarY = 112.0f * GuiScale;
    static constexpr float SearchX = 82.0f * GuiScale;
    static constexpr float SearchY = 6.0f * GuiScale;
    static constexpr float SearchW = 80.0f * GuiScale;
    static constexpr float SearchH = 9.0f * GuiScale;
    static constexpr int SearchMaxLen = 50;
    static constexpr float IconInset = 2.0f; // 16px icon in an 18px cell, scaled

    // CreativeModeTab rows/columns (JAR sprite files tab_top_*_1..7 and
    // tab_bottom_*_1..7 = column+1). CTAB order: building, colored, natural,
    // functional, redstone, tools, combat, food, ingredients, search.
    // Java puts combat/food/ingredients on the bottom row, search top-right.
    static constexpr float TabW = 28.0f * GuiScale;
    static constexpr float TabH = 32.0f * GuiScale;
    static constexpr float TabOverlap = 4.0f * GuiScale;
    static constexpr int TabRow(int tab) { return (tab >= 6 && tab <= 8) ? 1 : 0; }
    static constexpr int TabCol(int tab) { return tab == 9 ? 6 : (tab >= 6 ? tab - 6 : tab); }
    static constexpr float TabX(float screenW, int tab) { return PanelX(screenW) + TabCol(tab) * TabW; }
    static constexpr float TabY(float screenH, int tab) {
        return TabRow(tab) == 0 ? PanelY(screenH) - TabH + TabOverlap
                                : PanelY(screenH) + PanelH - TabOverlap;
    }

    static constexpr float PanelX(float screenW) { return (screenW - PanelW) * 0.5f; }
    static constexpr float PanelY(float screenH) { return (screenH - PanelH) * 0.5f; }

    static constexpr float PickerX(float screenW) { return PanelX(screenW) + PickX; }
    static constexpr float PickerY(float screenH) { return PanelY(screenH) + PickY; }
    static constexpr float HotbarX(float screenW) { return PanelX(screenW) + PickX; }
    static constexpr float HotbarRowY(float screenH) { return PanelY(screenH) + HotbarY; }

    // Flat index into the picker list, or -1. scrollRows shifts the grid up.
    static int HitPicker(float mx, float my, float screenW, float screenH, int scrollRows) {
        const float rx = mx - PickerX(screenW), ry = my - PickerY(screenH);
        if (rx < 0.0f || ry < 0.0f || rx >= Cols * Cell || ry >= PickRows * Cell) return -1;
        const int col = int(rx / Cell), row = int(ry / Cell);
        return (scrollRows + row) * Cols + col;
    }

    // Hotbar slot 0..8 inside the panel, or -1.
    static int HitHotbar(float mx, float my, float screenW, float screenH) {
        const float rx = mx - HotbarX(screenW), ry = my - HotbarRowY(screenH);
        if (rx < 0.0f || ry < 0.0f || rx >= Cols * Cell || ry >= Cell) return -1;
        return int(rx / Cell);
    }

    static bool HitSearch(float mx, float my, float screenW, float screenH) {
        const float x = PanelX(screenW) + SearchX, y = PanelY(screenH) + SearchY;
        return mx >= x && mx < x + SearchW && my >= y && my < y + SearchH;
    }
};

} // namespace potato