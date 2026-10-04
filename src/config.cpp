#include "config.h"
#include "log.h"

#include <windows.h>

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
    g_config.ddrawChain = readString(ini, L"DDraw", L"Chain", g_config.ddrawChain);
    g_config.width = readInt(ini, L"Display", L"Width", g_config.width);
    g_config.height = readInt(ini, L"Display", L"Height", g_config.height);
    g_config.borderless = readInt(ini, L"Display", L"Borderless", g_config.borderless) != 0;
    g_config.fpsLimit = readInt(ini, L"Display", L"FpsLimit", g_config.fpsLimit);
    g_config.uiScale = static_cast<float>(_wtof(readString(ini, L"UI", L"Scale", L"0").c_str()));
    g_config.uiLinearFilter = readInt(ini, L"UI", L"LinearFilter", g_config.uiLinearFilter) != 0;
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
    g_config.netRelay = readInt(ini, L"Net", L"Relay", g_config.netRelay) != 0;
    g_config.netPort = readInt(ini, L"Net", L"Port", g_config.netPort);
    g_config.netHosts = ascii(readString(ini, L"Net", L"Hosts", L""));
    g_config.d3dStats = readInt(ini, L"Debug", L"D3DStats", g_config.d3dStats) != 0;
    g_config.profiler = readInt(ini, L"Debug", L"Profiler", g_config.profiler) != 0;
    g_config.profilerIntervalUs = readInt(ini, L"Debug", L"ProfilerIntervalUs", g_config.profilerIntervalUs);

    LOG("Config: Width={} Height={} Borderless={} FpsLimit={} UI.Scale={} UI.LinearFilter={} TextureBudgetMB={} Batch={} "
        "BatchNoClip={} BatchVertexBuffer={} BatchModels={} AsyncAnimation={} RecordIndex={} Atlas={} ({} px, {} pages, textures <= {}) "
        "Net.Relay={} Net.Port={} Net.Hosts='{}' D3DStats={} Profiler={} ({} us)",
        g_config.width, g_config.height, g_config.borderless, g_config.fpsLimit, g_config.uiScale, g_config.uiLinearFilter,
        g_config.textureBudgetMB, g_config.batch, g_config.batchNoClip, g_config.batchVertexBuffer,
        g_config.batchModels, g_config.asyncAnimation, g_config.recordIndex, g_config.atlas, g_config.atlasPageSize, g_config.atlasPages,
        g_config.atlasMaxTextureSize, g_config.netRelay, g_config.netPort, g_config.netHosts, g_config.d3dStats, g_config.profiler, g_config.profilerIntervalUs);
}
