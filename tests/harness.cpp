// Offline check of CoAVolFog on a real D3D9 device: device wrapping, INTZ substitution, the fog passes,
// state restoration, depth linearisation, viewport clipping and Reset. Writes PNG captures to --out.
#include "config.h"
#include "engine.h"
#include "fog_data.h"
#include "fog_model.h"

#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" __declspec(dllimport) IDirect3D9* __cdecl vf_test_wrap_direct3d9(IDirect3D9*(WINAPI*)(UINT), UINT);
extern "C" __declspec(dllimport) void __cdecl vf_test_set_config(const Config*);
extern "C" __declspec(dllimport) int __cdecl vf_test_render(const FrameInputs*, const char**);
extern "C" __declspec(dllimport) void __cdecl vf_test_force_depth_write(int);

namespace
{
constexpr D3DFORMAT kIntz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
constexpr float kPi = 3.14159265f;
constexpr float kNear = 0.4f;
constexpr float kFar = 1000.0f;
constexpr float kFovY = 0.9f;

int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

struct Vec3
{
    float x, y, z;
};

Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

// Game-like world coordinates, so camera-relative rendering is exercised at a realistic distance and height.
constexpr Vec3 kWorldOffset = {-9100.0f, -100.0f, 80.0f};
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Norm(Vec3 a)
{
    float l = std::sqrt(Dot(a, a));
    return {a.x / l, a.y / l, a.z / l};
}

// World (Z up) -> view (x right, y up, z forward), row vectors. Like the client, the view is camera-relative:
// it has no translation, and geometry is drawn with the camera position subtracted.
void LookAt(Vec3 eye, Vec3 at, float* m)
{
    Vec3 f = Norm(Sub(at, eye));
    Vec3 r = Norm(Cross(f, {0, 0, 1}));
    Vec3 u = Cross(r, f);
    float v[16] = {r.x, u.x, f.x, 0, r.y, u.y, f.y, 0, r.z, u.z, f.z, 0, 0, 0, 0, 1};
    std::memcpy(m, v, sizeof(v));
}

// The client's projection builder (0x6BF370): OpenGL depth range, w = view z.
void EngineProjectionFrom(float ys, float aspect, float zn, float zf, float* m)
{
    float p[16] = {ys / aspect, 0, 0, 0, 0, ys, 0, 0, 0, 0, (zf + zn) / (zf - zn), 1,
                   0, 0, -2.0f * zf * zn / (zf - zn), 0};
    std::memcpy(m, p, sizeof(p));
}

void EngineProjection(float aspect, float* m)
{
    EngineProjectionFrom(1.0f / std::tan(kFovY * 0.5f), aspect, kNear, kFar, m);
}

// The D3D backend's upload of the same projection: depth remapped to [0, 1].
void D3DProjection(const float* engine, float* m)
{
    std::memcpy(m, engine, sizeof(float) * 16);
    m[10] = (1.0f + engine[10]) * 0.5f;
    m[14] = engine[14] * 0.5f;
}

struct SceneVertex
{
    float x, y, z;
    DWORD color;
};

void AddQuad(std::vector<SceneVertex>& v, Vec3 a, Vec3 b, Vec3 c, Vec3 d, DWORD color, Vec3 offset = kWorldOffset)
{
    for (Vec3 p : {a, b, c, a, c, d})
    {
        Vec3 w = Add(p, offset);
        v.push_back({w.x, w.y, w.z, color});
    }
}

void AddBox(std::vector<SceneVertex>& v, Vec3 lo, Vec3 hi, DWORD color)
{
    Vec3 p[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                 {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    AddQuad(v, p[0], p[1], p[5], p[4], color);
    AddQuad(v, p[1], p[2], p[6], p[5], color);
    AddQuad(v, p[2], p[3], p[7], p[6], color);
    AddQuad(v, p[3], p[0], p[4], p[7], color);
    AddQuad(v, p[4], p[5], p[6], p[7], color);
}

std::vector<SceneVertex> BuildScene()
{
    std::vector<SceneVertex> v;
    AddQuad(v, {-2000, -2000, 0}, {2000, -2000, 0}, {2000, 2000, 0}, {-2000, 2000, 0}, 0xFF4F6B3A);
    for (int i = 0; i < 14; ++i)
    {
        float x = 40.0f + i * 22.0f;
        float y = (i % 2 ? -1.0f : 1.0f) * (8.0f + (i * 7) % 30);
        AddBox(v, {x, y, 0}, {x + 4, y + 4, 25.0f + (i * 13) % 30}, 0xFF6E5A48);
    }
    AddBox(v, {600, -300, 0}, {640, 300, 90}, 0xFF5A5A62);
    return v;
}

bool SavePng(const std::wstring& path, UINT w, UINT h, const std::vector<unsigned char>& bgra)
{
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = SUCCEEDED(factory->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(w, h));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    ok = ok && SUCCEEDED(frame->SetPixelFormat(&format)) &&
         SUCCEEDED(frame->WritePixels(h, w * 4, static_cast<UINT>(bgra.size()), const_cast<BYTE*>(bgra.data()))) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    if (frame)
        frame->Release();
    if (encoder)
        encoder->Release();
    if (stream)
        stream->Release();
    factory->Release();
    return ok;
}

struct Image
{
    UINT w = 0, h = 0;
    std::vector<unsigned char> bgra;

    const unsigned char* At(UINT x, UINT y) const { return &bgra[(static_cast<size_t>(y) * w + x) * 4]; }
    float Luma(UINT x, UINT y) const
    {
        const unsigned char* p = At(x, y);
        return (0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2]) / 255.0f;
    }
};

Image Capture(IDirect3DDevice9* dev)
{
    Image img;
    IDirect3DSurface9* bb = nullptr;
    IDirect3DSurface9* sys = nullptr;
    dev->GetRenderTarget(0, &bb);
    D3DSURFACE_DESC desc;
    bb->GetDesc(&desc);
    dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    dev->GetRenderTargetData(bb, sys);
    D3DLOCKED_RECT lr;
    sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    img.w = desc.Width;
    img.h = desc.Height;
    img.bgra.resize(static_cast<size_t>(img.w) * img.h * 4);
    for (UINT y = 0; y < img.h; ++y)
    {
        std::memcpy(&img.bgra[static_cast<size_t>(y) * img.w * 4], static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch,
                    img.w * 4);
        for (UINT x = 0; x < img.w; ++x)
            img.bgra[(static_cast<size_t>(y) * img.w + x) * 4 + 3] = 255;
    }
    sys->UnlockRect();
    sys->Release();
    bb->Release();
    return img;
}

// Covers every texture stage and pixel constant the fog passes touch (renderer.cpp kStages, kPixelConstants).
constexpr DWORD kSentinelStages = 4;
constexpr UINT kSentinelConstants = 36;

struct Sentinel
{
    DWORD renderStates[10];
    DWORD samplers[kSentinelStages][4];
    IDirect3DBaseTexture9* textures[kSentinelStages];
    IDirect3DVertexShader9* vs;
    IDirect3DPixelShader9* ps;
    IDirect3DVertexDeclaration9* decl;
    IDirect3DVertexBuffer9* stream;
    UINT streamOffset, streamStride;
    float constants[kSentinelConstants * 4];
    D3DVIEWPORT9 viewport;
    RECT scissor;
    IDirect3DSurface9* rt;
    IDirect3DSurface9* ds;
};

const D3DRENDERSTATETYPE kSentinelStates[10] = {
    D3DRS_ZENABLE,  D3DRS_ZWRITEENABLE,      D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,  D3DRS_DESTBLEND,
    D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_FOGENABLE, D3DRS_STENCILENABLE,
};
const D3DSAMPLERSTATETYPE kSentinelSamplers[4] = {D3DSAMP_ADDRESSU, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
                                                  D3DSAMP_MIPFILTER};

void ReadSentinel(IDirect3DDevice9* dev, Sentinel& s)
{
    std::memset(&s, 0, sizeof(s));
    for (int i = 0; i < 10; ++i)
        dev->GetRenderState(kSentinelStates[i], &s.renderStates[i]);
    for (DWORD t = 0; t < kSentinelStages; ++t)
    {
        for (int i = 0; i < 4; ++i)
            dev->GetSamplerState(t, kSentinelSamplers[i], &s.samplers[t][i]);
        dev->GetTexture(t, &s.textures[t]);
    }
    dev->GetVertexShader(&s.vs);
    dev->GetPixelShader(&s.ps);
    dev->GetVertexDeclaration(&s.decl);
    dev->GetStreamSource(0, &s.stream, &s.streamOffset, &s.streamStride);
    dev->GetPixelShaderConstantF(0, s.constants, kSentinelConstants);
    dev->GetViewport(&s.viewport);
    dev->GetScissorRect(&s.scissor);
    dev->GetRenderTarget(0, &s.rt);
    dev->GetDepthStencilSurface(&s.ds);
}

void ReleaseSentinel(Sentinel& s)
{
    IUnknown* refs[] = {s.textures[0], s.textures[1], s.textures[2], s.textures[3], s.vs, s.ps, s.decl, s.stream, s.rt, s.ds};
    for (IUnknown* r : refs)
        if (r)
            r->Release();
}

void ReportSentinelDifferences(const Sentinel& a, const Sentinel& b)
{
    for (int i = 0; i < 10; ++i)
        if (a.renderStates[i] != b.renderStates[i])
            std::printf("     render state %d: %lu -> %lu\n", kSentinelStates[i], a.renderStates[i], b.renderStates[i]);
    for (DWORD t = 0; t < kSentinelStages; ++t)
    {
        for (int i = 0; i < 4; ++i)
            if (a.samplers[t][i] != b.samplers[t][i])
                std::printf("     sampler %d state %d: %lu -> %lu\n", t, kSentinelSamplers[i], a.samplers[t][i],
                            b.samplers[t][i]);
        if (a.textures[t] != b.textures[t])
            std::printf("     texture %d: %p -> %p\n", t, static_cast<void*>(a.textures[t]),
                        static_cast<void*>(b.textures[t]));
    }
    if (a.vs != b.vs || a.ps != b.ps || a.decl != b.decl)
        std::printf("     shaders/decl: vs %p->%p ps %p->%p decl %p->%p\n", static_cast<void*>(a.vs),
                    static_cast<void*>(b.vs), static_cast<void*>(a.ps), static_cast<void*>(b.ps),
                    static_cast<void*>(a.decl), static_cast<void*>(b.decl));
    if (a.stream != b.stream || a.streamOffset != b.streamOffset || a.streamStride != b.streamStride)
        std::printf("     stream 0: %p+%u/%u -> %p+%u/%u\n", static_cast<void*>(a.stream), a.streamOffset,
                    a.streamStride, static_cast<void*>(b.stream), b.streamOffset, b.streamStride);
    for (UINT i = 0; i < kSentinelConstants * 4; ++i)
        if (a.constants[i] != b.constants[i])
        {
            std::printf("     pixel constant c%d differs\n", i / 4);
            break;
        }
    if (std::memcmp(&a.viewport, &b.viewport, sizeof(a.viewport)) != 0)
        std::printf("     viewport %lu,%lu %lux%lu -> %lu,%lu %lux%lu\n", a.viewport.X, a.viewport.Y, a.viewport.Width,
                    a.viewport.Height, b.viewport.X, b.viewport.Y, b.viewport.Width, b.viewport.Height);
    if (std::memcmp(&a.scissor, &b.scissor, sizeof(a.scissor)) != 0)
        std::printf("     scissor differs\n");
    if (a.rt != b.rt || a.ds != b.ds)
        std::printf("     targets: rt %p->%p ds %p->%p\n", static_cast<void*>(a.rt), static_cast<void*>(b.rt),
                    static_cast<void*>(a.ds), static_cast<void*>(b.ds));
}

bool SameSentinel(const Sentinel& a, const Sentinel& b)
{
    return std::memcmp(a.renderStates, b.renderStates, sizeof(a.renderStates)) == 0 &&
           std::memcmp(a.samplers, b.samplers, sizeof(a.samplers)) == 0 &&
           std::memcmp(a.textures, b.textures, sizeof(a.textures)) == 0 && a.vs == b.vs && a.ps == b.ps &&
           a.decl == b.decl && a.stream == b.stream && a.streamOffset == b.streamOffset &&
           a.streamStride == b.streamStride && std::memcmp(a.constants, b.constants, sizeof(a.constants)) == 0 &&
           std::memcmp(&a.viewport, &b.viewport, sizeof(a.viewport)) == 0 &&
           std::memcmp(&a.scissor, &b.scissor, sizeof(a.scissor)) == 0 && a.rt == b.rt && a.ds == b.ds;
}

struct Harness
{
    HWND window = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* dev = nullptr;
    D3DPRESENT_PARAMETERS pp = {};
    std::vector<SceneVertex> scene = BuildScene();
    IDirect3DTexture9* dummyTexture = nullptr;
    IDirect3DVertexBuffer9* dummyBuffer = nullptr;
    IDirect3DVertexShader9* engineVs = nullptr;
    IDirect3DPixelShader9* enginePs = nullptr;
    IDirect3DVertexDeclaration9* engineDecl = nullptr;
    D3DCOLOR clearColor = 0xFF6FA0DC;

    void CreateEngineObjects()
    {
        dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &dummyTexture, nullptr);
        dev->CreateVertexBuffer(256, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &dummyBuffer, nullptr);
        static const D3DVERTEXELEMENT9 elements[] = {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
            D3DDECL_END(),
        };
        dev->CreateVertexDeclaration(elements, &engineDecl);
    }

    void ReleaseEngineObjects()
    {
        if (dummyTexture)
            dummyTexture->Release();
        if (dummyBuffer)
            dummyBuffer->Release();
        if (engineDecl)
            engineDecl->Release();
        dummyTexture = nullptr;
        dummyBuffer = nullptr;
        engineDecl = nullptr;
    }

    void BeginFrame()
    {
        dev->BeginScene();
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        for (DWORD t = 0; t < kSentinelStages; ++t)
            dev->SetTexture(t, nullptr);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, clearColor, 1.0f, 0);
    }

