#pragma once
#include <cstdint>

// Addresses for the German Sacred Gold build (sacred.exe, PE timestamp 0x451BBE74).
// Names match the Ghidra program "/sacred.exe (DE)".
namespace Sacred::Addr
{
    constexpr uint32_t kTimestamp = 0x451BBE74;
    constexpr uint32_t kSizeOfImage = 0x01999000;
    constexpr uint32_t kEntryPoint = 0x0044C704;

    // dxDriver7 (DirectDraw7 + Direct3D7 wrapper, one global instance)
    constexpr uintptr_t g_pDxDriver = 0x00CDA99C;
    constexpr uintptr_t dxDriver7_ctor = 0x00644960;
    constexpr uintptr_t dxDriver7_createSurface = 0x00644BE0;
    constexpr uintptr_t dxDriver7_createZBuffer = 0x00645000;
    constexpr uintptr_t dxDriver7_init = 0x00645390;
    constexpr uintptr_t dxDriver7_flip = 0x00645F70;
    constexpr uintptr_t dxDriver7_lockBack = 0x00646280;
    constexpr uintptr_t dxDriver7_unlockBack = 0x006462F0;
    constexpr uintptr_t dxDriver7_setFullViewport = 0x00646510;
    constexpr uintptr_t dxDriver7_drawTexturedQuad = 0x006466B0;
    constexpr uintptr_t dxDriver7_drawLoadingScreen = 0x006469D0;
    constexpr uintptr_t dxDriver7_beginScene = 0x00646F50;
    constexpr uintptr_t dxDriver7_endScene = 0x00646FD0;

    // cDxDevices::findMode(w, h, bpp, flags) -> mode record; called once by initApp with 1024x768.
    constexpr uintptr_t cDxDevices_findMode = 0x00644260;

    // Return address of the CreateWindowExA call that creates the main game window.
    constexpr uintptr_t mainWindowCreateReturn = 0x00664A5E;

    // Mouse: getClientCursorPos(hwnd, POINT*) is the only cursor read; cMouse is the singleton the UI and world poll.
    constexpr uintptr_t getClientCursorPos = 0x0066E500;
    constexpr uintptr_t cMouse_instance = 0x006550F0;
    constexpr uintptr_t cMouse_renderCursor = 0x006555E0;       // (device, flag)

    // Renders the hero into the back buffer and reads the centered region back for the savegame JPEG.
    constexpr uintptr_t renderSavePortrait = 0x004B1740;        // thiscall (path, w, h, scale), this = 0xAAAF00
    // Plays an intro/cutscene video through DirectShow onto a fullscreen 1024x768 TL quad.
    constexpr uintptr_t playVideo = 0x006A0C60;                 // thiscall, 5 stack args

    // UI framework (cUI_Control2 / cUI_Window2 / cUI_Manager)
    constexpr uintptr_t g_pUiManager = 0x017ECB3C;              // cUI_Manager*
    constexpr uintptr_t cUI_Manager_isCursorOverUi = 0x0075A370; // thiscall (x, y) -> bool, UI coordinates
    constexpr uintptr_t cUI_Control2_ctor = 0x00731420;          // (name, x, y, w|h<<16, parent, flags)
    constexpr uintptr_t cUI_Window2_addChild = 0x00727120;       // pushes into the children vector
    constexpr uintptr_t cUI_Manager_createGameWindows = 0x007593D0;
    constexpr uintptr_t cUI_Window2_typeDescriptor = 0x009DE188;

    // Texture manager (one instance): loads textures on use, evicts least recently used ones above a budget.
    // initApp computes the budget from GlobalMemoryStatus and the reported video memory (min 32 MB).
    constexpr uintptr_t g_pTextureManager = 0x013E57B8;
    constexpr uintptr_t cTextureManager_init = 0x0065EA20;      // thiscall (budgetBytes)

    // cEngine
    constexpr uintptr_t initApp = 0x00815620;
    constexpr uintptr_t cEngine_renderThreadRun = 0x0060E3F0;   // fastcall (engine), loops until shutdown
    // cEngine_updateWorldCursor (0x611F10) asks the UI whether the cursor is over a window before picking.
    constexpr uintptr_t worldCursorUiTestCall = 0x00611F83;
    constexpr uintptr_t cEngine_captureInternal = 0x00613710;
    constexpr uintptr_t captureLockBackReturn = 0x0061374B;     // after captureInternal's call to lockBack

    // 3D model path: cCreature vtable[5] render -> cObject3D_render (needs the Granny model attached, flags bit 26)
    // -> cObject3D_drawModel -> cGranny_render* -> DrawIndexedPrimitiveStrided.
    constexpr uintptr_t cCreature_render = 0x00599880;          // thiscall (device, 0)
    constexpr uintptr_t cObject3D_render = 0x0044B400;          // thiscall (device, ?)
    constexpr uintptr_t cObject3D_drawModel = 0x0044ABA0;       // thiscall (device, model, instance, flags64)

