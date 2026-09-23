#pragma once

// Installs the GetProcAddress filter and the world-render call-site thunks.
// Must run on the loader thread before the client creates its D3D device.
bool InstallEngineHooks();

// Retargets the two calls of the far-clip clamp so FarClipMax can lift Extensions.dll's continent cap.
// Only when FarClipMax is set at load; independent of the fog hooks.
void InstallFarClipHooks();
