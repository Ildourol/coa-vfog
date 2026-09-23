# CoAVolFog

Volumetric fog and light shafts for the Ascension/CoA 3.3.5a (build 12340) Direct3D 9 client.

It implements the first playable slice of the `coa-vfog-kit` plan: the development loader, the D3D9
device wrapper with a readable depth buffer, the world-render hooks, and the per-pixel ray-march tier
with layers derived from the client's live day/night lighting.

## What it draws

- **Distance haze** that thickens toward the horizon and fades with altitude, tinted by the zone's
  stock fog colour.
- **Ground mist** that hugs the terrain around the player; zones with short stock fog get more.
- **Distance fog** that replaces the stock linear fog: it turns opaque where the stock fog did, is lit by
  the direct light (warm at sunset), and fades into a horizon band on the sky.
- **Forward scattering** around the sun or moon (Henyey–Greenstein phase).
- **Light shafts**: in-scattering is shadowed by a screen-space march toward the light, so trees,
  buildings and terrain cast shafts into the fog.
- **God rays** (optional): a radial blur of the bright sky around the sun.

The effect is composited over the world before glow and the UI. While it draws, the stock fog is pushed
out of range for the world render and restored afterwards; a frame the effect skips keeps the stock fog.

## How it attaches

| Piece | Mechanism |
|---|---|
| Loader | `version.dll` proxy (all 17 exports forward lazily to the system copy). Its static import loads `CoAVolFog.dll` before the client starts. |
| D3D9 | The client resolves `Direct3DCreate9` through the delay-loaded `GetProcAddress` slot `[0xB2ED98]`. The DLL points that slot at a filter that returns a wrapped `IDirect3D9`. No d3d9 code is patched, so DXVK or other `d3d9.dll` builds keep working underneath. |
| Depth | The wrapper creates the device without auto depth and binds an `INTZ` texture as the depth-stencil, which the client caches as its world depth. MSAA is reported unavailable and forced off; `D3DCREATE_PUREDEVICE` is removed. |
| Hooks | Three 5-byte call displacements: the world render call (`0x4FB03D`, stock-fog override and restore), after the opaque M2 pass (`0x4F911D`, records the world viewport and matrices) and before the frame effects (`0x4F9281`, renders the fog). The original bytes are checked first; on any mismatch nothing is patched. |
| State | Every state the passes touch is captured with a recorded state block and restored, plus render targets, depth and stream 0 (whose offset state blocks drop). The client's shader-constant cache stays valid. |

Engine inputs (all static addresses in the 12340 image):

| Input | Address |
|---|---|
| World view / projection (camera-relative view; OpenGL depth range, converted by the D3D backend) | device `+0x1B00` stack, `+0xF88`; copies at `0xADF5E8`, `0xADF628` |
| Camera position / look-at target | `0xCD8F5C`, `0xCD8F68` |
| Day fraction | `0xD38B04` |
| Stock fog groups: colour, start, end (read by the render callbacks) | `0xD38B8C`–`0xD38B94`, `0xD38BA0`–`0xD38BA8` |
| Zone fog distance (light float band 0) | `0xD38C1C` |
| Light colours: ambient, direct, sun | `0xD38BD4`, `0xD38BD8`, `0xD38BF8` |
| Visible sun / moon sprite positions, sky centre | `0xD38E28`, `0xD38E48`, `0xD38B18` (day window `[0xA41CA4, 0xA41CA0]`) |
| Camera in liquid | `0xCD8794` |
| Far clip | From the projection; `[[0xB7436C] + 0xB14]` as a fallback |

Two readings in the kit were corrected against the disassembly: the fog end is `0xD38BA8` (the kit's
`0xD38B98` is a density-like value that Extensions.dll patches), and `0xD38C9C` is a near-constant model
lighting direction (polar angle 110–127°), not the visible sun, so shafts use the sprite positions.

## Build

Requirements: Visual Studio 2022 (C++ x86), the Windows 10/11 SDK (`fxc.exe`), CMake 3.20+.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

Outputs in `build/Release`: `version.dll`, `CoAVolFog.dll`, and `vfog_harness.exe`.

## Test

```powershell
ctest --test-dir build -C Release --output-on-failure
```

`vfog_harness` creates a real D3D9 device through the wrapper with the client's flags (`0x52`, auto
depth D24S8), renders a Z-up test scene with the client's projection convention, runs the fog passes
through the same entry the hook uses, and checks:

- device wrapping, INTZ substitution, pure-device removal and preserved engine-visible parameters;
- restoration of render, sampler, texture, shader, constant, stream, viewport, scissor and target state;
- pixels outside the world viewport (the glow sub-rectangle case) left untouched;
- linear depth against the scene geometry, and sky transmittance against a CPU reference integration;
- temporal accumulation converging on a static camera;
- Reset at a new size and reference counts reaching zero.

It writes `before.png`, `after.png` and the debug views to `build/harness-out`.

## Install

Close the client, then copy `version.dll`, `CoAVolFog.dll` and `CoAVolFog.ini` next to `Ascension.exe`.
Remove `version.dll` and `CoAVolFog.dll` to uninstall; nothing else in the client is changed. The DLL
writes `CoAVolFog.log` next to itself.

## Settings

`CoAVolFog.ini` is re-read within a second while the game runs (except `Enable` and `EngineHooks`).

| Key | Default | Meaning |
|---|---|---|
| `Enable` | 1 | Master switch (restart) |
| `EngineHooks` | 1 | Install the hooks; 0 leaves the client unmodified (restart) |
| `Quality` | 2 | 1 quarter resolution / 16 steps, 2 half / 24, 3 half / 32 |
| `Density`, `Haze`, `GroundFog`, `FarFog` | 1, 1, 0.6, 1 | Density multipliers |
| `StockFog` | 1 | 1 replaces the stock fog with the distance fog, 0 keeps it |
| `SunScatter`, `Ambient`, `Exposure` | 1, 1, 1 | Light in the fog |
| `LightShafts` | 1 | Shadowed in-scattering |
| `GodRays` | 0.35 | Radial sky rays, 0 = off |
| `MaxDistance` | 1500 | Sky column length in yards |
| `Temporal` | 0.85 | History weight, 0 = off |
| `Underwater` | 0 | Keep the effect under water |
| `DebugView` | 0 | 1 radiance, 2 transmittance, 3 linear depth |
| `SunMarker` | 0 | Red dot where the light direction projects |
| `LogLevel` | 1 | 0 errors, 1 info, 2 debug |

## Status and limits

- First in-client run (2026-09-23, native D3D9, 2560x1440): hooks, wrapper and INTZ worked, and the
  server recorded no anticheat alerts (`player_anticheat_alert` empty) with `Warden.Enabled = 1`.
  That run exposed the camera-relative view, now covered by the harness. DXVK and Wine are untested.
- Extensions.dll can report "injected DLLs" through opcode `0x51F`; the server logs and stores such
  alerts and disconnects after more than 5 in 10 s.
- Transparent effects, particles and water are fogged by the opaque depth behind them (kit IP-B), so
  near effects in front of the sky are dimmed slightly.
- Interiors get the outdoor layers; the `gxApi d3d9ex` path is not wrapped (fog stays off there).
- Not implemented from the kit: the froxel pipeline (M3), Classic-authored layer data (M5),
  fitted fog for transparents (M6), in-game CVars.