    void DrawScene(Vec3 eye, const float* view, const float* engineProj, const D3DVIEWPORT9& vp)
    {
        float d3dProj[16];
        D3DProjection(engineProj, d3dProj);
        D3DMATRIX identity = {};
        identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
        identity._41 = -eye.x;
        identity._42 = -eye.y;
        identity._43 = -eye.z;
        dev->SetTransform(D3DTS_WORLD, &identity);
        dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(view));
        dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(d3dProj));
        dev->SetViewport(&vp);
        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetTexture(0, nullptr);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(scene.size() / 3), scene.data(),
                             sizeof(SceneVertex));
    }

    // A pretransformed quad written at a raw depth value, over the full [0, 1] depth range.
    void DrawScreenQuad(float x0, float y0, float x1, float y1, float depth)
    {
        struct ScreenVertex
        {
            float x, y, z, rhw;
            DWORD color;
        };
        const ScreenVertex quad[6] = {
            {x0, y0, depth, 1, 0xFF808890}, {x1, y0, depth, 1, 0xFF808890}, {x0, y1, depth, 1, 0xFF808890},
            {x1, y0, depth, 1, 0xFF808890}, {x1, y1, depth, 1, 0xFF808890}, {x0, y1, depth, 1, 0xFF808890},
        };
        D3DVIEWPORT9 vp = {};
        dev->GetViewport(&vp);
        D3DVIEWPORT9 full = vp;
        full.MinZ = 0.0f;
        full.MaxZ = 1.0f;
        dev->SetViewport(&full);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(ScreenVertex));
        dev->SetViewport(&vp);
    }

    void SetEngineState(const D3DVIEWPORT9& vp)
    {
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_DESTCOLOR);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
        dev->SetRenderState(D3DRS_FOGENABLE, TRUE);
        dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        for (DWORD t = 0; t < kSentinelStages; ++t)
        {
            dev->SetTexture(t, dummyTexture);
            dev->SetSamplerState(t, D3DSAMP_ADDRESSU, D3DTADDRESS_MIRROR);
            dev->SetSamplerState(t, D3DSAMP_MAGFILTER, D3DTEXF_ANISOTROPIC);
            dev->SetSamplerState(t, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(t, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        }
        float constants[kSentinelConstants * 4];
        for (UINT i = 0; i < kSentinelConstants * 4; ++i)
            constants[i] = 0.25f * i - 3.0f;
        dev->SetPixelShaderConstantF(0, constants, kSentinelConstants);
        dev->SetVertexDeclaration(engineDecl);
        dev->SetStreamSource(0, dummyBuffer, 16, 16);
        dev->SetViewport(&vp);
        RECT scissor = {3, 5, 700, 400};
        dev->SetScissorRect(&scissor);
    }
};

FrameInputs MakeInputs(const float* view, const float* proj, Vec3 eye, Vec3 at, const D3DVIEWPORT9& vp)
{
    FrameInputs in = {};
    std::memcpy(in.view, view, sizeof(in.view));
    std::memcpy(in.proj, proj, sizeof(in.proj));
    in.camPos[0] = eye.x;
    in.camPos[1] = eye.y;
    in.camPos[2] = eye.z;
    Vec3 f = Norm(Sub(at, eye));
    in.camTarget[0] = eye.x + f.x * 6;
    in.camTarget[1] = eye.y + f.y * 6;
    in.camTarget[2] = eye.z - 6;
    in.viewport = vp;
    in.dayFraction = 0.75f;
    Vec3 sun = Norm({1.0f, 0.12f, 0.16f});
    in.toLight[0] = sun.x;
    in.toLight[1] = sun.y;
    in.toLight[2] = sun.z;
    in.lightIsMoon = false;
    in.fogColor = 0xFF9DB2C8;
    in.sunColor = 0xFFFFD9A0;
    in.directColor = 0xFFE8D8C0;
    in.ambientColor = 0xFF404858;
    in.fogStart = 150.0f;
    in.fogEnd = 600.0f;
    in.zoneFogDistance = 600.0f;
    in.farClip = kFar;
    in.inLiquid = false;
    in.mapId = -1;
    return in;
}

