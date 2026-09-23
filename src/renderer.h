#pragma once

#include "config.h"
#include "engine.h"

#include <d3d9.h>

class Renderer
{
public:
    ~Renderer();

    // Default-pool resources must go before IDirect3DDevice9::Reset.
    void ReleaseDefaultPool();
    void ReleaseAll();

    // Composites fog onto the currently bound render target. The depth surface must be bound
    // as the device's depth-stencil and is sampled through depthTexture.
    bool Render(IDirect3DDevice9* dev, IDirect3DTexture9* depthTexture, IDirect3DSurface9* depthSurface,
                const FrameInputs& in, const Config& cfg);

    const char* LastSkipReason() const { return m_skip; }

private:
    bool EnsureShaders(IDirect3DDevice9* dev);
    bool EnsureStateBlock(IDirect3DDevice9* dev);
    bool EnsureTargets(IDirect3DDevice9* dev, UINT lowW, UINT lowH, UINT rayW, UINT rayH);
    bool Skip(const char* reason);
    void DrawFullscreen(IDirect3DDevice9* dev);
    void BindTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex, bool linear);
    bool RenderPasses(IDirect3DDevice9* dev, IDirect3DTexture9* depthTexture, IDirect3DSurface9* target,
                      const D3DSURFACE_DESC& depthDesc, const FrameInputs& in, const Config& cfg);

    IDirect3DVertexShader9* m_vs = nullptr;
    IDirect3DPixelShader9* m_march[3] = {};
    IDirect3DPixelShader9* m_temporal = nullptr;
    IDirect3DPixelShader9* m_composite = nullptr;
    IDirect3DPixelShader9* m_rayMask = nullptr;
    IDirect3DPixelShader9* m_rayBlur = nullptr;
    IDirect3DVertexDeclaration9* m_decl = nullptr;
    IDirect3DStateBlock9* m_state = nullptr;

    IDirect3DTexture9* m_marchTarget = nullptr;
    IDirect3DTexture9* m_history[2] = {};
    IDirect3DTexture9* m_rays[2] = {};
    UINT m_lowW = 0;
    UINT m_lowH = 0;
    UINT m_rayW = 0;
    UINT m_rayH = 0;
    bool m_fogFilterable = false;

    int m_historyIndex = 0;
    bool m_historyValid = false;
    float m_prevView[16] = {};
    float m_prevProj[16] = {};
    float m_prevCam[3] = {};
    D3DVIEWPORT9 m_prevViewport = {};
    UINT m_prevScale = 0;
    long long m_prevTicks = 0;
    unsigned m_frame = 0;
    unsigned m_logged = 0;
    const char* m_skip = "";
};
