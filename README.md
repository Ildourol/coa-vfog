# CoAVolFog

Volumetric fog and light shafts for the Ascension/CoA 3.3.5a (build 12340) Direct3D 9 client.

It implements the `coa-vfog-kit` plan's loader, D3D9 device wrapper with a readable depth buffer,
world-render hooks and per-pixel ray-march tier. Fog layers come from the Classic client's own
`LightDataGlobalVolumeFog` data wherever its lights cover the map, and are derived from the 3.3.5
day/night lighting elsewhere (Outland, Northrend, custom maps).

## Classic fog data

`tools/convert_classic_fog.py` converts the kit's Classic `Light`, `LightData` and
`LightDataGlobalVolumeFog` exports into `data/fogdata.bin` (625 lights, 2,079 time keys, 5,737 layers):

```powershell
python tools/convert_classic_fog.py <path>\coa-vfog-kit.zip data/fogdata.bin
```

At run time the DLL blends the Classic lights around the camera (spheres: full weight inside the
falloff start, linear to the falloff end, the map's global light takes the rest), interpolates the two
time keys around the current time, pairs layers by their layer index, and applies the Classic transforms:
density ×0.01, heights relative to the player when flag bit 1 is set, sun shadowing for flag bit 0
(a light below the horizon counts as shadow), `1 + strength·((d − start)/range)^exponent` over a 5,000-yd
fog range, and scatter intensities up to 10 in linear light. The fog is blended over the scene in linear
light, as the modern client adds its volume to a linear frame, with a per-channel highlight roll-off. The
data is Classic-derived; keep it in private repositories.

## What it draws

- **Distance haze** that thickens toward the horizon and fades with altitude, tinted by the zone's
  stock fog colour.
- **Ground mist** that hugs the terrain around the player; zones with short stock fog get more.
- **Distance fog** that replaces the stock linear fog on maps without Classic data: it turns opaque where
  the stock fog did, is lit by the direct light (warm at sunset), and fades into a horizon band on the sky.
  With Classic layers the geometry near the far clip fades into the sky column instead; the distance fog
  returns only across the edge of Classic coverage and where the Classic layers are too thin to hide the
  far clip.
- **Forward scattering** around the sun or moon (Henyey–Greenstein phase).
- **Light shafts**: in-scattering is shadowed by a screen-space march toward the light, so trees,
  buildings and terrain cast shafts into the fog.
- **God rays** (optional): a radial blur of the bright sky around the sun.

The effect is composited over the world before glow and the UI. The client's glow (`screen + g·blur²`, with
`g` from the day/night light) runs afterwards and would bleach bright fog to white, so fogged pixels are
pre-compensated with the live glow amount. Its other term, a blend toward the blur while drunk or under
water, is left as is. While the effect draws, the stock fog is pushed out of range for
the world render and restored afterwards; a frame the effect skips keeps the stock fog.

**View distance.** Ascension's Extensions.dll detours the far-clip clamp (`0x780770`) and caps maps 0, 1, 530
and 571 at 791.66 yd; the engine allows 1583.33 and instances use it. With `FarClipMax` set, the DLL's calls
to the clamp (`0x780810` when the `farclip` CVar is set, `0x781444` on map load) lift that cap. Terrain
loading, the chunk pool, the WDL horizon and the fog follow the far clip; placed objects keep their own
size-class culling (`environmentDetail`), and creatures the server's visibility distance.

## How it attaches

| Piece | Mechanism |
|---|---|
| Loader | `version.dll` proxy (all 17 exports forward lazily to the system copy). Its static import loads `CoAVolFog.dll` before the client starts. |
| D3D9 | The client resolves `Direct3DCreate9` through the delay-loaded `GetProcAddress` slot `[0xB2ED98]`. The DLL points that slot at a filter that returns a wrapped `IDirect3D9`. No d3d9 code is patched, so DXVK or other `d3d9.dll` builds keep working underneath. |
| Depth | The wrapper creates the device without auto depth and binds an `INTZ` texture as the depth-stencil, which the client caches as its world depth. MSAA is reported unavailable and forced off; `D3DCREATE_PUREDEVICE` is removed. The client draws the world with viewport depth `[0, 0.94]` (`[0xADEEE4]`, set at `0x4F9019`), the distant WDL terrain into `[0.998, 0.999]` with its own projection, and leaves the sky at the clear depth 1; the shaders read depth through the captured world viewport's range and treat anything deeper as beyond the far clip. |
| Hooks | Four 5-byte call displacements: the world render call (`0x4FB03D`, stock-fog override and restore), after the opaque M2 pass (`0x4F911D`, records the world viewport and matrices), the liquid surface pass (`0x4F9170`, depth writes forced on so water is fogged by its own distance) and before the frame effects (`0x4F9281`, renders the fog). The original bytes are checked first; on any mismatch nothing is patched. Two more retarget the far-clip clamp calls (`0x780810`, `0x781444`) when `FarClipMax` is set at start-up, independently of the fog hooks. |
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
- linear depth against the scene geometry, also with the client's world depth range `[0, 0.94]` and a
  distant-terrain patch behind it, and sky transmittance against a CPU reference integration for
  derived and Classic layers;
