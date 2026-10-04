#include "patch.h"
#include "log.h"

#include <windows.h>
#include <detours/detours.h>
#include <cstring>

bool Patch::write(uintptr_t addr, const void* data, size_t size)
{
    DWORD old = 0;
    auto* p = reinterpret_cast<void*>(addr);
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old))
    {
        LOG("Patch: VirtualProtect failed at {:08x}", addr);
        return false;
    }
    std::memcpy(p, data, size);
    VirtualProtect(p, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, size);
    return true;
}

bool Patch::verify(uintptr_t addr, std::initializer_list<uint8_t> expected)
{
    if (std::memcmp(reinterpret_cast<const void*>(addr), expected.begin(), expected.size()) == 0)
    {
        return true;
    }
    LOG("Patch: unexpected bytes at {:08x}", addr);
    return false;
}

bool Patch::imm32(uintptr_t addr, uint32_t expected, uint32_t value)
{
    uint32_t cur = 0;
    std::memcpy(&cur, reinterpret_cast<const void*>(addr), 4);
    if (cur != expected)
    {
        LOG("Patch: imm32 at {:08x} is {:x}, expected {:x}", addr, cur, expected);
        return false;
    }
    return write(addr, &value, 4);
}

bool Patch::redirectCall(uintptr_t callSite, const void* target)
{
    if (*reinterpret_cast<const uint8_t*>(callSite) != 0xE8)
    {
        LOG("Patch: no call instruction at {:08x}", callSite);
        return false;
    }
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (callSite + 5));
    return write(callSite + 1, &rel, 4);
}

void* Patch::iat(const char* dll, const char* function, void* replacement)
{
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    // Imports by ordinal (Winsock in Sacred) carry no name: those slots are matched by the address they are bound to.
    const HMODULE module = GetModuleHandleA(dll);
    const auto bound = module ? reinterpret_cast<ULONG_PTR>(GetProcAddress(module, function)) : 0;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp)
    {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0)
        {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
        {
            const bool match = IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)
                ? bound && slots->u1.Function == bound
                : std::strcmp(reinterpret_cast<const char*>(
                      reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name), function) == 0;
            if (match)
            {
                void* old = reinterpret_cast<void*>(slots->u1.Function);
                write(reinterpret_cast<uintptr_t>(&slots->u1.Function), &replacement, sizeof(replacement));
                return old;
            }
        }
    }
    LOG("Patch: import {}!{} not found", dll, function);
    return nullptr;
}

void Patch::begin()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
}

bool Patch::hook(void** original, void* detour, const char* name)
{
    const LONG err = DetourAttach(original, detour);
    if (err != NO_ERROR)
    {
        LOG("Patch: DetourAttach({}) failed: {}", name, err);
        return false;
    }
    return true;
}

bool Patch::commit()
{
    const LONG err = DetourTransactionCommit();
    if (err != NO_ERROR)
    {
        LOG("Patch: DetourTransactionCommit failed: {}", err);
        return false;
    }
    return true;
}
