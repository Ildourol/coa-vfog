#include <windows.h>

extern "C" __declspec(dllimport) int __cdecl vf_loader_anchor();

namespace
{
constexpr int kExportCount = 17;
constexpr unsigned kUnresolvedExportExitCode = 0xDEAD0000u;

const char* const kExports[kExportCount] = {
    "GetFileVersionInfoA",       "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",     "GetFileVersionInfoSizeA",    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",    "GetFileVersionInfoW",
    "VerFindFileA",              "VerFindFileW",               "VerInstallFileA",
    "VerInstallFileW",           "VerLanguageNameA",           "VerLanguageNameW",
    "VerQueryValueA",            "VerQueryValueW",
};

HMODULE LoadSystemVersionDll()
{
    static HMODULE module = nullptr;
    if (!module)
    {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH - 16);
        lstrcpyA(path + n, "\\version.dll");
        module = LoadLibraryA(path);
    }
    return module;
}
}

extern "C" void* g_versionTargets[kExportCount] = {};

extern "C" void __cdecl ResolveVersionExport(int index)
{
    HMODULE real = LoadSystemVersionDll();
    void* target = real ? reinterpret_cast<void*>(GetProcAddress(real, kExports[index])) : nullptr;
    if (!target)
        ExitProcess(kUnresolvedExportExitCode | static_cast<unsigned>(index));
    InterlockedExchangePointer(&g_versionTargets[index], target);
}

#define VERSION_PROXY(i)                                          \
    extern "C" __declspec(naked) void proxy_##i()                 \
    {                                                             \
        __asm mov eax, dword ptr [g_versionTargets + 4 * i]       \
        __asm test eax, eax                                       \
        __asm jnz resolved                                        \
        __asm pushad                                              \
        __asm push i                                              \
        __asm call ResolveVersionExport                           \
        __asm add esp, 4                                          \
        __asm popad                                               \
        __asm mov eax, dword ptr [g_versionTargets + 4 * i]       \
        __asm resolved:                                           \
        __asm jmp eax                                             \
    }

VERSION_PROXY(0)
VERSION_PROXY(1)
VERSION_PROXY(2)
VERSION_PROXY(3)
VERSION_PROXY(4)
VERSION_PROXY(5)
VERSION_PROXY(6)
VERSION_PROXY(7)
VERSION_PROXY(8)
VERSION_PROXY(9)
VERSION_PROXY(10)
VERSION_PROXY(11)
VERSION_PROXY(12)
VERSION_PROXY(13)
VERSION_PROXY(14)
VERSION_PROXY(15)
VERSION_PROXY(16)

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        vf_loader_anchor();
    }
    return TRUE;
}
