#include "hooks.h"

#include "config.h"
#include "d3d9_wrap.h"
#include "engine.h"
#include "log.h"

#include <windows.h>

#include <cstring>

namespace
{
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);

uintptr_t g_opaqueDoneTarget = engine::kOpaqueDoneTarget;
uintptr_t g_worldDoneTarget = engine::kWorldDoneTarget;
bool g_failed = false;
bool g_deviceChecked = false;
unsigned g_skipsLogged = 0;
DWORD g_lastReload = 0;
const char* g_lastSkip = "";

FARPROC WINAPI GetProcAddressFilter(HMODULE module, LPCSTR name)
{
    FARPROC proc = GetProcAddress(module, name);
    if (!proc || !name || IS_INTRESOURCE(name))
        return proc;
    if (std::strcmp(name, "Direct3DCreate9") == 0)
    {
        HMODULE pinned = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCSTR>(proc), &pinned);
        SetRealDirect3DCreate9(reinterpret_cast<Direct3DCreate9Fn>(proc));
        VF_LOG_INFO("Direct3DCreate9 resolved at %p; wrapping", reinterpret_cast<void*>(proc));
        return reinterpret_cast<FARPROC>(&WrappedDirect3DCreate9);
    }
    if (std::strcmp(name, "Direct3DCreate9Ex") == 0)
        VF_LOG_INFO("Direct3DCreate9Ex requested; the D3D9Ex path is not wrapped (fog unavailable with gxApi d3d9ex)");
    return proc;
}

FogDevice* GameFogDevice()
{
    void* game = engine::GameD3DDevice();
    FogDevice* device = FindFogDevice(game);
    if (!g_deviceChecked && device)
    {
        g_deviceChecked = true;
        if (!IsWrapperOf(device, game))
            VF_LOG_ERROR("the client's D3D device %p is not a fog wrapper; using the latest fog device", game);
    }
    return device;
}

void OnOpaqueDone()
{
    FogDevice* device = g_failed ? nullptr : GameFogDevice();
    if (!device)
        return;
    engine::CaptureOpaqueState(RealDevice(device));
}

void OnWorldDone()
{
    FogDevice* device = g_failed ? nullptr : GameFogDevice();
    if (!device || !engine::HasOpaqueState())
    {
        engine::ClearOpaqueState();
        return;
    }

    DWORD now = GetTickCount();
    if (now - g_lastReload > 1000)
    {
        g_lastReload = now;
        GlobalConfig().ReloadIfChanged();
    }

    FrameInputs in = {};
    bool valid = engine::BuildFrameInputs(in);
    engine::ClearOpaqueState();
    const Config& cfg = GlobalConfig().Get();
    const char* skip = "invalid frame inputs";
    bool rendered = false;
    if (valid && (!in.inLiquid || cfg.underwater))
        rendered = RenderFog(device, in, cfg, &skip);
    else if (valid)
        skip = "camera under liquid";
    if (!rendered && skip != g_lastSkip && g_skipsLogged < 50)
    {
        ++g_skipsLogged;
        VF_LOG_INFO("fog skipped: %s", skip);
    }
    g_lastSkip = rendered ? "" : skip;
}

int GuardFilter(unsigned code, const char* where)
{
    VF_LOG_ERROR("exception 0x%08X in %s; fog disabled for this session", code, where);
    return EXCEPTION_EXECUTE_HANDLER;
}

bool PatchCallSite(uintptr_t site, uintptr_t expectedTarget, const void* thunk)
{
    auto* bytes = reinterpret_cast<unsigned char*>(site);
    int32_t rel;
    std::memcpy(&rel, bytes + 1, sizeof(rel));
    if (bytes[0] != 0xE8 || site + 5 + rel != expectedTarget)
    {
        VF_LOG_ERROR("call site 0x%08X does not match (E8 -> 0x%08X expected); hooks not installed",
                     static_cast<unsigned>(site), static_cast<unsigned>(expectedTarget));
        return false;
    }
    int32_t newRel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(thunk) - (site + 5));
    DWORD old;
    if (!VirtualProtect(bytes + 1, sizeof(newRel), PAGE_EXECUTE_READWRITE, &old))
        return false;
    std::memcpy(bytes + 1, &newRel, sizeof(newRel));
    VirtualProtect(bytes + 1, sizeof(newRel), old, &old);
    FlushInstructionCache(GetCurrentProcess(), bytes, 5);
    return true;
}

bool SiteMatches(uintptr_t site, uintptr_t expectedTarget)
{
    auto* bytes = reinterpret_cast<const unsigned char*>(site);
    int32_t rel;
    std::memcpy(&rel, bytes + 1, sizeof(rel));
    return bytes[0] == 0xE8 && site + 5 + rel == expectedTarget;
}
}

extern "C" void __cdecl vf_on_opaque_done()
{
    __try
    {
        OnOpaqueDone();
    }
    __except (GuardFilter(GetExceptionCode(), "opaque hook"))
    {
        g_failed = true;
    }
}

extern "C" void __cdecl vf_on_world_done()
{
    __try
    {
        OnWorldDone();
    }
    __except (GuardFilter(GetExceptionCode(), "world hook"))
    {
        g_failed = true;
    }
}

// 0x4F911D: thiscall M2 pass with one stack argument (ret 4). ECX is passed through untouched.
__declspec(naked) static void OpaqueDoneThunk()
{
    __asm {
        push dword ptr [esp + 4]
        call dword ptr [g_opaqueDoneTarget]
        pushad
        call vf_on_opaque_done
        popad
        ret 4
    }
}

// 0x4F9281: FFX end, no arguments. Runs the fog before the glow and screen effects, then tail-calls it.
__declspec(naked) static void WorldDoneThunk()
{
    __asm {
        pushad
        call vf_on_world_done
        popad
        jmp dword ptr [g_worldDoneTarget]
    }
}

bool InstallEngineHooks()
{
    auto* slot = reinterpret_cast<GetProcAddressFn*>(engine::kGetProcAddressSlot);
    uintptr_t current = reinterpret_cast<uintptr_t>(*slot);
    if (current != engine::kGetProcAddressThunk)
    {
        VF_LOG_ERROR("GetProcAddress slot holds 0x%08X (expected the loader thunk 0x%08X); hooks not installed",
                     static_cast<unsigned>(current), static_cast<unsigned>(engine::kGetProcAddressThunk));
        return false;
    }
    if (!SiteMatches(engine::kOpaqueDoneSite, engine::kOpaqueDoneTarget) ||
        !SiteMatches(engine::kWorldDoneSite, engine::kWorldDoneTarget))
    {
        VF_LOG_ERROR("world render call sites differ from the 12340 client; hooks not installed");
        return false;
    }
    if (!PatchCallSite(engine::kOpaqueDoneSite, engine::kOpaqueDoneTarget, &OpaqueDoneThunk))
        return false;
    if (!PatchCallSite(engine::kWorldDoneSite, engine::kWorldDoneTarget, &WorldDoneThunk))
    {
        PatchCallSite(engine::kOpaqueDoneSite, reinterpret_cast<uintptr_t>(&OpaqueDoneThunk),
                      reinterpret_cast<const void*>(engine::kOpaqueDoneTarget));
        return false;
    }
    *slot = &GetProcAddressFilter;
    VF_LOG_INFO("engine hooks installed: GetProcAddress filter, opaque 0x%08X, world 0x%08X",
                static_cast<unsigned>(engine::kOpaqueDoneSite), static_cast<unsigned>(engine::kWorldDoneSite));
    return true;
}
