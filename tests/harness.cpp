// Offline check of CoAVolFog on a real D3D9 device: device wrapping, INTZ substitution, the fog passes,
// state restoration, depth linearisation, viewport clipping and Reset. Writes PNG captures to --out.
#include "config.h"
#include "engine.h"
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
void EngineProjection(float aspect, float* m)
{
    float ys = 1.0f / std::tan(kFovY * 0.5f);
    float p[16] = {ys / aspect, 0, 0, 0, 0, ys, 0, 0, 0, 0, (kFar + kNear) / (kFar - kNear), 1,
                   0, 0, -2.0f * kFar * kNear / (kFar - kNear), 0};
    std::memcpy(m, p, sizeof(p));
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

void AddQuad(std::vector<SceneVertex>& v, Vec3 a, Vec3 b, Vec3 c, Vec3 d, DWORD color)
{
    for (Vec3 p : {a, b, c, a, c, d})
    {
        Vec3 w = Add(p, kWorldOffset);
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

struct Sentinel
{
    DWORD renderStates[10];
    DWORD samplers[3][4];
    IDirect3DBaseTexture9* textures[3];
    IDirect3DVertexShader9* vs;
    IDirect3DPixelShader9* ps;
    IDirect3DVertexDeclaration9* decl;
    IDirect3DVertexBuffer9* stream;
    UINT streamOffset, streamStride;
    float constants[24 * 4];
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
    for (DWORD t = 0; t < 3; ++t)
    {
        for (int i = 0; i < 4; ++i)
            dev->GetSamplerState(t, kSentinelSamplers[i], &s.samplers[t][i]);
        dev->GetTexture(t, &s.textures[t]);
    }
    dev->GetVertexShader(&s.vs);
    dev->GetPixelShader(&s.ps);
    dev->GetVertexDeclaration(&s.decl);
    dev->GetStreamSource(0, &s.stream, &s.streamOffset, &s.streamStride);
    dev->GetPixelShaderConstantF(0, s.constants, 24);
    dev->GetViewport(&s.viewport);
    dev->GetScissorRect(&s.scissor);
    dev->GetRenderTarget(0, &s.rt);
    dev->GetDepthStencilSurface(&s.ds);
}

void ReleaseSentinel(Sentinel& s)
{
    IUnknown* refs[] = {s.textures[0], s.textures[1], s.textures[2], s.vs, s.ps, s.decl, s.stream, s.rt, s.ds};
    for (IUnknown* r : refs)
        if (r)
            r->Release();
}

void ReportSentinelDifferences(const Sentinel& a, const Sentinel& b)
{
    for (int i = 0; i < 10; ++i)
        if (a.renderStates[i] != b.renderStates[i])
            std::printf("     render state %d: %lu -> %lu\n", kSentinelStates[i], a.renderStates[i], b.renderStates[i]);
    for (int t = 0; t < 3; ++t)
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
    for (int i = 0; i < 24 * 4; ++i)
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
        for (DWORD t = 0; t < 3; ++t)
            dev->SetTexture(t, nullptr);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF6FA0DC, 1.0f, 0);
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
        for (DWORD t = 0; t < 3; ++t)
        {
            dev->SetTexture(t, dummyTexture);
            dev->SetSamplerState(t, D3DSAMP_ADDRESSU, D3DTADDRESS_MIRROR);
            dev->SetSamplerState(t, D3DSAMP_MAGFILTER, D3DTEXF_ANISOTROPIC);
            dev->SetSamplerState(t, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(t, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        }
        float constants[24 * 4];
        for (int i = 0; i < 24 * 4; ++i)
            constants[i] = 0.25f * i - 3.0f;
        dev->SetPixelShaderConstantF(0, constants, 24);
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
    return in;
}

float SmoothStep(float e0, float e1, float x)
{
    float t = std::fmin(std::fmax((x - e0) / (e1 - e0), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// CPU transmittance of the march for one pixel of a sky ray (no occluders, shafts disabled).
float ReferenceSkyTransmittance(const FrameInputs& in, const Config& cfg, float px, float py, int steps, float jitter)
{
    FogParams fog = BuildFogParams(in, cfg);
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
        for (int i = 0; i < 3; ++i)
        {
            const FogLayer& l = fog.layers[i];
            float scale = i == 2 ? std::exp(-std::fmax(dirW.z, 0.0f) * fog.farSkyFalloff) *
                                       std::fmin(std::fmax((fog.farLimit - ta) / std::fmax(dt, 1e-3f), 0.0f), 1.0f)
                                 : 1.0f;
            float cover = std::fmin(std::fmax((t - l.start) / std::fmax(dt, 1e-3f), 0.0f), 1.0f);
            float curve = 1.0f + l.strength * std::pow(std::fmin(t / fog.maxDistance, 1.0f) + 1e-6f, l.exponent);
            float heightF = std::fmin(std::exp((l.heightBase - h) * l.heightFalloff), 1.0f);
            tau += l.density * scale * dt * cover * curve * heightF;
        }
    }
    return static_cast<float>(std::exp(-tau));
}

int Run(const std::wstring& outDir)
{
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
    cfg.maxDistance = 1500.0f;
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

    auto renderDebug = [&](int mode, float maxDist) {
        Config c = cfg;
        c.debugView = mode;
        c.maxDistance = maxDist;
        c.temporal = 0.0f;
        vf_test_set_config(&c);
        LookAt(eye, at, view);
        h.BeginFrame();
        h.DrawScene(eye, view, proj, world);
        FrameInputs in = MakeInputs(view, proj, eye, at, world);
        in.farClip = std::fmin(in.farClip, maxDist);
        vf_test_render(&in, &skip);
        Image img = Capture(h.dev);
        h.dev->EndScene();
        h.dev->Present(nullptr, nullptr, nullptr, nullptr);
        return img;
    };

    Image radiance = renderDebug(1, 1500.0f);
    SavePng(outDir + L"\\debug-radiance.png", radiance.w, radiance.h, radiance.bgra);
    Image transmittance = renderDebug(2, 1500.0f);
    SavePng(outDir + L"\\debug-transmittance.png", transmittance.w, transmittance.h, transmittance.bgra);

    {
        Config c = cfg;
        c.lightShafts = false;
        vf_test_set_config(&c);
        Config saved = cfg;
        cfg = c;
        Image t = renderDebug(2, 1500.0f);
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
                float r = ReferenceSkyTransmittance(in, c, s[0] + 0.5f, s[1] + 0.5f, 24, (j + 0.5f) / 16.0f);
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
}

int wmain(int argc, wchar_t** argv)
{
    std::wstring out = L"harness-out";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::wcscmp(argv[i], L"--out") == 0)
            out = argv[i + 1];
    return Run(out);
}
