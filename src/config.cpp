#include "config.h"
#include "log.h"

#include <windows.h>
#include <algorithm>

Config g_config;

namespace
{
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
    void readPosition(const std::wstring& ini, const wchar_t* key, Config::UiPosition& pos)
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

void ConfigFile::load(const std::wstring& gameDir)
{
    const std::wstring ini = gameDir + L"\\SacredBild.ini";
    g_config.ddrawD3D9 = _wcsicmp(readString(ini, L"DDraw", L"Backend", L"d3d9").c_str(), L"chain") != 0;
    g_config.ddrawChain = readString(ini, L"DDraw", L"Chain", g_config.ddrawChain);
    g_config.mediaFoundation = g_config.ddrawD3D9 || readBool(ini, L"DDraw", L"MediaFoundation", g_config.mediaFoundation);
    g_config.width = readInt(ini, L"Display", L"Width", g_config.width);
    g_config.height = readInt(ini, L"Display", L"Height", g_config.height);
    g_config.borderless = readInt(ini, L"Display", L"Borderless", g_config.borderless) != 0;
    g_config.fpsLimit = readInt(ini, L"Display", L"FpsLimit", g_config.fpsLimit);
    g_config.vsync = readInt(ini, L"Display", L"VSync", g_config.vsync) != 0;
    g_config.maxFrameLatency = readInt(ini, L"Display", L"MaxFrameLatency", g_config.maxFrameLatency);
    g_config.uiScale = static_cast<float>(_wtof(readString(ini, L"UI", L"Scale", L"0").c_str()));
    g_config.uiScaleMenus = _wcsicmp(readString(ini, L"UI", L"ScaleMode", L"InGame").c_str(), L"Full") == 0;
    g_config.uiLinearFilter = readInt(ini, L"UI", L"LinearFilter", g_config.uiLinearFilter) != 0;
    g_config.uiAnchor = readInt(ini, L"UI", L"Anchor", g_config.uiAnchor) != 0;
    readPosition(ini, L"Taskbar", g_config.uiTaskbar);
    readPosition(ini, L"Chat", g_config.uiChat);
    readPosition(ini, L"Inventory", g_config.uiInventory);
    readPosition(ini, L"Equipment", g_config.uiEquipment);
    readPosition(ini, L"Stats", g_config.uiStats);
    readPosition(ini, L"Minimap", g_config.uiMinimap);
    readPosition(ini, L"Portraits", g_config.uiPortraits);
    readPosition(ini, L"Shops", g_config.uiShops);
    g_config.textureBudgetMB = readInt(ini, L"Render", L"TextureBudgetMB", g_config.textureBudgetMB);
    g_config.batch = readInt(ini, L"Render", L"Batch", g_config.batch) != 0;
    g_config.batchNoClip = readInt(ini, L"Render", L"BatchNoClip", g_config.batchNoClip) != 0;
    g_config.batchVertexBuffer = readInt(ini, L"Render", L"BatchVertexBuffer", g_config.batchVertexBuffer) != 0;
    g_config.batchModels = readInt(ini, L"Render", L"BatchModels", g_config.batchModels) != 0;
    g_config.asyncAnimation = readInt(ini, L"Render", L"AsyncAnimation", g_config.asyncAnimation) != 0;
    g_config.recordIndex = readInt(ini, L"Render", L"RecordIndex", g_config.recordIndex) != 0;
    g_config.atlas = readInt(ini, L"Render", L"Atlas", g_config.atlas) != 0;
    g_config.atlasPageSize = readInt(ini, L"Render", L"AtlasPageSize", g_config.atlasPageSize);
    g_config.atlasPages = readInt(ini, L"Render", L"AtlasPages", g_config.atlasPages);
    g_config.atlasMaxTextureSize = readInt(ini, L"Render", L"AtlasMaxTextureSize", g_config.atlasMaxTextureSize);
    g_config.screenshotJpeg = _wcsicmp(readString(ini, L"Screenshot", L"Format", L"png").c_str(), L"jpg") == 0;
    g_config.netRelay = readInt(ini, L"Net", L"Relay", g_config.netRelay) != 0;
    g_config.netPort = readInt(ini, L"Net", L"Port", g_config.netPort);
    g_config.netHosts = ascii(readString(ini, L"Net", L"Hosts", L""));
    g_config.netNoDelay = readInt(ini, L"Net", L"NoDelay", g_config.netNoDelay) != 0;
    g_config.netJoinTimeout = readInt(ini, L"Net", L"JoinTimeout", g_config.netJoinTimeout);
    g_config.d3dStats = readInt(ini, L"Debug", L"D3DStats", g_config.d3dStats) != 0;
    g_config.profiler = readInt(ini, L"Debug", L"Profiler", g_config.profiler) != 0;
    g_config.profilerIntervalUs = readInt(ini, L"Debug", L"ProfilerIntervalUs", g_config.profilerIntervalUs);
    g_config.uiTrace = readInt(ini, L"Debug", L"UiTrace", g_config.uiTrace) != 0;

    LOG("Config: Backend={} MediaFoundation={} Width={} Height={} Borderless={} FpsLimit={} VSync={} MaxFrameLatency={} UI.Scale={} UI.ScaleMode={} UI.LinearFilter={} UI.Anchor={} TextureBudgetMB={} Batch={} "
        "BatchNoClip={} BatchVertexBuffer={} BatchModels={} AsyncAnimation={} RecordIndex={} Atlas={} ({} px, {} pages, textures <= {}) "
        "Screenshot.Format={} Net.Relay={} Net.Port={} Net.Hosts='{}' Net.NoDelay={} Net.JoinTimeout={} D3DStats={} Profiler={} ({} us) UiTrace={}",
        g_config.ddrawD3D9 ? "d3d9" : "chain", g_config.mediaFoundation, g_config.width, g_config.height, g_config.borderless, g_config.fpsLimit,
        g_config.vsync, g_config.maxFrameLatency, g_config.uiScale,
        g_config.uiScaleMenus ? "Full" : "InGame", g_config.uiLinearFilter, g_config.uiAnchor,
        g_config.textureBudgetMB, g_config.batch, g_config.batchNoClip, g_config.batchVertexBuffer,
        g_config.batchModels, g_config.asyncAnimation, g_config.recordIndex, g_config.atlas, g_config.atlasPageSize, g_config.atlasPages,
        g_config.atlasMaxTextureSize, g_config.screenshotJpeg ? "jpg" : "png", g_config.netRelay, g_config.netPort, g_config.netHosts, g_config.netNoDelay, g_config.netJoinTimeout, g_config.d3dStats, g_config.profiler, g_config.profilerIntervalUs, g_config.uiTrace);
    const auto pos = [](const Config::UiPosition& p) { return std::format("{},{}", p.x, p.y); };
    LOG("Config: UI.Layout Taskbar={} Chat={} Inventory={} Equipment={} Stats={} Minimap={} Portraits={} Shops={}",
        pos(g_config.uiTaskbar), pos(g_config.uiChat), pos(g_config.uiInventory), pos(g_config.uiEquipment),
        pos(g_config.uiStats), pos(g_config.uiMinimap), pos(g_config.uiPortraits), pos(g_config.uiShops));
}