float SmoothStep(float e0, float e1, float x)
{
    float t = std::fmin(std::fmax((x - e0) / (e1 - e0), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// CPU transmittance of the march for one pixel of a sky ray (no occluders, shafts disabled).
float ReferenceSkyTransmittance(const FrameInputs& in, const Config& cfg, const AuthoredFog* authored, float px,
                                float py, int steps, float jitter)
{
    FogParams fog = BuildFogParams(in, cfg, authored);
    const float* P = in.proj;
    const D3DVIEWPORT9& vp = in.viewport;
    float ndcX = (px - vp.X) / vp.Width * 2.0f - 1.0f;
    float ndcY = 1.0f - (py - vp.Y) / vp.Height * 2.0f;
    Vec3 ray = {(ndcX - P[8]) / P[0], (ndcY - P[9]) / P[5], 1.0f};
    float len = std::sqrt(Dot(ray, ray));
    Vec3 v = {ray.x / len, ray.y / len, ray.z / len};
    const float* m = in.view;
    Vec3 dirW = {v.x * m[0] + v.y * m[1] + v.z * m[2], v.x * m[4] + v.y * m[5] + v.z * m[6],
                 v.x * m[8] + v.y * m[9] + v.z * m[10]};
    float z = fog.maxDistance;
    z = z + (fog.maxDistance - z) * SmoothStep(fog.horizonStart, fog.farClip, z);
    float tMax = std::fmin(z * len, fog.maxDistance);
    double tau = 0.0;
    for (int s = 0; s < steps; ++s)
    {
        float u0 = static_cast<float>(s) / steps;
        float u1 = u0 + 1.0f / steps;
        float ta = tMax * u0 * u0;
        float tb = tMax * u1 * u1;
        float dt = tb - ta;
        float t = ta + (tb - ta) * jitter;
        float h = in.camPos[2] + dirW.z * t;
        auto clamp01 = [](float x) { return std::fmin(std::fmax(x, 0.0f), 1.0f); };
        for (const FogLayer& l : fog.layers)
        {
            float scale = std::exp(-std::fmax(dirW.z, 0.0f) * l.skyFalloff);
            float cover = clamp01((t - l.start) / std::fmax(dt, 1e-3f)) * clamp01((l.limit - ta) / std::fmax(dt, 1e-3f));
            float curve = 1.0f + l.strength * std::pow(std::fmin(std::fmax(t - l.start, 0.0f) / fog.maxDistance, 1.0f) +
                                                           1e-6f,
                                                       l.exponent);
            float heightF = std::fmin(std::exp((l.upperHeight - h) * l.upperFalloff), 1.0f) *
                            std::fmin(std::exp((h - l.lowerHeight) * l.lowerFalloff), 1.0f);
            tau += l.density * scale * dt * cover * curve * heightF;
        }
    }
    return static_cast<float>(std::exp(-tau));
}

const AuthoredLayer* FarWall(const AuthoredFog& fog)
{
    for (int i = 0; i < fog.layerCount; ++i)
        if (fog.layers[i].start >= 1000.0f)
            return &fog.layers[i];
    return nullptr;
}

const AuthoredLayer* MidHaze(const AuthoredFog& fog)
{
    for (int i = 0; i < fog.layerCount; ++i)
        if (fog.layers[i].start < 1.0f && std::fabs(fog.layers[i].g - 0.5f) < 0.01f)
            return &fog.layers[i];
    return nullptr;
}

float WeightOf(const AuthoredFog& fog, uint32_t light)
{
    for (int i = 0; i < fog.lightCount; ++i)
        if (fog.lightIds[i] == light)
            return fog.lightWeights[i];
    return 0.0f;
}

void CheckClassicData(const FogData& data)
{
    // Stormwind harbour, 18:00: Classic light 1 (params 7748) with the edge of light 77 (params 7472).
    const float harbour[3] = {-8565.21f, 993.46f, 104.96f};
    AuthoredFog fog = {};
    bool ok = data.Resolve(0, harbour, 0.75f, 0, fog);
    float w1 = WeightOf(fog, 1);
    float w77 = WeightOf(fog, 77);
    const AuthoredLayer* wall = FarWall(fog);
    const AuthoredLayer* haze = MidHaze(fog);
    std::printf("     harbour at 18:00: lights 1:%.3f 77:%.3f, %d layers, far wall density %.4f, haze density %.4f\n",
                w1, w77, fog.layerCount, wall ? wall->density : -1.0f, haze ? haze->density : -1.0f);
    Check(ok && fog.layerCount == 3 && std::fabs(w1 + w77 - 1.0f) < 1e-4f && w77 > 0.05f && w77 < 0.1f,
          "Classic light blend at the harbour (light 77 falloff edge)");
    Check(wall && haze && std::fabs(wall->start - 3000.0f) < 0.5f &&
              std::fabs(wall->density - (w1 * 0.6f + w77 * 0.1f)) < 1e-4f &&
              std::fabs(haze->density - (w1 * 0.12f + w77 * 0.1f)) < 1e-4f,
          "Classic layers blended by light weight at a key time");

    AuthoredFog mid = {};
    data.Resolve(0, harbour, 2250.0f / 2880.0f, 0, mid);
    float expected = WeightOf(mid, 1) * 0.8f + WeightOf(mid, 77) * 0.3f;
    const AuthoredLayer* midWall = FarWall(mid);
    std::printf("     harbour at 18:45: far wall density %.4f (expected %.4f)\n", midWall ? midWall->density : -1.0f,
                expected);
    Check(midWall && std::fabs(midWall->density - expected) < 1e-4f, "Classic keys interpolated between 18:00 and 19:30");

    AuthoredFog none = {};
    Check(!data.Resolve(530, harbour, 0.75f, 0, none), "maps without Classic lights fall back to derived layers");
}

int Run(const std::wstring& outDir, const std::string& dataPath)
{
    FogData classic;
    Check(classic.Load(dataPath), "Classic fog data loads");
    CheckClassicData(classic);

    CreateDirectoryW(outDir.c_str(), nullptr);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"vfog_harness";
    RegisterClassW(&wc);
    Harness h;
    h.window = CreateWindowW(L"vfog_harness", L"vfog", WS_OVERLAPPEDWINDOW, 0, 0, 1280, 720, nullptr, nullptr,
                             wc.hInstance, nullptr);

    HMODULE d3d9 = LoadLibraryA("d3d9.dll");
    auto realCreate = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(d3d9, "Direct3DCreate9"));
    h.d3d = vf_test_wrap_direct3d9(realCreate, D3D_SDK_VERSION);
    Check(h.d3d != nullptr, "wrapped Direct3DCreate9");
    if (!h.d3d)
        return 1;

    D3DMULTISAMPLE_TYPE ms = D3DMULTISAMPLE_4_SAMPLES;
    Check(h.d3d->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE, ms, nullptr) ==
              D3DERR_NOTAVAILABLE,
          "multisampling reported unavailable while fog is enabled");

    h.pp.Windowed = TRUE;
    h.pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    h.pp.BackBufferWidth = 1280;
    h.pp.BackBufferHeight = 720;
    h.pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    h.pp.EnableAutoDepthStencil = TRUE;
    h.pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    h.pp.hDeviceWindow = h.window;
    h.pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    DWORD engineFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE | D3DCREATE_FPU_PRESERVE;
    Check(engineFlags == 0x52, "harness uses the client's device flags");
    HRESULT hr = h.d3d->CreateDevice(0, D3DDEVTYPE_HAL, h.window, engineFlags, &h.pp, &h.dev);
    Check(SUCCEEDED(hr) && h.dev, "CreateDevice through the wrapper");
    if (!h.dev)
        return 1;

    D3DDEVICE_CREATION_PARAMETERS cp;
    h.dev->GetCreationParameters(&cp);
    Check((cp.BehaviorFlags & D3DCREATE_PUREDEVICE) == 0, "pure-device flag removed");
    Check(h.pp.EnableAutoDepthStencil == TRUE && h.pp.AutoDepthStencilFormat == D3DFMT_D24S8,
          "engine-visible depth parameters preserved");
    IDirect3DSurface9* depth = nullptr;
    h.dev->GetDepthStencilSurface(&depth);
    D3DSURFACE_DESC depthDesc = {};
    if (depth)
        depth->GetDesc(&depthDesc);
    Check(depth && depthDesc.Format == kIntz && depthDesc.Width == 1280, "INTZ depth bound as the device depth");
    if (depth)
        depth->Release();
    IDirect3D9* parent = nullptr;
    h.dev->GetDirect3D(&parent);
    Check(parent == h.d3d, "GetDirect3D returns the wrapper");
    if (parent)
        parent->Release();

    h.CreateEngineObjects();
    const float aspect = 1280.0f / 688.0f;
    const D3DVIEWPORT9 world = {0, 0, 1280, 688, 0.0f, 1.0f};
    float proj[16];
    EngineProjection(aspect, proj);
    Vec3 eye = Add({0, 0, 9}, kWorldOffset);
    Vec3 at = Add({100, 2, 4}, kWorldOffset);
    float view[16];

    Config cfg = {};
    cfg.maxDistance = 5000.0f;
    vf_test_set_config(&cfg);

    Image before;
    Image after;
    bool allRendered = true;
    bool statesKept = true;
    const char* skip = "";
    for (int frame = 0; frame < 12; ++frame)
    {
        Vec3 e = {eye.x + frame * 0.6f, eye.y, eye.z};
        LookAt(e, at, view);
        h.BeginFrame();
        h.DrawScene(e, view, proj, world);
        if (frame == 11)
            before = Capture(h.dev);
        h.SetEngineState(world);
        Sentinel s0;
        ReadSentinel(h.dev, s0);
        FrameInputs in = MakeInputs(view, proj, e, at, world);
        allRendered = vf_test_render(&in, &skip) != 0 && allRendered;
        Sentinel s1;
        ReadSentinel(h.dev, s1);
        if (!SameSentinel(s0, s1))
        {
            if (statesKept)
                ReportSentinelDifferences(s0, s1);
            statesKept = false;
        }
        ReleaseSentinel(s0);
        ReleaseSentinel(s1);
        if (frame == 11)
            after = Capture(h.dev);
        h.dev->EndScene();
        h.dev->Present(nullptr, nullptr, nullptr, nullptr);
    }
    Check(allRendered, (std::string("fog rendered on every frame ") + skip).c_str());
    Check(statesKept, "render, sampler, shader, constant, stream, viewport, scissor and target state restored");

    bool outsideKept = true;
    for (UINT y = 688; y < 720 && outsideKept; ++y)
        for (UINT x = 0; x < 1280; ++x)
            if (std::memcmp(before.At(x, y), after.At(x, y), 3) != 0)
            {
                outsideKept = false;
                break;
            }
    Check(outsideKept, "pixels outside the world viewport untouched");

    double diff = 0;
    for (UINT y = 0; y < 688; ++y)
        for (UINT x = 0; x < 1280; ++x)
            diff += std::fabs(after.Luma(x, y) - before.Luma(x, y));
    diff /= 1280.0 * 688.0;
    std::printf("     mean luma change inside the world viewport: %.4f\n", diff);
    Check(diff > 0.01 && diff < 0.5, "fog visibly changes the world viewport");

    float sunDir[3];
    {
        FrameInputs in = MakeInputs(view, proj, eye, at, world);
        const float* v = view;
        float s[3] = {in.toLight[0] * v[0] + in.toLight[1] * v[4] + in.toLight[2] * v[8],
                      in.toLight[0] * v[1] + in.toLight[1] * v[5] + in.toLight[2] * v[9],
                      in.toLight[0] * v[2] + in.toLight[1] * v[6] + in.toLight[2] * v[10]};
        std::memcpy(sunDir, s, sizeof(s));
    }
    UINT sunX = static_cast<UINT>((sunDir[0] / sunDir[2] * proj[0] * 0.5f + 0.5f) * 1280.0f);
    UINT sunY = static_cast<UINT>((0.5f - sunDir[1] / sunDir[2] * proj[5] * 0.5f) * 688.0f);
    UINT awayX = sunX > 640 ? 60 : 1220;
    float haloGain = after.Luma(sunX, sunY - 40) - before.Luma(sunX, sunY - 40);
    float awayGain = after.Luma(awayX, sunY - 40) - before.Luma(awayX, sunY - 40);
    std::printf("     sky luma change near the sun %.3f, away from the sun %.3f (sun at %u,%u)\n", haloGain, awayGain,
                sunX, sunY);
    Check(haloGain > awayGain, "forward scattering brightens the sky around the sun");

    SavePng(outDir + L"\\before.png", before.w, before.h, before.bgra);
    SavePng(outDir + L"\\after.png", after.w, after.h, after.bgra);

    {
        // The linear composite: zero fog leaves the world viewport unchanged, and fogged pixels equal
        // encode(RollOff(scene * T + L)) rebuilt from the radiance and transmittance debug views. The temporal
        // filter converges each run so the march jitter averages out.
        LookAt(eye, at, view);
        auto convergeGlow = [&](const Config& c, float glow) {
            vf_test_set_config(&c);
            Image img;
            for (int frame = 0; frame < 16; ++frame)
            {
                h.BeginFrame();
                h.DrawScene(eye, view, proj, world);
                FrameInputs in = MakeInputs(view, proj, eye, at, world);
                in.glow = glow;
                vf_test_render(&in, &skip);
                if (frame == 15)
                    img = Capture(h.dev);
                h.dev->EndScene();
            }
            return img;
        };
        auto converge = [&](const Config& c) { return convergeGlow(c, 0.0f); };
        h.BeginFrame();
        h.DrawScene(eye, view, proj, world);
        Image scene = Capture(h.dev);
        h.dev->EndScene();

        Config c = cfg;
        c.godRays = 0.0f;
        c.temporal = 0.85f;
        Config none = c;
        none.density = 0.0f;
        Image unfogged = converge(none);
        int maxDiff = 0;
        for (UINT y = 0; y < 688; ++y)
            for (UINT x = 0; x < 1280; ++x)
                for (int ch = 0; ch < 3; ++ch)
                    maxDiff = std::max(maxDiff, std::abs(unfogged.At(x, y)[ch] - scene.At(x, y)[ch]));
        std::printf("     zero fog: largest channel change %d/255\n", maxDiff);
        Check(maxDiff <= 1, "zero fog leaves the world viewport unchanged (linear composite)");

        Config radianceCfg = c;
        radianceCfg.debugView = 1;
        Image radiance = converge(radianceCfg);
        Config transmittanceCfg = c;
        transmittanceCfg.debugView = 2;
        Image transmittance = converge(transmittanceCfg);
        Image composited = converge(c);
        auto boxMean = [](const Image& img, UINT cx, UINT cy, int ch) {
            double sum = 0.0;
            for (UINT y = cy - 3; y <= cy + 3; ++y)
                for (UINT x = cx - 3; x <= cx + 3; ++x)
                    sum += img.At(x, y)[ch];
            return static_cast<float>(sum / 49.0 / 255.0);
        };
        auto rollOff = [](float x, float knee) {
            float span = std::fmax(1.0f - knee, 1e-4f);
            return x <= knee ? x : knee + span * (1.0f - std::exp(-(x - knee) / span));
        };
        const UINT points[3][2] = {{101, 101}, {640, 330}, {640, 600}};
        float worst = 0.0f;
        for (const auto& p : points)
            for (int ch = 0; ch < 3; ++ch)
            {
                float s = std::pow(boxMean(scene, p[0], p[1], ch), 2.2f);
                float l = std::pow(boxMean(radiance, p[0], p[1], ch), 2.2f);
                float t = boxMean(transmittance, p[0], p[1], 2);
                float expected = std::pow(rollOff(s * t + l, std::fmax(0.8f, s)), 1.0f / 2.2f);
                worst = std::fmax(worst, std::fabs(boxMean(composited, p[0], p[1], ch) - expected));
            }
        std::printf("     linear composite vs scene * T + L: largest difference %.1f/255\n", worst * 255.0f);
        Check(worst < 4.0f / 255.0f, "fog blends over the scene in linear light");

        // With the client's glow (screen + g * blur^2) running afterwards, fogged pixels are pre-inverted so that
        // c' + g c'^2 lands on the composited colour c; the glow amount comes from the frame inputs.
        const float glow = 0.65f;
        Image compensated = convergeGlow(c, glow);
        float glowWorst = 0.0f;
        for (const auto& p : points)
            for (int ch = 0; ch < 3; ++ch)
            {
                float cc = boxMean(composited, p[0], p[1], ch);
                float a = 1.0f - boxMean(transmittance, p[0], p[1], 2);
                float solved = (std::sqrt(1.0f + 4.0f * glow * cc) - 1.0f) / (2.0f * glow);
                float expected = cc + (solved - cc) * a;
                glowWorst = std::fmax(glowWorst, std::fabs(boxMean(compensated, p[0], p[1], ch) - expected));
            }
        Config noGlow = c;
        noGlow.glowCompensation = false;
        Image uncompensated = convergeGlow(noGlow, glow);
        float offDiff = 0.0f;
        for (const auto& p : points)
            for (int ch = 0; ch < 3; ++ch)
                offDiff = std::fmax(offDiff, std::fabs(boxMean(uncompensated, p[0], p[1], ch) -
                                                       boxMean(composited, p[0], p[1], ch)));
        std::printf("     glow compensation: largest difference %.1f/255; switched off %.1f/255\n", glowWorst * 255.0f,
                    offDiff * 255.0f);
        Check(glowWorst < 4.0f / 255.0f && offDiff < 1.0f / 255.0f,
              "fog is pre-compensated for the client's glow, and GlowCompensation 0 turns it off");

        Config gamma = c;
        gamma.colorSpace = 0;
        Image gammaImage = converge(gamma);
        double gammaDiff = 0.0;
        for (UINT y = 0; y < 688; ++y)
            for (UINT x = 0; x < 1280; ++x)
                gammaDiff += std::fabs(gammaImage.Luma(x, y) - scene.Luma(x, y));
        gammaDiff /= 1280.0 * 688.0;
        std::printf("     gamma mode: mean luma change %.4f\n", gammaDiff);
        Check(gammaDiff > 0.01 && gammaDiff < 0.5, "gamma mode (ColorSpace 0) still blends the fog");
        vf_test_set_config(&cfg);
    }

    // wdlDepth > 0 also draws a patch in the top-left corner at that raw depth, as the client draws its distant
    // WDL terrain into [0.998, 0.999] behind the world.
    auto renderDebugIn = [&](int mode, float maxDist, const D3DVIEWPORT9& vp, float wdlDepth) {
        Config c = cfg;
        c.debugView = mode;
        c.maxDistance = maxDist;
        c.temporal = 0.0f;
        vf_test_set_config(&c);
        LookAt(eye, at, view);
        h.BeginFrame();
        h.DrawScene(eye, view, proj, vp);
        if (wdlDepth > 0.0f)
            h.DrawScreenQuad(0.0f, 0.0f, 200.0f, 100.0f, wdlDepth);
        FrameInputs in = MakeInputs(view, proj, eye, at, vp);
        in.farClip = std::fmin(in.farClip, maxDist);
        vf_test_render(&in, &skip);
        Image img = Capture(h.dev);
        h.dev->EndScene();
        h.dev->Present(nullptr, nullptr, nullptr, nullptr);
        return img;
    };
    auto renderDebug = [&](int mode, float maxDist) { return renderDebugIn(mode, maxDist, world, 0.0f); };

    Image radiance = renderDebug(1, 5000.0f);
    SavePng(outDir + L"\\debug-radiance.png", radiance.w, radiance.h, radiance.bgra);
    Image transmittance = renderDebug(2, 5000.0f);
    SavePng(outDir + L"\\debug-transmittance.png", transmittance.w, transmittance.h, transmittance.bgra);

    {
        Config c = cfg;
        c.lightShafts = false;
        vf_test_set_config(&c);
        Config saved = cfg;
        cfg = c;
        Image t = renderDebug(2, 5000.0f);
        cfg = saved;
        LookAt(eye, at, view);
        FrameInputs in = MakeInputs(view, proj, eye, at, world);
        const UINT samples[3][2] = {{101, 101}, {1181, 101}, {101, 201}};
        bool match = true;
        for (const auto& s : samples)
        {
            float lo = 1.0f;
            float hi = 0.0f;
            for (int j = 0; j < 16; ++j)
            {
                float r = ReferenceSkyTransmittance(in, c, nullptr, s[0] + 0.5f, s[1] + 0.5f, 24, (j + 0.5f) / 16.0f);
                lo = std::fmin(lo, r);
                hi = std::fmax(hi, r);
            }
            float got = t.At(s[0], s[1])[2] / 255.0f;
            std::printf("     sky transmittance at %u,%u: shader %.3f, reference %.3f..%.3f\n", s[0], s[1], got, lo, hi);
            match = match && got > lo - 0.02f && got < hi + 0.02f;
        }
        Check(match, "sky transmittance matches the CPU reference (world-space reconstruction)");

        UINT farRow = 0;
        float farZ = 0.0f;
        for (UINT py = 0; py < 688 && !farRow; ++py)
        {
            float ndcY = 1.0f - (py + 0.5f) / 688.0f * 2.0f;
            Vec3 ray = {0.0f, ndcY / proj[5], 1.0f};
            float dz = ray.y * view[9] + ray.z * view[10];
            float z = dz < 0.0f ? -(eye.z - kWorldOffset.z) / dz : 1e9f;
            if (z < 0.9f * in.fogEnd)
            {
                farRow = py;
                farZ = z;
            }
        }
        float farT = t.At(640, farRow)[2] / 255.0f;
        std::printf("     ground at %.0f yd (row %u, stock fog end %.0f): transmittance %.3f\n", farZ, farRow, in.fogEnd,
                    farT);
        Check(farRow && farT < 0.15f, "distance fog hides terrain where the stock fog it replaces turns opaque");
        float highSky = t.At(640, 20)[2] / 255.0f;
        std::printf("     sky near the top of the view: transmittance %.3f\n", highSky);
        Check(highSky > 0.5f, "distance fog leaves the upper sky visible");
    }

    {
        Config c = cfg;
        c.temporal = 0.0f;
        c.godRays = 0.0f;
        vf_test_set_config(&c);
        LookAt(eye, at, view);
        FrameInputs in = MakeInputs(view, proj, eye, at, world);
        h.BeginFrame();
        h.DrawScene(eye, view, proj, world);
        vf_test_render(&in, &skip);
        Image single = Capture(h.dev);
        h.dev->EndScene();
        c.temporal = 0.85f;
        vf_test_set_config(&c);
        Image accumulated;
        for (int frame = 0; frame < 16; ++frame)
        {
            h.BeginFrame();
            h.DrawScene(eye, view, proj, world);
            vf_test_render(&in, &skip);
            if (frame == 15)
                accumulated = Capture(h.dev);
            h.dev->EndScene();
        }
        double diff = 0.0;
        for (UINT y = 0; y < 688; ++y)
            for (UINT x = 0; x < 1280; ++x)
                diff += std::fabs(accumulated.Luma(x, y) - single.Luma(x, y));
        diff /= 1280.0 * 688.0;
        std::printf("     static camera: accumulated vs single-frame mean luma difference %.4f\n", diff);
        Check(diff < 0.01, "temporal accumulation converges on a static camera (reprojection)");
        vf_test_set_config(&cfg);
    }

    {
        Config c = cfg;
        c.lightShafts = false;
        c.godRays = 0.0f;
        c.temporal = 0.0f;
        c.dataMode = 1;
        LookAt(eye, at, view);
        FrameInputs in = MakeInputs(view, proj, eye, at, world);
        in.mapId = 0;
        AuthoredFog authored = {};
        bool resolved = classic.Resolve(0, in.camPos, in.dayFraction, 0, authored);
        c.debugView = 2;
        vf_test_set_config(&c);
        h.BeginFrame();
        h.DrawScene(eye, view, proj, world);
        vf_test_render(&in, &skip);
        Image t = Capture(h.dev);
        h.dev->EndScene();
        c.debugView = 0;
        vf_test_set_config(&c);
        h.BeginFrame();
        h.DrawScene(eye, view, proj, world);
        vf_test_render(&in, &skip);
        Image classicImage = Capture(h.dev);
        h.dev->EndScene();
        SavePng(outDir + L"\\after-classic.png", classicImage.w, classicImage.h, classicImage.bgra);

        // Facing the sun with it just above the view, as in an in-game report of an over-bright sky.
        Vec3 sunAt = Add(eye, {in.toLight[0] * 100.0f, in.toLight[1] * 100.0f, -2.0f});
        float sunView[16];
        LookAt(eye, sunAt, sunView);
        FrameInputs sunIn = MakeInputs(sunView, proj, eye, sunAt, world);
        sunIn.mapId = 0;
        sunIn.toLight[2] = 0.45f;
        float len = std::sqrt(sunIn.toLight[0] * sunIn.toLight[0] + sunIn.toLight[1] * sunIn.toLight[1] + 0.2025f);
        for (float& v : sunIn.toLight)
            v /= len;
        const float variants[][2] = {{1.0f, 1.0f}, {0.6f, 1.0f}, {0.6f, 0.6f}, {0.45f, 0.8f}};
        for (const auto& v : variants)
        {
            Config sunCfg = cfg;
            sunCfg.dataMode = 1;
            sunCfg.exposure = v[0];
            sunCfg.sunScatter = v[1];
            vf_test_set_config(&sunCfg);
            for (int frame = 0; frame < 8; ++frame)
            {
                h.BeginFrame();
                h.DrawScene(eye, sunView, proj, world);
                vf_test_render(&sunIn, &skip);
                if (frame == 7)
                {
                    Image sunImage = Capture(h.dev);
                    wchar_t name[96];
                    swprintf(name, 96, L"\\classic-sun-exposure%.2f-sun%.2f.png", v[0], v[1]);
                    SavePng(outDir + name, sunImage.w, sunImage.h, sunImage.bgra);
                }
                h.dev->EndScene();
            }
        }
        bool match = resolved;
        const UINT samples[3][2] = {{101, 101}, {1181, 101}, {101, 201}};
        for (const auto& s : samples)
        {
            float lo = 1.0f;
            float hi = 0.0f;
            for (int j = 0; j < 16; ++j)
            {
                float r = ReferenceSkyTransmittance(in, c, resolved ? &authored : nullptr, s[0] + 0.5f, s[1] + 0.5f, 24,
                                                    (j + 0.5f) / 16.0f);
                lo = std::fmin(lo, r);
                hi = std::fmax(hi, r);
            }
            float got = t.At(s[0], s[1])[2] / 255.0f;
            std::printf("     Classic sky transmittance at %u,%u: shader %.3f, reference %.3f..%.3f\n", s[0], s[1], got,
                        lo, hi);
            match = match && got > lo - 0.02f && got < hi + 0.02f;
        }
        Check(match, "Classic-layer sky transmittance matches the CPU reference");
        vf_test_set_config(&cfg);
    }

    Image depthView = renderDebug(3, 255.0f);
    SavePng(outDir + L"\\debug-depth.png", depthView.w, depthView.h, depthView.bgra);
    {
        // Ground plane (z = 0) straight down the centre column near the bottom of the viewport.
        UINT px = 640;
        UINT py = 660;
        float ndcY = 1.0f - (py + 0.5f) / 688.0f * 2.0f;
        float ndcX = (px + 0.5f) / 1280.0f * 2.0f - 1.0f;
        Vec3 ray = {ndcX / proj[0], ndcY / proj[5], 1.0f};
        float inv[16];
        LookAt(eye, at, view);
        Vec3 dirW = {ray.x * view[0] + ray.y * view[1] + ray.z * view[2],
                     ray.x * view[4] + ray.y * view[5] + ray.z * view[6],
                     ray.x * view[8] + ray.y * view[9] + ray.z * view[10]};
        (void)inv;
        float viewZ = -(eye.z - kWorldOffset.z) / dirW.z;
        float expected = std::fmin(viewZ / 255.0f, 1.0f);
        float got = depthView.At(px, py)[2] / 255.0f;
        std::printf("     ground view depth: expected %.2f yd, shader %.2f yd\n", viewZ, got * 255.0f);
        Check(std::fabs(got - expected) < 2.5f / 255.0f, "depth linearisation matches the scene geometry");

        // The client draws the world with viewport depth [0, 0.94] (0x004F9019) and its distant WDL terrain
        // behind it at [0.998, 0.999]. Depth must be read through the captured viewport's range.
        D3DVIEWPORT9 clientVp = world;
        clientVp.MaxZ = 0.94f;
        Image clientDepth = renderDebugIn(3, 255.0f, clientVp, 0.9985f);
        float clientGot = clientDepth.At(px, py)[2] / 255.0f;
        float wdl = clientDepth.At(100, 50)[2] / 255.0f;
        std::printf("     client depth range: ground %.2f yd (expected %.2f), distant terrain %.2f of max distance\n",
                    clientGot * 255.0f, viewZ, wdl);
        Check(std::fabs(clientGot - expected) < 2.5f / 255.0f,
              "depth linearisation follows the world viewport's MaxZ 0.94");
        Check(wdl > 0.99f, "depth beyond the world range (distant terrain) counts as beyond the far clip");
        // Distant terrain is marched to the fog range with full density; the sky beside it thins with elevation.
        Image clientSkyT = renderDebugIn(2, 5000.0f, clientVp, 0.9985f);
        float wdlT = clientSkyT.At(100, 50)[2] / 255.0f;
        float skyT = clientSkyT.At(300, 50)[2] / 255.0f;
        std::printf("     transmittance: distant terrain %.3f, sky beside it %.3f\n", wdlT, skyT);
        Check(wdlT < skyT - 0.1f, "distant terrain is fogged as terrain, not as sky");

        UINT midRow = 0;
        float midZ = 0.0f;
        for (UINT row = 687; row > 0 && !midRow; --row)
        {
            float rowNdc = 1.0f - (row + 0.5f) / 688.0f * 2.0f;
            float dz = rowNdc / proj[5] * view[9] + view[10];
            float z = dz < 0.0f ? -(eye.z - kWorldOffset.z) / dz : 1e9f;
            if (z > 150.0f)
            {
                midRow = row;
                midZ = z;
            }
        }
        Image idealT = renderDebugIn(2, 5000.0f, world, 0.0f);
        Image clientT = renderDebugIn(2, 5000.0f, clientVp, 0.0f);
        float ti = idealT.At(640, midRow)[2] / 255.0f;
        float tc = clientT.At(640, midRow)[2] / 255.0f;
        std::printf("     ground at %.0f yd: transmittance %.3f with the full depth range, %.3f with the client's\n",
                    midZ, ti, tc);
        Check(ti < 0.97f && std::fabs(ti - tc) < 2.5f / 255.0f,
              "fog on geometry is the same with the client's depth range");
    }

    {
        DWORD before = 0;
        DWORD forced = 0;
        DWORD during = 0;
        DWORD after = 0;
        h.dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        h.dev->GetRenderState(D3DRS_ZWRITEENABLE, &before);
        vf_test_force_depth_write(1);
        h.dev->GetRenderState(D3DRS_ZWRITEENABLE, &forced);
        h.dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        h.dev->GetRenderState(D3DRS_ZWRITEENABLE, &during);
        h.dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        h.dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        vf_test_force_depth_write(0);
        h.dev->GetRenderState(D3DRS_ZWRITEENABLE, &after);
        Check(before == FALSE && forced == TRUE && during == TRUE && after == FALSE,
              "liquid-pass depth writes stay on while forced and the client's last request is restored");
    }

    Config restored = cfg;
    vf_test_set_config(&restored);

    h.ReleaseEngineObjects();
    h.pp.BackBufferWidth = 1024;
    h.pp.BackBufferHeight = 600;
    hr = h.dev->Reset(&h.pp);
    Check(SUCCEEDED(hr), "Reset through the wrapper");
    h.dev->GetDepthStencilSurface(&depth);
    std::memset(&depthDesc, 0, sizeof(depthDesc));
    if (depth)
    {
        depth->GetDesc(&depthDesc);
        depth->Release();
    }
    Check(depthDesc.Format == kIntz && depthDesc.Width == 1024, "INTZ depth recreated at the new size after Reset");
    h.CreateEngineObjects();
    const D3DVIEWPORT9 resized = {0, 0, 1024, 600, 0.0f, 1.0f};
    EngineProjection(1024.0f / 600.0f, proj);
    LookAt(eye, at, view);
    h.BeginFrame();
    h.DrawScene(eye, view, proj, resized);
    FrameInputs in = MakeInputs(view, proj, eye, at, resized);
    Check(vf_test_render(&in, &skip) != 0, (std::string("fog renders after Reset ") + skip).c_str());
    h.dev->EndScene();

    h.ReleaseEngineObjects();
    ULONG devRefs = h.dev->Release();
    ULONG d3dRefs = h.d3d->Release();
    Check(devRefs == 0 && d3dRefs == 0, "wrapper reference counts reach zero");
    DestroyWindow(h.window);
    CoUninitialize();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

// --scene harbour: the logged in-game frame at the Stormwind harbour (map 0, day 0.7802, CoAVolFog.log) rendered
// through the real wrapper and shaders over an ideal depth buffer. It checks nothing: it writes PNGs and prints
// probe tables, so the fog model can be judged apart from the in-game pipeline.
constexpr Vec3 kHarbourEye = {-8576.0f, 1007.0f, 104.0f};
constexpr float kHarbourNear = 0.2f;
constexpr float kHarbourFar = 791.6f;
// P11 of the logged frame, recovered from its sunPx (y -5485 of 1440) and view-space sun (0.950 0.308 0.054).
constexpr float kHarbourProjY = 1.511f;
constexpr UINT kHarbourWidth = 2560;
constexpr UINT kHarbourHeight = 1440;
constexpr int kHarbourSettleFrames = 24;
constexpr D3DCOLOR kHarbourSky = 0xFFFFD890;
constexpr D3DCOLOR kHarbourSea = 0xFF1A2430;
constexpr float kDegree = kPi / 180.0f;

Vec3 Dir(float azimuthDeg, float elevationDeg)
{
    float a = azimuthDeg * kDegree;
    float e = elevationDeg * kDegree;
    return {std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e)};
}

D3DCOLOR Shade(D3DCOLOR c, float s)
{
    auto ch = [&](int shift) {
        return static_cast<D3DCOLOR>(std::fmin(((c >> shift) & 0xFF) * s, 255.0f)) << shift;
    };
    return 0xFF000000u | ch(16) | ch(8) | ch(0);
}

// Objects stand on the sea (z = 0); side is degrees left of the view azimuth, distance is to the near face.
struct HarbourObject
{
    float side, distance, depth, halfWidth, top;
    D3DCOLOR color;
};

const HarbourObject kHarbourObjects[] = {
    {18.0f, 80.0f, 30.0f, 12.0f, 75.0f, 0xFF808080},   // ship
    {-6.0f, 350.0f, 20.0f, 10.0f, 140.0f, 0xFFA8A8B0}, // lighthouse
    {-25.0f, 650.0f, 80.0f, 150.0f, 190.0f, 0xFF4A5A38}, // ridge
};

void AddStandingBox(std::vector<SceneVertex>& v, float azimuthDeg, const HarbourObject& o)
{
    Vec3 f = Dir(azimuthDeg, 0.0f);
    Vec3 r = {f.y, -f.x, 0.0f};
    Vec3 c = {kHarbourEye.x + f.x * o.distance, kHarbourEye.y + f.y * o.distance, 0.0f};
    auto corner = [&](float along, float side, float z) {
        return Vec3{c.x + f.x * along + r.x * side, c.y + f.y * along + r.y * side, z};
    };
    const float w = o.halfWidth;
    Vec3 p[8] = {corner(0, -w, 0),       corner(0, w, 0),       corner(o.depth, w, 0),       corner(o.depth, -w, 0),
                 corner(0, -w, o.top), corner(0, w, o.top), corner(o.depth, w, o.top), corner(o.depth, -w, o.top)};
    const Vec3 absolute = {0, 0, 0};
    AddQuad(v, p[0], p[1], p[5], p[4], o.color, absolute);
    AddQuad(v, p[1], p[2], p[6], p[5], Shade(o.color, 0.75f), absolute);
    AddQuad(v, p[2], p[3], p[7], p[6], Shade(o.color, 0.6f), absolute);
    AddQuad(v, p[3], p[0], p[4], p[7], Shade(o.color, 0.75f), absolute);
    AddQuad(v, p[4], p[5], p[6], p[7], Shade(o.color, 1.15f), absolute);
}

// The same set of objects in front of each of the two horizontal views.
std::vector<SceneVertex> BuildHarbourScene(const float* baseAzimuths, int count)
{
    std::vector<SceneVertex> v;
    const float e = 3000.0f;
    const Vec3 absolute = {0, 0, 0};
    const Vec3 o = {kHarbourEye.x, kHarbourEye.y, 0.0f};
    AddQuad(v, Add(o, {-e, -e, 0}), Add(o, {e, -e, 0}), Add(o, {e, e, 0}), Add(o, {-e, e, 0}), kHarbourSea, absolute);
    for (int i = 0; i < count; ++i)
        for (const HarbourObject& obj : kHarbourObjects)
            AddStandingBox(v, baseAzimuths[i] + obj.side, obj);
    return v;
}

enum class ProbeKind
{
    World, // a = horizontal distance, b = height
    Sky,   // a = elevation (deg)
    Sun,   // a = elevation offset from the sun (deg), at the sun's azimuth
};

struct HarbourProbe
{
    const char* name;
    ProbeKind kind;
    float side;
    float a;
    float b;
};

const HarbourProbe kHarbourProbes[] = {
    {"ship (80 yd, z 55)", ProbeKind::World, 18.0f, 80.0f, 55.0f},
    {"ship hull (80 yd, z 10)", ProbeKind::World, 18.0f, 80.0f, 10.0f},
    {"lighthouse (350 yd, z 60)", ProbeKind::World, -6.0f, 350.0f, 60.0f},
    {"lighthouse top (350 yd, z 125)", ProbeKind::World, -6.0f, 350.0f, 125.0f},
    {"ridge (650 yd, z 60)", ProbeKind::World, -25.0f, 650.0f, 60.0f},
    {"ridge top (650 yd, z 170)", ProbeKind::World, -25.0f, 650.0f, 170.0f},
    {"sea 100 yd", ProbeKind::World, 3.0f, 100.0f, 0.0f},
    {"sea 160 yd", ProbeKind::World, 3.0f, 160.0f, 0.0f},
    {"sea 400 yd", ProbeKind::World, 3.0f, 400.0f, 0.0f},
    {"sea 700 yd", ProbeKind::World, 3.0f, 700.0f, 0.0f},
    {"sky past the far clip (-4 deg)", ProbeKind::Sky, 3.0f, -4.0f, 0.0f},
    {"sky at the horizon (+1 deg)", ProbeKind::Sky, 30.0f, 1.0f, 0.0f},
    {"sky 10 deg", ProbeKind::Sky, 30.0f, 10.0f, 0.0f},
    {"sky 40 deg", ProbeKind::Sky, 30.0f, 40.0f, 0.0f},
    {"near the sun (6 deg below)", ProbeKind::Sun, 0.0f, -6.0f, 0.0f},
    {"near the sun (15 deg below)", ProbeKind::Sun, 0.0f, -15.0f, 0.0f},
};

struct HarbourView
{
    const wchar_t* file;
    const char* name;
    float baseAzimuth;
    float pitch;
    // The world pass's viewport MaxZ: 1 is the ideal depth buffer; the client passes [0x00ADEEE4] = 0.94 to
    // GxXformSetViewport at 0x004F905A, which its D3D9 backend (0x006A9ACC) uploads as D3DVIEWPORT9::MaxZ.
    float maxZ;
};

constexpr float kClientWorldMaxZ = 0.94f;

struct Rgb
{
    float r, g, b;
};

Rgb SampleRgb(const Image& img, int cx, int cy, int radius)
{
    Rgb sum = {};
    int n = 0;
    for (int y = cy - radius; y <= cy + radius; ++y)
        for (int x = cx - radius; x <= cx + radius; ++x)
        {
            const unsigned char* p = img.At(static_cast<UINT>(x), static_cast<UINT>(y));
            sum.r += p[2];
            sum.g += p[1];
            sum.b += p[0];
            ++n;
        }
    return {sum.r / n, sum.g / n, sum.b / n};
}

// Continuous optical depth per layer along one pixel ray, as vf_march.hlsl integrates it with the sun visible
// (no screen-space shadows) and without its 24-step quadrature.
void ReferenceOpticalDepth(const FogParams& fog, float camZ, Vec3 dirW, float viewZ, float rayLen, bool sky,
                           float* tau)
{
    float z = sky ? fog.maxDistance : viewZ;
    float horizon = sky ? 1.0f : SmoothStep(fog.horizonStart, fog.farClip, z);
    z = z + (fog.maxDistance - z) * horizon;
    float tMax = std::fmin(z * rayLen, fog.maxDistance);
    float up = std::fmax(dirW.z, 0.0f);
    float dirZ = dirW.z + (up - dirW.z) * horizon;
    const int n = 8192;
    const float dt = tMax / n;
    for (int j = 0; j < kFogLayers; ++j)
        tau[j] = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        float t = (i + 0.5f) * dt;
        float h = camZ + dirZ * t;
        for (int j = 0; j < kFogLayers; ++j)
        {
            const FogLayer& l = fog.layers[j];
            if (t < l.start || t > l.limit)
                continue;
            float scale = sky ? std::exp(-up * l.skyFalloff) : 1.0f;
            float curve = 1.0f + l.strength * std::pow(std::fmin(std::fmax(t - l.start, 0.0f) / fog.maxDistance, 1.0f) +
                                                           1e-6f,
                                                       l.exponent);
            float heightF = std::fmin(std::exp((l.upperHeight - h) * l.upperFalloff), 1.0f) *
                            std::fmin(std::exp((h - l.lowerHeight) * l.lowerFalloff), 1.0f);
            tau[j] += l.density * scale * curve * heightF * dt;
        }
    }
}