    // World <-> pixel conversion (cdecl (out, point, rotation, projection) -> out). Pixels assume a projection
    // spanning 1024x768 pixels: true for g_unzoomedProjection, not for the device projection (W x H).
    constexpr uintptr_t pixelsToWorld = 0x00623A20;
    constexpr uintptr_t worldToPixels = 0x00623C40;
    constexpr uintptr_t g_unzoomedProjection = 0x0182CCF0;

    // Ground: renderTileRow draws each tile's base through the quad batcher (cWorldView + 0x86890) and collects
    // tiles with blend layers into a fixed array (at most 0x6D5 per frame); after all rows cWorldView0_render
    // flushes the batcher and cWorldView_drawTileLayers draws the collected layers.
    constexpr uintptr_t cWorldView_renderTileRow = 0x0062B000;  // thiscall (device, rowPos, detail)
    constexpr uintptr_t cWorldView_drawTileLayers = 0x0062D530; // thiscall (device)
    constexpr uintptr_t cQuadBatcher_flush = 0x00629420;        // thiscall (device)

    // Engine input: cEngine_receiveEvent (0x618130) passes mouse events to the UI manager first (UI coordinates)
    // and then to the world mouse handler, which picks with the event position.
    constexpr uintptr_t cEngine_worldMouse = 0x00617360;        // thiscall (event, flag) -> bool
    constexpr uintptr_t cEventMouseDown_vtable = 0x008950A8;    // x at +8, y at +0xC
    constexpr uintptr_t cEventMouseUp_vtable = 0x00897248;

    // cWorldView0 vtable slot 5: draws the isometric world (arg: device).
    constexpr uintptr_t cWorldView0_render = 0x006322B0;

    // cUI_Manager::render(device): letterbox bars + all UI windows.
    constexpr uintptr_t cUI_Manager_render = 0x007587B0;
}

// dxDriver7 member offsets (identical in the ENG and DE builds).
namespace Sacred::DxDriver
{
    constexpr uintptr_t height = 0x1C;       // uint16
    constexpr uintptr_t width = 0x20;        // uint16
    constexpr uintptr_t windowed = 0x90;     // 1 = fullscreen, else windowed
    constexpr uintptr_t bpp = 0x94;
    constexpr uintptr_t hwnd = 0xB0;
    constexpr uintptr_t ddraw = 0xB4;        // IDirectDraw7*
    constexpr uintptr_t primary = 0xB8;      // IDirectDrawSurface7*
    constexpr uintptr_t back = 0xBC;         // IDirectDrawSurface7* (3D render target)
    constexpr uintptr_t d3d = 0xC8;          // IDirect3D7*
    constexpr uintptr_t device = 0xCC;       // IDirect3DDevice7*
}

// cUI_Control2 member offsets. Window children keep absolute screen coordinates.
namespace Sacred::UiControl
{
    constexpr uintptr_t flags = 0x10;        // bit 0: visible
    constexpr uintptr_t x = 0x24;
    constexpr uintptr_t y = 0x28;
    constexpr uintptr_t width = 0x2C;        // int16
    constexpr uintptr_t height = 0x2E;       // int16
    constexpr uintptr_t name = 0x30;         // char[0x20]
    constexpr uintptr_t parent = 0x50;
    constexpr uintptr_t childrenBegin = 0x78; // cUI_Window2 only: std::vector<cUI_Control2*>
    constexpr uintptr_t childrenEnd = 0x7C;
}

// cEngine member offsets.
namespace Sacred::Engine
{
    constexpr uintptr_t flags = 0x54;        // 0x10000 loading screen, 0x20000 fade out, 0x40000 fade in, 0x80000 black
}

// cWorldView member offsets (ground layers).
namespace Sacred::WorldView
{
    constexpr uintptr_t quadBatcher = 0x86890;
    constexpr uintptr_t layeredTileCount = 0x3FF1C;
    constexpr uint32_t layeredTileCapacity = 0x6D5;
}

// cTextureManager member offsets.
namespace Sacred::TextureManager
{
    constexpr uintptr_t usedBytes = 0xC001C;
    constexpr uintptr_t budgetBytes = 0xC0020;
}

// cMouse member offsets.
namespace Sacred::Mouse
{
    constexpr uintptr_t x = 0x04;
    constexpr uintptr_t y = 0x08;
}

// cUI_Manager: top-level game windows created by createGameWindows live in this pointer range.
namespace Sacred::UiManager
{
    constexpr uintptr_t flags = 0x08;        // 0x01 menus, 0x04 in game, 0x10 cinematic
    constexpr uintptr_t firstGameWindow = 0x80;
    constexpr uintptr_t lastGameWindow = 0xD8;
}
