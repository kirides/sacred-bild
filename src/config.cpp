#include "config.h"
#include "config/controller.h"
#include "config/ddraw.h"
#include "config/debug.h"
#include "config/display.h"
#include "config/launcher.h"
#include "config/net.h"
#include "config/render.h"
#include "config/screenshot.h"
#include "config/ui.h"
#include "log.h"

#include <windows.h>
#include <algorithm>

namespace
{
    std::wstring g_iniPath;

    int readInt(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int def)
    {
        return static_cast<int>(GetPrivateProfileIntW(section, key, def, ini.c_str()));
    }

    std::wstring readString(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const std::wstring& def)
    {
        wchar_t buf[1024] = {};
        GetPrivateProfileStringW(section, key, def.c_str(), buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
        return buf;
    }

    // 1/0, true/false, yes/no, on/off.
    bool readBool(const std::wstring& ini, const wchar_t* section, const wchar_t* key, bool def)
    {
        wchar_t buf[16] = {};
        GetPrivateProfileStringW(section, key, L"", buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
        for (const wchar_t* yes : {L"1", L"true", L"yes", L"on"})
        {
            if (_wcsicmp(buf, yes) == 0)
            {
                return true;
            }
        }
        for (const wchar_t* no : {L"0", L"false", L"no", L"off"})
        {
            if (_wcsicmp(buf, no) == 0)
            {
                return false;
            }
        }
        return def;
    }

    // "X,Y", each clamped to 0..4096.
    void readPosition(const std::wstring& ini, const wchar_t* key, Config::Ui::Position& pos)
    {
        const std::wstring value = readString(ini, L"UI.Layout", key, L"");
        int x = 0, y = 0;
        if (swscanf_s(value.c_str(), L"%d , %d", &x, &y) == 2)
        {
            pos = {std::clamp(x, 0, 4096), std::clamp(y, 0, 4096)};
        }
    }

    std::string ascii(const std::wstring& s)
    {
        std::string out;
        for (wchar_t c : s)
        {
            out += c < 0x80 ? static_cast<char>(c) : '?';
        }
        return out;
    }
}

const std::wstring& ConfigFile::path()
{
    return g_iniPath;
}

void ConfigFile::load(const std::wstring& gameDir)
{
    const std::wstring ini = gameDir + L"\\SacredBild.ini";
    g_iniPath = ini;
    Config::launcher.settingsWindow = !readBool(ini, L"Launcher", L"HideSettingsWindow", false);
    Config::ddraw.d3d9 = _wcsicmp(readString(ini, L"DDraw", L"Backend", L"d3d9").c_str(), L"chain") != 0;
    Config::ddraw.chain = readString(ini, L"DDraw", L"Chain", Config::ddraw.chain);
    Config::ddraw.d3d9Path = readString(ini, L"DDraw", L"D3D9", L"");
    Config::ddraw.mediaFoundation = Config::ddraw.d3d9 || readBool(ini, L"DDraw", L"MediaFoundation", Config::ddraw.mediaFoundation);
    Config::display.width = readInt(ini, L"Display", L"Width", Config::display.width);
    Config::display.height = readInt(ini, L"Display", L"Height", Config::display.height);
    const std::wstring borderless = readString(ini, L"Display", L"Borderless", L"auto");
    Config::display.frame = _wcsicmp(borderless.c_str(), L"auto") == 0 ? Config::Display::Frame::Auto
        : readBool(ini, L"Display", L"Borderless", true)      ? Config::Display::Frame::Never
                                                               : Config::Display::Frame::Always;
    Config::display.clipCursor = readInt(ini, L"Display", L"ClipCursor", Config::display.clipCursor) != 0;
    Config::display.fpsLimit = readInt(ini, L"Display", L"FpsLimit", Config::display.fpsLimit);
    Config::display.fpsLimitInactive = readInt(ini, L"Display", L"FpsLimitInactive", Config::display.fpsLimitInactive);
    Config::display.vsync = readInt(ini, L"Display", L"VSync", Config::display.vsync) != 0;
    Config::display.maxFrameLatency = readInt(ini, L"Display", L"MaxFrameLatency", Config::display.maxFrameLatency);
    Config::ui.scale = static_cast<float>(_wtof(readString(ini, L"UI", L"Scale", L"0").c_str()));
    Config::ui.scaleMenus = _wcsicmp(readString(ini, L"UI", L"ScaleMode", L"InGame").c_str(), L"Full") == 0;
    Config::ui.linearFilter = readInt(ini, L"UI", L"LinearFilter", Config::ui.linearFilter) != 0;
    Config::ui.anchor = readInt(ini, L"UI", L"Anchor", Config::ui.anchor) != 0;
    readPosition(ini, L"Taskbar", Config::ui.taskbar);
    readPosition(ini, L"Chat", Config::ui.chat);
    readPosition(ini, L"Inventory", Config::ui.inventory);
    readPosition(ini, L"Equipment", Config::ui.equipment);
    readPosition(ini, L"Stats", Config::ui.stats);
    readPosition(ini, L"Minimap", Config::ui.minimap);
    readPosition(ini, L"Portraits", Config::ui.portraits);
    readPosition(ini, L"Shops", Config::ui.shops);
    Config::render.textureBudgetMB = readInt(ini, L"Render", L"TextureBudgetMB", Config::render.textureBudgetMB);
    Config::render.batch = readInt(ini, L"Render", L"Batch", Config::render.batch) != 0;
    Config::render.batchNoClip = readInt(ini, L"Render", L"BatchNoClip", Config::render.batchNoClip) != 0;
    Config::render.batchVertexBuffer = readInt(ini, L"Render", L"BatchVertexBuffer", Config::render.batchVertexBuffer) != 0;
    Config::render.batchModels = readInt(ini, L"Render", L"BatchModels", Config::render.batchModels) != 0;
    Config::render.batchGround = readInt(ini, L"Render", L"BatchGround", Config::render.batchGround) != 0;
    Config::render.batchSprites = readInt(ini, L"Render", L"BatchSprites", Config::render.batchSprites) != 0;
    Config::render.groundMesh = readInt(ini, L"Render", L"GroundMesh", Config::render.groundMesh) != 0;
    Config::render.gpuSkinning = readInt(ini, L"Render", L"GpuSkinning", Config::render.gpuSkinning) != 0;
    Config::render.offscreenPoses = readInt(ini, L"Render", L"OffscreenPoses", Config::render.offscreenPoses);
    Config::render.asyncAnimation = readInt(ini, L"Render", L"AsyncAnimation", Config::render.asyncAnimation) != 0;
    Config::render.animationThreads = readInt(ini, L"Render", L"AnimationThreads", Config::render.animationThreads);
    Config::render.recordIndex = readInt(ini, L"Render", L"RecordIndex", Config::render.recordIndex) != 0;
    Config::render.soundLock = readInt(ini, L"Render", L"SoundLock", Config::render.soundLock) != 0;
    Config::render.fastMath = readInt(ini, L"Render", L"FastMath", Config::render.fastMath) != 0;
    Config::render.warmFileCache = readInt(ini, L"Render", L"WarmFileCache", Config::render.warmFileCache) != 0;
    Config::render.atlas = readInt(ini, L"Render", L"Atlas", Config::render.atlas) != 0;
    Config::render.atlasPageSize = readInt(ini, L"Render", L"AtlasPageSize", Config::render.atlasPageSize);
    Config::render.atlasPages = readInt(ini, L"Render", L"AtlasPages", Config::render.atlasPages);
    Config::render.atlasMaxTextureSize = readInt(ini, L"Render", L"AtlasMaxTextureSize", Config::render.atlasMaxTextureSize);
    Config::screenshot.jpeg = _wcsicmp(readString(ini, L"Screenshot", L"Format", L"png").c_str(), L"jpg") == 0;
    Config::net.relay = readInt(ini, L"Net", L"Relay", Config::net.relay) != 0;
    Config::net.port = readInt(ini, L"Net", L"Port", Config::net.port);
    Config::net.hosts = ascii(readString(ini, L"Net", L"Hosts", L""));
    Config::net.noDelay = readInt(ini, L"Net", L"NoDelay", Config::net.noDelay) != 0;
    Config::net.joinTimeout = readInt(ini, L"Net", L"JoinTimeout", Config::net.joinTimeout);
    Config::net.udp = readInt(ini, L"Net", L"Udp", Config::net.udp) != 0;
    Config::net.matchmaker = ascii(readString(ini, L"Net", L"Matchmaker", L""));
    Config::net.publish = readInt(ini, L"Net", L"Publish", Config::net.publish) != 0;
    Config::net.preferIpv6 = _wcsicmp(readString(ini, L"Net", L"Prefer", L"IPv6").c_str(), L"IPv4") != 0;
    Config::controller.enabled = readBool(ini, L"Controller", L"Enabled", Config::controller.enabled);
    Config::controller.deadzone = std::clamp(readInt(ini, L"Controller", L"Deadzone", Config::controller.deadzone), 0, 90);
    Config::controller.cursorSpeed = std::clamp(readInt(ini, L"Controller", L"CursorSpeed", Config::controller.cursorSpeed), 50, 5000);
    Config::controller.moveRadius = std::clamp(readInt(ini, L"Controller", L"MoveRadius", Config::controller.moveRadius), 40, 1000);
    Config::controller.aimRange = std::clamp(readInt(ini, L"Controller", L"AimRange", Config::controller.aimRange), 50, 3000);
    Config::controller.aimCone = std::clamp(readInt(ini, L"Controller", L"AimCone", Config::controller.aimCone), 10, 360);
    Config::controller.artClick = readBool(ini, L"Controller", L"ArtClick", Config::controller.artClick);
    Config::controller.walk = readBool(ini, L"Controller", L"Walk", Config::controller.walk);
    Config::controller.prompts = readBool(ini, L"Controller", L"Prompts", Config::controller.prompts);
    Config::debug.d3dStats = readInt(ini, L"Debug", L"D3DStats", Config::debug.d3dStats) != 0;
    Config::debug.profiler = readInt(ini, L"Debug", L"Profiler", Config::debug.profiler) != 0;
    Config::debug.profilerIntervalUs = readInt(ini, L"Debug", L"ProfilerIntervalUs", Config::debug.profilerIntervalUs);
    Config::debug.uiTrace = readInt(ini, L"Debug", L"UiTrace", Config::debug.uiTrace) != 0;
    Config::debug.crashDump = readInt(ini, L"Debug", L"CrashDump", Config::debug.crashDump);
    Config::debug.movieFallback = readInt(ini, L"Debug", L"MovieFallback", Config::debug.movieFallback) != 0;
    Config::debug.skinCheck = readInt(ini, L"Debug", L"SkinCheck", Config::debug.skinCheck) != 0;
    Config::debug.animationCheck = readInt(ini, L"Debug", L"AnimationCheck", Config::debug.animationCheck) != 0;

    LOG("Config: HideSettingsWindow={} Backend={} D3D9='{}' MediaFoundation={} Width={} Height={} Borderless={} ClipCursor={} FpsLimit={} FpsLimitInactive={} VSync={} MaxFrameLatency={} UI.Scale={} UI.ScaleMode={} UI.LinearFilter={} UI.Anchor={} TextureBudgetMB={} Batch={} "
        "BatchNoClip={} BatchVertexBuffer={} BatchModels={} BatchGround={} BatchSprites={} GroundMesh={} GpuSkinning={} OffscreenPoses={} AsyncAnimation={} AnimationThreads={} RecordIndex={} SoundLock={} FastMath={} WarmFileCache={} Atlas={} ({} px, {} pages, textures <= {}) "
        "Screenshot.Format={} Net.Relay={} Net.Port={} Net.Hosts='{}' Net.NoDelay={} Net.JoinTimeout={} Net.Udp={} Net.Matchmaker='{}' Net.Publish={} Net.Prefer={} D3DStats={} Profiler={} ({} us) UiTrace={} CrashDump={} MovieFallback={} SkinCheck={} AnimationCheck={}",
        !Config::launcher.settingsWindow, Config::ddraw.d3d9 ? "d3d9" : "chain", ascii(Config::ddraw.d3d9Path), Config::ddraw.mediaFoundation, Config::display.width, Config::display.height,
        Config::display.frame == Config::Display::Frame::Auto ? "auto" : Config::display.frame == Config::Display::Frame::Never ? "1" : "0", Config::display.clipCursor, Config::display.fpsLimit, Config::display.fpsLimitInactive,
        Config::display.vsync, Config::display.maxFrameLatency, Config::ui.scale,
        Config::ui.scaleMenus ? "Full" : "InGame", Config::ui.linearFilter, Config::ui.anchor,
        Config::render.textureBudgetMB, Config::render.batch, Config::render.batchNoClip, Config::render.batchVertexBuffer,
        Config::render.batchModels, Config::render.batchGround, Config::render.batchSprites, Config::render.groundMesh, Config::render.gpuSkinning, Config::render.offscreenPoses, Config::render.asyncAnimation, Config::render.animationThreads, Config::render.recordIndex, Config::render.soundLock, Config::render.fastMath, Config::render.warmFileCache, Config::render.atlas, Config::render.atlasPageSize, Config::render.atlasPages,
        Config::render.atlasMaxTextureSize, Config::screenshot.jpeg ? "jpg" : "png", Config::net.relay, Config::net.port, Config::net.hosts, Config::net.noDelay, Config::net.joinTimeout, Config::net.udp, Config::net.matchmaker, Config::net.publish, Config::net.preferIpv6 ? "IPv6" : "IPv4", Config::debug.d3dStats, Config::debug.profiler, Config::debug.profilerIntervalUs, Config::debug.uiTrace, Config::debug.crashDump, Config::debug.movieFallback, Config::debug.skinCheck, Config::debug.animationCheck);
    LOG("Config: Controller Enabled={} Deadzone={} CursorSpeed={} MoveRadius={} AimRange={} AimCone={} ArtClick={} Walk={} Prompts={}",
        Config::controller.enabled, Config::controller.deadzone, Config::controller.cursorSpeed, Config::controller.moveRadius,
        Config::controller.aimRange, Config::controller.aimCone, Config::controller.artClick, Config::controller.walk,
        Config::controller.prompts);
    const auto pos = [](const Config::Ui::Position& p) { return Fmt::format("{},{}", p.x, p.y); };
    LOG("Config: UI.Layout Taskbar={} Chat={} Inventory={} Equipment={} Stats={} Minimap={} Portraits={} Shops={}",
        pos(Config::ui.taskbar), pos(Config::ui.chat), pos(Config::ui.inventory), pos(Config::ui.equipment),
        pos(Config::ui.stats), pos(Config::ui.minimap), pos(Config::ui.portraits), pos(Config::ui.shops));
}
