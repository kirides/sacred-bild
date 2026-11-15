#include "instance.h"

#include <windows.h>
#include <cwctype>
#include <vector>

namespace
{
    // The command line's arguments (quotes honoured; no need for shell32's CommandLineToArgvW).
    std::vector<std::wstring> arguments()
    {
        std::vector<std::wstring> out;
        const wchar_t* p = GetCommandLineW();
        while (p && *p)
        {
            while (*p == L' ' || *p == L'\t')
            {
                ++p;
            }
            if (!*p)
            {
                break;
            }
            std::wstring arg;
            bool quoted = false;
            for (; *p && (quoted || (*p != L' ' && *p != L'\t')); ++p)
            {
                if (*p == L'"')
                {
                    quoted = !quoted;
                }
                else
                {
                    arg.push_back(*p);
                }
            }
            out.push_back(arg);
        }
        return out;
    }

    std::wstring envVar(const wchar_t* name)
    {
        wchar_t value[512] = {};
        const DWORD n = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
        return n && n < std::size(value) ? std::wstring(value, n) : std::wstring();
    }

    struct State
    {
        bool automation = false;
        std::string name;
    };

    State compute()
    {
        State s;
        s.automation = envVar(L"SACREDGEAR_AUTO") == L"1";
        std::wstring requested = envVar(L"SACREDGEAR_INSTANCE");
        const std::vector<std::wstring> args = arguments();
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (_wcsicmp(args[i].c_str(), L"-gear-auto") == 0)
            {
                s.automation = true;
            }
            else if (_wcsnicmp(args[i].c_str(), L"-gear-instance=", 15) == 0)
            {
                requested = args[i].substr(15);
            }
        }
        if (!s.automation)
        {
            return s;
        }
        for (const wchar_t c : requested)
        {
            if (s.name.size() >= 48)
            {
                break;
            }
            s.name.push_back((c < 0x80 && (iswalnum(c) || c == L'_' || c == L'-' || c == L'.')) ? static_cast<char>(c) : '_');
        }
        if (s.name.empty() || s.name[0] == '.')
        {
            s.name = "pid" + std::to_string(GetCurrentProcessId());
        }
        return s;
    }

    const State& state()
    {
        static const State s = compute();
        return s;
    }

    std::wstring widen(const std::string& s)
    {
        return std::wstring(s.begin(), s.end());    // ASCII only
    }
}

bool Instance::automation()
{
    return state().automation;
}

const std::string& Instance::name()
{
    return state().name;
}

std::wstring Instance::directory(const std::wstring& gameDir)
{
    if (!automation())
    {
        return {};
    }
    const std::wstring base = gameDir + L"\\GearAutomation";
    CreateDirectoryW(base.c_str(), nullptr);
    const std::wstring dir = base + L"\\" + widen(name());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring Instance::logPath(const std::wstring& gameDir, const wchar_t* file)
{
    return (automation() ? directory(gameDir) : gameDir) + L"\\" + file;
}