- Classic light blending and time-key interpolation at a known position against hand-computed values;
- temporal accumulation converging on a static camera;
- Reset at a new size and reference counts reaching zero.

It writes `before.png`, `after.png` and the debug views to `build/harness-out`.

`vfog_harness --scene harbour <dir> --data data/fogdata.bin` renders the logged in-game frame at the
Stormwind harbour (sunset, far clip 791.6 yd) with ideal depth and with the client's depth range, and
prints fog opacity and colour at probe points next to a CPU integration.

## Install

Close the client, then copy `version.dll`, `CoAVolFog.dll`, `CoAVolFog.ini` and `fogdata.bin` next to
`Ascension.exe`.
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
| `DataMode` | 1 | 1 Classic layers where available, 0 derived layers everywhere |
| `ColorSpace` | 1 | 1 scatter and blend in linear light with a highlight roll-off, 0 gamma |
| `SunScatter`, `Ambient`, `Exposure` | 1, 1, 1 | Light in the fog |
| `ClassicExposure` | 1 | Brightness of the Classic layers (1 = as authored) |
| `LightShafts` | 1 | Shadowed in-scattering |
| `GodRays` | 0 | Radial sky rays, 0 = off |
| `GlowCompensation` | 1 | Pre-compensate the fog for the client's glow |
| `FarClipMax` | 1583 | Continent view distance up to 1583 yd, within the `farclip` setting (0 = Ascension's 791 cap). Turning it on from 0 needs a restart; other changes (including 0) apply at the next `farclip` change, map load or zone change, where raising it shows a loading screen |
| `MaxDistance` | 5000 | Fog range: sky integration length and the Classic distance-curve scale |
| `Temporal` | 0.85 | History weight, 0 = off |
| `Underwater` | 0 | Keep the effect under water |
| `LiquidDepth` | 1 | Water surfaces write depth while the fog draws |
| `DebugView` | 0 | 1 radiance, 2 transmittance, 3 linear depth |
| `SunMarker` | 0 | Red dot where the light direction projects |
| `LogLevel` | 1 | 0 errors, 1 info (frame summary every 60 s, depth probe every 30 s), 2 debug |

## Status and limits

- Tested in the client with native D3D9; DXVK and Wine are untested.
- Transparent effects, particles and water are fogged by the opaque depth behind them (kit IP-B), so
  near effects in front of the sky are dimmed slightly.
- Interiors get the outdoor layers; the `gxApi d3d9ex` path is not wrapped (fog stays off there).
- Water surfaces write depth only in the outdoor liquid pass; WMO liquids (city canals) still do not.
  Pixels beyond the far clip below the horizon are marched as level rays so they meet the sky at eye level.
- The modern fog path applies no exposure or tonemap and its frame is graded with a clamp and a LUT; the
  LUT (and the modern lighting and bloom) are not reproduced, so colours still differ from Classic.
- The distance fog (`FarFog`, maps without Classic data) has no modern counterpart: it stands in for the
  stock fog up to the 3.3.5 far clip, which is far shorter than the modern client's.
- `FarClipMax` raises memory use (about four times the loaded terrain in a 32-bit process); Ascension's
  reason for the continent cap is unknown. Without the key in the INI it stays off.
- Classic data covers the lights the Classic `Light` table references (slot 0, clear weather); zone
  lights, weather/underwater/death slots and noise modulation are not used yet.
- Not implemented from the kit: the froxel pipeline (M3), fitted fog for transparents (M6), in-game CVars.

## License

GPL-2.0, see `LICENSE`. `data/fogdata.bin` is converted from WoW Classic client data and is not covered
by it.
