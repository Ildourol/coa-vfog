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

constexpr float kStockFogAway = 50000.0f;

uintptr_t g_worldRenderTarget = engine::kWorldRenderTarget;
uintptr_t g_opaqueDoneTarget = engine::kOpaqueDoneTarget;
uintptr_t g_liquidSurfaceTarget = engine::kLiquidSurfaceTarget;
FogDevice* g_liquidDevice = nullptr;
uintptr_t g_worldDoneTarget = engine::kWorldDoneTarget;
bool g_failed = false;
bool g_renderedLastFrame = false;
bool g_renderedThisFrame = false;
bool g_stockFogPushed = false;
engine::StockFog g_savedStockFog = {};
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

// The volumetric fog replaces the stock fog only while it is actually drawing, so a frame it
// skips keeps the client's own fog.
void OnFrameBegin()
{
    g_renderedThisFrame = false;
    const Config& cfg = GlobalConfig().Get();
    if (g_failed || !g_renderedLastFrame || cfg.stockFog != 1 || engine::CameraInLiquid() || !GameFogDevice())
        return;
    g_savedStockFog = engine::ReadStockFog();
    engine::StockFog pushed;
    for (int i = 0; i < 2; ++i)
    {
        pushed.start[i] = kStockFogAway;
        pushed.end[i] = kStockFogAway * 2.0f;
    }
    engine::WriteStockFog(pushed);
    g_stockFogPushed = true;
}

// Water writes no depth in the stock client; while the fog draws, the liquid surface pass writes it so
// water is fogged by its own distance instead of by the sea floor or the sky behind it.
void OnLiquidBegin()
{
    if (g_failed || !g_renderedLastFrame || !GlobalConfig().Get().liquidDepth)
        return;
    g_liquidDevice = GameFogDevice();
    ForceDepthWrite(g_liquidDevice, true);
}

void OnLiquidEnd()
{
    ForceDepthWrite(g_liquidDevice, false);
    g_liquidDevice = nullptr;
}

void OnFrameEnd()
{
    OnLiquidEnd();
    if (g_stockFogPushed)
    {
        engine::WriteStockFog(g_savedStockFog);
        g_stockFogPushed = false;
    }
    g_renderedLastFrame = g_renderedThisFrame;
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
    g_renderedThisFrame = rendered;
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

extern "C" void __cdecl vf_on_frame_begin()
{
    __try
    {
        OnFrameBegin();
    }
    __except (GuardFilter(GetExceptionCode(), "frame begin hook"))
    {
        g_failed = true;
    }
}

extern "C" void __cdecl vf_on_frame_end()
{
    __try
    {
        OnFrameEnd();
    }
    __except (GuardFilter(GetExceptionCode(), "frame end hook"))
    {
        g_failed = true;
    }
}

extern "C" void __cdecl vf_on_liquid_begin()
{
    __try
    {
        OnLiquidBegin();
    }
    __except (GuardFilter(GetExceptionCode(), "liquid begin hook"))
    {
        g_failed = true;
    }
}

extern "C" void __cdecl vf_on_liquid_end()
{
    __try
    {
        OnLiquidEnd();
    }
    __except (GuardFilter(GetExceptionCode(), "liquid end hook"))
    {
        g_failed = true;
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

// 0x4FB03D: the world render, thiscall on the world frame with no stack arguments.
__declspec(naked) static void WorldRenderThunk()
{
    __asm {
        push ecx
        call vf_on_frame_begin
        pop ecx
        call dword ptr [g_worldRenderTarget]
        pushad
        call vf_on_frame_end
        popad
        ret
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

// 0x4F9170: the liquid surface pass (no arguments), outside liquid only.
__declspec(naked) static void LiquidSurfaceThunk()
{
    __asm {
        pushad
        call vf_on_liquid_begin
        popad
        call dword ptr [g_liquidSurfaceTarget]
        pushad
        call vf_on_liquid_end
        popad
        ret
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

namespace
{
struct CallSite
{
    uintptr_t site;
    uintptr_t target;
    const void* thunk;
};
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
    const CallSite sites[] = {
        {engine::kWorldRenderSite, engine::kWorldRenderTarget, &WorldRenderThunk},
        {engine::kOpaqueDoneSite, engine::kOpaqueDoneTarget, &OpaqueDoneThunk},
        {engine::kLiquidSurfaceSite, engine::kLiquidSurfaceTarget, &LiquidSurfaceThunk},
        {engine::kWorldDoneSite, engine::kWorldDoneTarget, &WorldDoneThunk},
    };
    for (const CallSite& s : sites)
        if (!SiteMatches(s.site, s.target))
        {
            VF_LOG_ERROR("world render call site 0x%08X differs from the 12340 client; hooks not installed",
                         static_cast<unsigned>(s.site));
            return false;
        }
    int patched = 0;
    for (const CallSite& s : sites)
    {
        if (!PatchCallSite(s.site, s.target, s.thunk))
        {
            while (patched-- > 0)
                PatchCallSite(sites[patched].site, reinterpret_cast<uintptr_t>(sites[patched].thunk),
                              reinterpret_cast<const void*>(sites[patched].target));
            return false;
        }
        ++patched;
    }
    *slot = &GetProcAddressFilter;
    VF_LOG_INFO("engine hooks installed: GetProcAddress filter, world render 0x%08X, opaque 0x%08X, liquid 0x%08X, "
                "world done 0x%08X",
                static_cast<unsigned>(engine::kWorldRenderSite), static_cast<unsigned>(engine::kOpaqueDoneSite),
                static_cast<unsigned>(engine::kLiquidSurfaceSite), static_cast<unsigned>(engine::kWorldDoneSite));
    return true;
}