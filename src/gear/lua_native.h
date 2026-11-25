#pragma once
// What the native side of the Lua host shares between its files: lua_host.cpp (the state, the budget, the kernel
// calls and the natives every area needs) and the *_lua.cpp files next to the modules whose natives they hold.
// Each of those registers its natives into the native table on top of the stack (register<Area>).
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <lua.hpp>

#include "items/inventory.h"
#include "save.h"
#include "ui/text.h"
#include "ui/ui.h"
#include "world/party.h"
#include "world/world.h"

namespace LuaNative
{
    // Sets each entry of `regs` (ending with {nullptr, nullptr}) as a field of the table on top of the stack.
    void setFunctions(lua_State* L, const luaL_Reg* regs);

    void registerTables(lua_State* L, bool automation);   // tables_lua.cpp
    void registerWorld(lua_State* L, bool automation);   // world/world_lua.cpp
    void registerItems(lua_State* L, bool automation);   // items/items_lua.cpp
    void registerSave(lua_State* L, bool automation);   // save_lua.cpp
    void registerUi(lua_State* L, bool automation);   // ui/ui_lua.cpp
    void registerNet(lua_State* L, bool automation);   // net_lua.cpp
    void registerParty(lua_State* L, bool automation);   // world/party_lua.cpp
    void registerFiles(lua_State* L, bool automation);   // files_lua.cpp
    void registerAutomation(lua_State* L, bool automation);   // automation_lua.cpp

    extern HMODULE g_module;
    extern std::wstring g_modsDir;
    extern std::wstring g_gameDir;
    extern lua_State* g_lua;
    extern bool g_started;
    extern std::mutex g_running;
    extern thread_local bool t_inLua;
    extern std::atomic<ULONGLONG> g_lastFrame;
    extern int g_kernelRef;
    extern LARGE_INTEGER g_frequency;
    extern LONGLONG g_deadline;
    extern double g_budget;
    double now();
    void countHook(lua_State* L, lua_Debug*);
    void setBudget(double seconds);
    std::wstring widen(const char* s, size_t n);
    std::string narrow(const std::wstring& w);
    std::wstring modPath(const char* dir, size_t dirLen, const char* rel, size_t relLen);
    bool readFile(const std::wstring& path, std::string& out);
    extern std::atomic<int> g_levelCallState;
    extern std::atomic<int> g_levelCallResult;
    uint32_t checkId(lua_State* L, int arg);
    bool checkPosition(lua_State* L, int arg, World::Position& out);
    int pushFailure(lua_State* L, const char* error);
    bool checkSlot(lua_State* L, int index, uint32_t& slot);
    int pushResult(lua_State* L, bool ok, const char* error);
    std::string upperName(lua_State* L, int index);
    void pushFileInfo(lua_State* L, const Save::FileInfo& info);
    std::wstring heroFilePath(const char* dir, size_t dirLen, const std::string& spec);
    void pushHeroFile(lua_State* L, const std::string& spec, const Party::HeroFile& f);
    uint32_t toColor(lua_Number v);
    bool uiCommon(lua_State* L, Ui::Command& c, int layerArg);
    bool uiBox(lua_State* L, Ui::Command& c, int arg);
    bool textStyle(lua_State* L, int index, Text::Style& out);
    void pushWindow(lua_State* L, const Ui::WindowInfo& w);
    std::wstring layoutPath();
    std::wstring statePath(const wchar_t* file = L"gear_mods.json");
    std::wstring stateFile(lua_State* L, int arg);
    bool writeAtomic(const std::wstring& path, const char* text, size_t n);
    bool plainName(const char* s, size_t n);
    std::wstring scopeDir(const char* dir, size_t dirLen, const char* scope, size_t scopeLen);
    void makeDirs(const std::wstring& path);
    bool dataExtension(std::string name);
    void clearDir(const std::wstring& root);
    std::vector<uint32_t> idList(lua_State* L, int arg);
    std::wstring automationFile(const char* name, size_t n);
    bool readObjectBytes(uintptr_t address, uint8_t* out, size_t size);
}
