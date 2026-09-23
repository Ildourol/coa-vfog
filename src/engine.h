#pragma once

#include <windows.h>
#include <d3d9.h>

#include <cstdint>

// Everything the renderer needs from one world frame. The projection is the engine's own
// (OpenGL-style, clip z in [-1, 1]); its D3D backend remaps depth to [0, 1] on upload.
struct FrameInputs
{
    float view[16];
    float proj[16];
    float camPos[3];
    float camTarget[3];
    D3DVIEWPORT9 viewport;
    float dayFraction;
    float toLight[3];
    bool lightIsMoon;
    uint32_t fogColor;
    uint32_t sunColor;
    uint32_t directColor;
    uint32_t ambientColor;
    float fogStart;
    float fogEnd;
    // The zone's own fog distance (light float band 0) before the far clip caps it into fogEnd.
    float zoneFogDistance;
    float farClip;
    bool inLiquid;
    int mapId;
};

namespace engine
{
constexpr uint32_t kClientTimestamp = 0x4C2452FE;

constexpr uintptr_t kGetProcAddressSlot = 0x00B2ED98;
constexpr uintptr_t kGetProcAddressThunk = 0x0041C654;

constexpr uintptr_t kWorldRenderSite = 0x004FB03D;
constexpr uintptr_t kWorldRenderTarget = 0x004F8EA0;
constexpr uintptr_t kOpaqueDoneSite = 0x004F911D;
constexpr uintptr_t kOpaqueDoneTarget = 0x00823CB0;
constexpr uintptr_t kWorldDoneSite = 0x004F9281;
constexpr uintptr_t kWorldDoneTarget = 0x008C1010;

bool IsSupportedClient();
void* GameD3DDevice();
bool CameraInLiquid();

// The stock fog's start/end in both DayNight fog groups, which the render callbacks read per draw.
struct StockFog
{
    float start[2];
    float end[2];
};
StockFog ReadStockFog();
void WriteStockFog(const StockFog& fog);

// Called right after the opaque M2 pass: records the world viewport and the bound matrices.
void CaptureOpaqueState(IDirect3DDevice9* device);
bool HasOpaqueState();
void ClearOpaqueState();

// Called before the frame effects: reads the rest of the frame state. False outside the world.
bool BuildFrameInputs(FrameInputs& out);
}
