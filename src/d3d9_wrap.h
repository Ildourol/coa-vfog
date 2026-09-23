#pragma once

#include "config.h"
#include "engine.h"

#include <d3d9.h>

using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);

void SetRealDirect3DCreate9(Direct3DCreate9Fn fn);
IDirect3D9* WINAPI WrappedDirect3DCreate9(UINT sdkVersion);

// Fog is only set up on devices created while it is allowed (engine hooks installed, or the test harness).
void AllowFog(bool allowed);

class FogDevice;
FogDevice* ActiveFogDevice();
// The wrapper behind the client's device pointer; the most recent fog device if the pointer is not a wrapper.
FogDevice* FindFogDevice(void* gameDevice);
bool IsWrapperOf(FogDevice* device, void* gameDevice);
IDirect3DDevice9* RealDevice(FogDevice* device);
void ForceDepthWrite(FogDevice* device, bool force);
bool RenderFog(FogDevice* device, const FrameInputs& in, const Config& cfg, const char** skipReason);