void PrintLayers(const FogParams& fog)
{
    for (int i = 0; i < kFogLayers; ++i)
    {
        const FogLayer& l = fog.layers[i];
        std::printf("  layer %d (%s): start %.0f density %.6f g %.2f diffuse %.2f %.2f %.2f emissive %.2f %.2f %.2f "
                    "upper %.1f/%.4f lower %.1f/%.4f shadowed %.0f limit %.0f\n",
                    i, fog.authored && i < 3 ? "classic" : "derived", l.start, l.density, l.g, l.diffuse[0],
                    l.diffuse[1], l.diffuse[2], l.emissive[0], l.emissive[1], l.emissive[2], l.upperHeight,
                    l.upperFalloff, l.lowerHeight, l.lowerFalloff, l.shadowed, std::fmin(l.limit, 99999.0f));
        std::printf("           curve strength %.2f exponent %.2f, sky falloff %.2f, shadow density %.2f, "
                    "shadow emissive %.2f %.2f %.2f, isotropic %.2f\n",
                    l.strength, l.exponent, l.skyFalloff, l.shadowDensity, l.shadowEmissive[0], l.shadowEmissive[1],
                    l.shadowEmissive[2], l.isotropic);
    }
}

int RunHarbour(const std::wstring& outDir, const std::string& dataPath)
{
    FogData classic;
    if (!classic.Load(dataPath))
    {
        std::printf("Classic fog data %s did not load\n", dataPath.c_str());
        return 1;
    }
    CreateDirectoryW(outDir.c_str(), nullptr);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const Vec3 toLight = Norm({0.673f, 0.673f, 0.307f});
    const float sunAz = std::atan2(toLight.y, toLight.x) / kDegree;
    const float sunEl = std::asin(toLight.z) / kDegree;
    const float pitch = sunEl - 20.0f;
    const HarbourView views[] = {
        {L"sun", "facing the sun, sun 20 deg above the view centre", sunAz, pitch, 1.0f},
        {L"right", "90 deg right of the sun, same pitch", sunAz - 90.0f, pitch, 1.0f},
        {L"sun-up", "facing the sun, pitched up 25 deg (high sky)", sunAz, 25.0f, 1.0f},
        {L"right-up", "90 deg right of the sun, pitched up 25 deg (high sky)", sunAz - 90.0f, 25.0f, 1.0f},
        {L"sun-down", "facing the sun, pitched down 40 deg (ship hull, near sea)", sunAz, -40.0f, 1.0f},
        {L"sun-client-depth-range", "facing the sun, world drawn with the client's viewport MaxZ 0.94", sunAz, pitch,
         kClientWorldMaxZ},
        {L"right-client-depth-range", "90 deg right, world drawn with the client's viewport MaxZ 0.94",
         sunAz - 90.0f, pitch, kClientWorldMaxZ},
    };
    const float baseAzimuths[2] = {sunAz, sunAz - 90.0f};

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"vfog_harbour";
    RegisterClassW(&wc);
    Harness h;
    h.window = CreateWindowW(L"vfog_harbour", L"vfog harbour", WS_OVERLAPPEDWINDOW, 0, 0, 1280, 720, nullptr, nullptr,
                             wc.hInstance, nullptr);
    HMODULE d3d9 = LoadLibraryA("d3d9.dll");
    auto realCreate = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(d3d9, "Direct3DCreate9"));
    h.d3d = vf_test_wrap_direct3d9(realCreate, D3D_SDK_VERSION);
    if (!h.d3d)
        return 1;
    h.pp.Windowed = TRUE;
    h.pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    h.pp.BackBufferWidth = kHarbourWidth;
    h.pp.BackBufferHeight = kHarbourHeight;
    h.pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    h.pp.EnableAutoDepthStencil = TRUE;
    h.pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    h.pp.hDeviceWindow = h.window;
    h.pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    DWORD engineFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE | D3DCREATE_FPU_PRESERVE;
    if (FAILED(h.d3d->CreateDevice(0, D3DDEVTYPE_HAL, h.window, engineFlags, &h.pp, &h.dev)) || !h.dev)
    {
        std::printf("CreateDevice through the wrapper failed\n");
        h.d3d->Release();
        return 1;
    }
    h.scene = BuildHarbourScene(baseAzimuths, 2);
    h.clearColor = kHarbourSky;

    const D3DVIEWPORT9 vp = {0, 0, kHarbourWidth, kHarbourHeight, 0.0f, 1.0f};
    const float aspect = static_cast<float>(kHarbourWidth) / kHarbourHeight;
    float proj[16];
    EngineProjectionFrom(kHarbourProjY, aspect, kHarbourNear, kHarbourFar, proj);
    const Config cfg = {}; // the shipped CoAVolFog.ini values

    auto inputsFor = [&](const float* view, Vec3 at) {
        FrameInputs in = MakeInputs(view, proj, kHarbourEye, at, vp);
        // The client's camera target (0x00CD8F68) is one yard along the view (logged frame 0), so refZ = camera - 1.
        Vec3 f = Norm(Sub(at, kHarbourEye));
        in.camTarget[0] = kHarbourEye.x + f.x;
        in.camTarget[1] = kHarbourEye.y + f.y;
        in.camTarget[2] = kHarbourEye.z + f.z;
        in.dayFraction = 0.7802f;
        in.toLight[0] = toLight.x;
        in.toLight[1] = toLight.y;
        in.toLight[2] = toLight.z;
        in.lightIsMoon = false;
        in.fogColor = 0xFF574C5C;
        in.sunColor = 0xFFFFE7B6;
        in.directColor = 0xFFFF7400;
        in.ambientColor = 0xFF676680;
        in.fogStart = 197.9f;
        in.fogEnd = 791.7f;
        in.zoneFogDistance = 791.7f;
        in.farClip = kHarbourFar;
        in.inLiquid = false;
        in.mapId = 0;
        return in;
    };

    {
        float view[16];
        Vec3 at = Add(kHarbourEye, Dir(views[0].baseAzimuth, views[0].pitch));
        LookAt(kHarbourEye, at, view);
        FrameInputs in = inputsFor(view, at);
        AuthoredFog authored = {};
        bool resolved = classic.Resolve(0, in.camPos, in.dayFraction, 0, authored);
        std::printf("harbour frame: camera (%.1f %.1f %.1f), day %.4f, toLight (%.3f %.3f %.3f) = azimuth %.1f "
                    "elevation %.2f deg\n",
                    in.camPos[0], in.camPos[1], in.camPos[2], in.dayFraction, toLight.x, toLight.y, toLight.z, sunAz,
                    sunEl);
        std::printf("  projection P00 %.4f P11 %.4f near %.2f far %.1f, %ux%u\n", proj[0], proj[5], kHarbourNear,
                    kHarbourFar, kHarbourWidth, kHarbourHeight);
        std::printf("  Classic lights:");
        for (int i = 0; i < authored.lightCount; ++i)
            std::printf(" %u:%.2f", authored.lightIds[i], authored.lightWeights[i]);
        std::printf(" (%s, %d layers)\n", resolved ? "resolved" : "NOT resolved", authored.layerCount);
        for (int i = 0; i < authored.layerCount; ++i)
        {
            const AuthoredLayer& a = authored.layers[i];
            std::printf("  authored %d: start %.1f density %.4f g %.3f intensity %.2f strength %.3f exponent %.3f "
                        "diffuse %.3f %.3f %.3f emissive %.3f %.3f %.3f flags %u\n",
                        i, a.start, a.density, a.g, a.intensity, a.strength, a.exponent, a.diffuse[0], a.diffuse[1],
                        a.diffuse[2], a.emissive[0], a.emissive[1], a.emissive[2], a.flags);
        }
        FogParams fog = BuildFogParams(in, cfg, resolved ? &authored : nullptr);
        std::printf("  fog params (as the DLL logs them): refZ %.1f maxDistance %.0f horizonStart %.1f farLimit %.1f "
                    "exposure %.2f\n",
                    fog.referenceZ, fog.maxDistance, fog.horizonStart, fog.farLimit,
                    fog.authored ? cfg.classicExposure : cfg.exposure);
        PrintLayers(fog);
    }

    const char* skip = "";
    auto frame = [&](const float* view, const FrameInputs& in, const Config& c, bool capture, Image* before) {
        vf_test_set_config(&c);
        h.BeginFrame();
        h.DrawScene(kHarbourEye, view, proj, in.viewport);
        if (before)
            *before = Capture(h.dev);
        if (!vf_test_render(&in, &skip))
            std::printf("     fog skipped: %s\n", skip);
        Image img;
        if (capture)
            img = Capture(h.dev);
        h.dev->EndScene();
        h.dev->Present(nullptr, nullptr, nullptr, nullptr);
        return img;
    };

    for (const HarbourView& hv : views)
    {
        float view[16];
        Vec3 at = Add(kHarbourEye, Dir(hv.baseAzimuth, hv.pitch));
        LookAt(kHarbourEye, at, view);
        FrameInputs in = inputsFor(view, at);
        in.viewport.MaxZ = hv.maxZ; // the DLL captures this viewport after the opaque pass
        const bool idealDepth = hv.maxZ >= 1.0f;

        // Linear depth at 791.6/255 yd per level: MaxDistance below the far clip makes the far clip the range.
        Config depthCfg = cfg;
        depthCfg.debugView = 3;
        depthCfg.maxDistance = 200.0f;
        depthCfg.temporal = 0.0f;
        Image depth = frame(view, in, depthCfg, true, nullptr);

        Config c = cfg;
        c.temporal = 0.0f;
        frame(view, in, c, false, nullptr); // restarts the history from this view
        c.temporal = cfg.temporal;
        Image before;
        Image after;
        for (int i = 1; i < kHarbourSettleFrames; ++i)
        {
            bool last = i == kHarbourSettleFrames - 1;
            Image img = frame(view, in, c, last, last ? &before : nullptr);
            if (last)
                after = img;
        }
        Image radiance;
        Image transmittance;
        c.debugView = 1;
        for (int i = 0; i < 4; ++i)
            radiance = frame(view, in, c, i == 3, nullptr);
        c.debugView = 2;
        for (int i = 0; i < 4; ++i)
            transmittance = frame(view, in, c, i == 3, nullptr);

        std::wstring stem = outDir + L"\\harbour-" + hv.file;
        SavePng(stem + L"-before.png", before.w, before.h, before.bgra);
        SavePng(stem + L"-after.png", after.w, after.h, after.bgra);
        SavePng(stem + L"-radiance.png", radiance.w, radiance.h, radiance.bgra);
        SavePng(stem + L"-transmittance.png", transmittance.w, transmittance.h, transmittance.bgra);
        SavePng(stem + L"-depth.png", depth.w, depth.h, depth.bgra);

        AuthoredFog authored = {};
        bool resolved = classic.Resolve(0, in.camPos, in.dayFraction, 0, authored);
        FogParams fog = BuildFogParams(in, cfg, resolved ? &authored : nullptr);
        float sunV[3];
        TransformDirection(in.toLight, view, sunV);
        std::printf("\nview %ls: %s (azimuth %.1f, pitch %.1f; sun view-space %.3f %.3f %.3f, refZ %.1f, "
                    "viewport MaxZ %.2f)\n",
                    hv.file, hv.name, hv.baseAzimuth, hv.pitch, sunV[0], sunV[1], sunV[2], fog.referenceZ,
                    hv.maxZ);
        std::printf("  %-31s %6s %6s %6s %13s %6s %15s %15s %15s %15s %6s  %s\n", "probe", "px", "py", "dist",
                    "viewZ/shader", "alpha", "fog rgb", "fog rgb / a", "before", "after", "cpu a",
                    "cpu tau L0 L1 L2 L3");
        for (const HarbourProbe& p : kHarbourProbes)
        {
            Vec3 d;
            if (p.kind == ProbeKind::World)
            {
                Vec3 q = Dir(hv.baseAzimuth + p.side, 0.0f);
                d = {q.x * p.a, q.y * p.a, p.b - kHarbourEye.z};
            }
            else if (p.kind == ProbeKind::Sky)
                d = Dir(hv.baseAzimuth + p.side, p.a);
            else
                d = Dir(sunAz, sunEl + p.a);
            float vx = d.x * view[0] + d.y * view[4] + d.z * view[8];
            float vy = d.x * view[1] + d.y * view[5] + d.z * view[9];
            float vz = d.x * view[2] + d.y * view[6] + d.z * view[10];
            if (vz <= 0.01f)
                continue;
            float px = (vx / vz * proj[0] + proj[8]) * 0.5f + 0.5f;
            float py = 0.5f - (vy / vz * proj[5] + proj[9]) * 0.5f;
            int ix = static_cast<int>(px * kHarbourWidth);
            int iy = static_cast<int>(py * kHarbourHeight);
            if (ix < 4 || iy < 4 || ix >= static_cast<int>(kHarbourWidth) - 4 ||
                iy >= static_cast<int>(kHarbourHeight) - 4)
                continue;
            const bool world = p.kind == ProbeKind::World;
            float len = std::sqrt(Dot(d, d));
            float zShader = depth.At(ix, iy)[2] / 255.0f * kHarbourFar;
            bool shaderSky = depth.At(ix, iy)[2] >= 254;
            bool matches = world ? std::fabs(zShader - vz) < 6.0f && !shaderSky : shaderSky;
            // With the client's depth range the linearised depth is wrong by construction, so only sky versus
            // geometry is checked; the probe layout is the one verified in the ideal-depth views.
            if (!idealDepth)
                matches = world ? !shaderSky : shaderSky;
            if (!matches)
            {
                std::printf("  %-31s %6d %6d  hidden (depth %.1f yd%s)\n", p.name, ix, iy, zShader,
                            shaderSky ? ", sky" : "");
                continue;
            }
            const int radius = 2;
            Rgb fogRgb = SampleRgb(radiance, ix, iy, radius);
            Rgb t = SampleRgb(transmittance, ix, iy, radius);
            Rgb b = SampleRgb(before, ix, iy, radius);
            Rgb a = SampleRgb(after, ix, iy, radius);
            float alpha = 1.0f - t.r / 255.0f;
            float inv = alpha > 0.02f ? 1.0f / alpha : 0.0f;
            float tau[kFogLayers];
            Vec3 dirW = {d.x / len, d.y / len, d.z / len};
            ReferenceOpticalDepth(fog, kHarbourEye.z, dirW, world ? vz : 0.0f, world ? len / vz : 1.0f, !world, tau);
            float tauSum = tau[0] + tau[1] + tau[2] + tau[3];
            char distText[16];
            char zText[24];
            if (world)
            {
                std::snprintf(distText, sizeof(distText), "%.0f", len);
                std::snprintf(zText, sizeof(zText), "%.0f/%.0f", vz, zShader);
            }
            else
            {
                std::snprintf(distText, sizeof(distText), "sky");
                std::snprintf(zText, sizeof(zText), "sky/sky");
            }
            std::printf("  %-31s %6d %6d %6s %13s %6.3f %4.0f %4.0f %4.0f  %4.0f %4.0f %4.0f  %4.0f %4.0f %4.0f  "
                        "%4.0f %4.0f %4.0f %6.3f  %.3f %.3f %.3f %.3f\n",
                        p.name, ix, iy, distText, zText, alpha, fogRgb.r, fogRgb.g, fogRgb.b,
                        std::fmin(fogRgb.r * inv, 999.0f), std::fmin(fogRgb.g * inv, 999.0f),
                        std::fmin(fogRgb.b * inv, 999.0f), b.r, b.g, b.b, a.r, a.g, a.b, 1.0f - std::exp(-tauSum),
                        tau[0], tau[1], tau[2], tau[3]);
        }
    }

    vf_test_set_config(&cfg);
    ULONG devRefs = h.dev->Release();
    ULONG d3dRefs = h.d3d->Release();
    DestroyWindow(h.window);
    CoUninitialize();
    std::printf("\nharbour scene written to %ls (device refs %lu, d3d refs %lu)\n", outDir.c_str(), devRefs, d3dRefs);
    return 0;
}
}

int wmain(int argc, wchar_t** argv)
{
    std::wstring out = L"harness-out";
    std::string data = "fogdata.bin";
    std::wstring scene;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::wcscmp(argv[i], L"--out") == 0)
            out = argv[i + 1];
        if (std::wcscmp(argv[i], L"--data") == 0)
        {
            char path[MAX_PATH] = {};
            WideCharToMultiByte(CP_ACP, 0, argv[i + 1], -1, path, MAX_PATH, nullptr, nullptr);
            data = path;
        }
        if (std::wcscmp(argv[i], L"--scene") == 0)
        {
            scene = argv[i + 1];
            if (i + 2 < argc && argv[i + 2][0] != L'-')
                out = argv[i + 2];
        }
    }
    if (scene == L"harbour")
        return RunHarbour(out, data);
    if (!scene.empty())
    {
        std::printf("unknown scene %ls (known: harbour)\n", scene.c_str());
        return 2;
    }
    return Run(out, data);
}
