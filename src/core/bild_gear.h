#pragma once
// The functions SacredBild (ddraw.dll) and Sacred Gear (SacredGear.dll) export for each other, declared once for both
// sides: SacredBild's definitions are checked against these declarations by the compiler, and Sacred Gear casts what
// GetProcAddress returns to decltype(&<name>), so a signature that changes on one side fails to compile on the other.
// Neither DLL needs the other: a missing export is a nullptr from GetProcAddress, and the feature stays off.
// docs/Development.md, "With SacredBild".
#include <windows.h>
#include <cstdint>

struct IDirectDrawSurface7;
struct IDirect3DDevice7;

// SacredGear.dll exports its functions with dllexport (SACREDGEAR_BUILD is set for its sources), ddraw.dll through exports.def.
#ifdef SACREDGEAR_BUILD
#define SACREDGEAR_EXPORT __declspec(dllexport)
#else
#define SACREDGEAR_EXPORT
#endif

// A stop of the controller's D-pad navigation over mod UI (Gear's PadNav::Stop has this layout).
struct SacredBild_NavStop
{
    float x, y;                     // where the cursor goes
    float left, top, width, height;
    int32_t kind;                   // 0 button, 1 slider
};

extern "C"
{
    // ddraw.dll -> SacredGear.dll: UI frames, window placement and bounds (ui_canvas, ui_anchor, ui_windows).
    int __cdecl SacredBild_UiPushFrame(void* window, int confine);
    void __cdecl SacredBild_UiPopFrame();
    int __cdecl SacredBild_UiScreenBounds(float* bounds);
    int __cdecl SacredBild_UiSetWindowFrame(int slot, float x, float y, int set);
    int __cdecl SacredBild_UiGetWindowFrame(int slot, float* x, float* y);
    int __cdecl SacredBild_UiGetWindowBounds(int slot, float* rect);
    int __cdecl SacredBild_UiGetPopupFrame(const void* popup, float* x, float* y);
    int __cdecl SacredBild_UiPlacement(float* out);
    int __cdecl SacredBild_UiSetCursorFilter(int(__cdecl* filter)(int x, int y));
    void __cdecl SacredBild_UiSetWheelHandler(int(__cdecl* fn)(int delta));
    int __cdecl SacredBild_UiTakeScroll();

    // The controller over mod UI (gear_nav).
    void __cdecl SacredBild_UiSetNavStops(const SacredBild_NavStop* stops, int count, int flags);
    int __cdecl SacredBild_UiNavState(int* flags);
    int __cdecl SacredBild_UiTakePad(int* nudge, int* back, int* menu);
    void __cdecl SacredBild_UiPostPad(int nudge, int back, int menu);
    int __cdecl SacredBild_UiTakePage();
    void __cdecl SacredBild_UiPostPage(int pages);
    int __cdecl SacredBild_UiTakeSelect();
    void __cdecl SacredBild_UiPostScroll(int steps);
    void __cdecl SacredBild_UiSetNativePrompts(const float* rects, int count);
    int __cdecl SacredBild_PadInfo(int* style);
    int __cdecl SacredBild_PadGlyph(int style, unsigned button, unsigned* pixels);

    // The game window's procedure (focus) and pets (pets).
    void __cdecl SacredBild_SubclassWindow();
    int __cdecl SacredBild_PetSet(unsigned id, float scale, int untargetable);
    void __cdecl SacredBild_PetClear();

    // Creature tints and texture swaps (pets): set per object id and part, cleared all at once; the resolver turns a token into
    // the surface to draw with.
    int __cdecl SacredBild_TintSet(unsigned id, unsigned part, unsigned argb);
    void __cdecl SacredBild_TintClear();
    int __cdecl SacredBild_TextureSet(unsigned id, unsigned part, unsigned token);
    void __cdecl SacredBild_TextureResolver(IDirectDrawSurface7*(__cdecl* resolver)(IDirect3DDevice7* device, unsigned token));

    // Item type textures (pets): the inventory's item icon is a model drawn by the game's model renderer for an item *type*, not an object.
    // `TypeTextureSet` gives a type other textures (`part` 0 the whole model, 1..8 a mesh; `token` 0 takes it away); Sacred Gear brackets the
    // game's icon draw of a type with `IconBegin` / `IconEnd`, and the meshes drawn in between use them.
    int __cdecl SacredBild_TypeTextureSet(unsigned type, unsigned part, unsigned token);
    void __cdecl SacredBild_IconBegin(unsigned type);
    void __cdecl SacredBild_IconEnd();

    // SacredGear.dll -> ddraw.dll: the foreground window of an isolated automation run (automation).
    SACREDGEAR_EXPORT HWND __cdecl SacredGear_IsolatedForeground();
}
