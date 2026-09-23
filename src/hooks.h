#pragma once

// Installs the GetProcAddress filter and the world-render call-site thunks.
// Must run on the loader thread before the client creates its D3D device.
bool InstallEngineHooks();
