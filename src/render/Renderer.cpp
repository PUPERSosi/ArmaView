#include "Renderer.h"
#include "Perf.h"
#include <d3dcompiler.h>
#include <wincodec.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

namespace {

const char* kShaderSource = R"(
cbuffer Scene : register(b0)
{
    float4x4 viewProj;
    float4 lightDir;      // xyz: direction light travels
    float4 cameraPos;
    float4 overrideColor;
    float4 params;        // x: 1 = output overrideColor, y: shading mode, z: two-sided, w: flip normal-map green
    float4 lightColor;
    float4 ambientColor;  // sky ambient, w = reflections enabled
    float4 groundColor;   // ground ambient, w = exposure
    float4x4 lightViewProj;
    float4 shadowParams;  // x on, y softness (texels), z 1 / map size, w taps per axis
    float4 skinParams;    // x 1 = GPU skinning (point matrices)
};
StructuredBuffer<float4x4> pointMats : register(t7);

cbuffer Material : register(b1)
{
    float4 mAmbient, mDiffuse, mForced, mEmissive, mSpecular;
    float4 mParams;       // x specularPower, y alpha mode
    float4 uvA[6];
    float4 uvB[6];
    float4 mEnv;          // x reflections on, y fresnel n, z fresnel k
    float4x4 nmSwz;       // normal map un-swizzle (PAA SWIZ: _nohq keeps X as 1-X in alpha)
    float4 nmSwzOff;
    float4 mMulti;        // x 1 = Multi shader
    float4 mSkin;         // x 1 = Skin shader
    float4 muvA[13];
    float4 muvB[13];
};
Texture2D tM[13] : register(t8); // Multi: 4 x _co, mask, 4 x _dtsmdi, 4 x _nohq

Texture2D tDiffuse : register(t0);
Texture2D tNormal  : register(t1);
Texture2D tSpec    : register(t2);
Texture2D tAS      : register(t3);
Texture2D tMacro   : register(t4);
Texture2D tDetail  : register(t5);
SamplerState samp  : register(s0);
Texture2D tShadow  : register(t6);
SamplerComparisonState sampShadow : register(s1);

// Sun visibility 0..1 (PCF over taps x taps samples spread by the softness radius).
float Shadow(float3 wpos, float3 N)
{
    if (shadowParams.x < 0.5) return 1;
    float3 Ls = -lightDir.xyz;
    float4 sp = mul(lightViewProj, float4(wpos + N * 0.01, 1));
    float3 p = sp.xyz / sp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0) || any(uv > 1)) return 1;
    p.z = min(p.z, 1); // a receiver beyond the far plane (the floor under a low sun) is still behind what the map saw
    float bias = 0.0015 + 0.003 * (1 - saturate(dot(N, Ls)));
    int taps = (int)shadowParams.w;
    float step = shadowParams.y * shadowParams.z * 2.0 / max(taps - 1, 1);
    float sum = 0;
    [loop] for (int y = 0; y < taps; ++y)
        [loop] for (int x = 0; x < taps; ++x)
        {
            float2 o = (float2(x, y) - (taps - 1) * 0.5) * step;
            sum += tShadow.SampleCmpLevelZero(sampShadow, uv + o, p.z - bias);
        }
    return sum / (taps * taps);
}

struct MeshIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; float2 uv1 : TEXCOORD1; float4 col : COLOR0; uint pidx : BLENDINDICES0; };
struct MeshOut { float4 pos : SV_Position; float3 wpos : TEXCOORD2; float3 nrm : NORMAL; float2 uv : TEXCOORD0; float2 uv1 : TEXCOORD1; float4 col : COLOR0; };

MeshOut VSMesh(MeshIn i)
{
    MeshOut o;
    float3 p = i.pos, n = i.nrm;
    if (skinParams.x > 0.5)
    {
        float4x4 m = pointMats[i.pidx];
        p = mul(m, float4(p, 1)).xyz;
        n = mul((float3x3)m, n);
    }
    o.pos = mul(viewProj, float4(p, 1));
    o.wpos = p; o.nrm = n; o.uv = i.uv; o.uv1 = i.uv1; o.col = i.col;
    return o;
}

float2 StageUV(int s, float2 uv0, float2 uv1)
{
    float2 uv = uvB[s].z > 0.5 ? uv1 : uv0;
    return float2(uv.x * uvA[s].x + uv.y * uvA[s].z + uvB[s].x, uv.x * uvA[s].y + uv.y * uvA[s].w + uvB[s].y);
}
float2 MultiUV(int s, float2 uv0, float2 uv1)
{
    float2 uv = muvB[s].z > 0.5 ? uv1 : uv0;
    return float2(uv.x * muvA[s].x + uv.y * muvA[s].z + muvB[s].x, uv.x * muvA[s].y + uv.y * muvA[s].w + muvB[s].y);
}
float3 ToLinear(float3 c) { return pow(max(c, 0), 2.2); }
float3 ToGamma(float3 c)  { return pow(max(c, 0), 1.0 / 2.2); }
// Hemispheric ambient: sky from above, ground from below.
float3 Ambient(float3 N) { return lerp(groundColor.rgb, ambientColor.rgb, saturate(N.y * 0.5 + 0.5)); }

// Sky "cube map" from the current lighting: ground below, sky above (brighter towards the horizon), sun disc.
float3 EnvSky(float3 R)
{
    float3 L = -lightDir.xyz;
    float h = R.y;
    float3 sky = ambientColor.rgb * (1.3 - 0.5 * saturate(h)) ;
    float3 c = h >= 0 ? sky : lerp(ambientColor.rgb * 0.8, groundColor.rgb, saturate(-h * 4));
    c += lightColor.rgb * (pow(saturate(dot(R, L)), 600) * 30 + pow(saturate(dot(R, L)), 12) * 0.4);
    return c;
}
// Fresnel reflectance for a complex index of refraction n + ik (unpolarised), as the procedural fresnel(n,k) texture.
float FresnelNK(float c, float n, float k)
{
    c = clamp(c, 1e-3, 1);
    float s2 = 1 - c * c, n2 = n * n, k2 = k * k;
    float t = n2 - k2 - s2;
    float w = sqrt(t * t + 4 * n2 * k2);
    float a2 = max(0.5 * (w + t), 0), b2 = max(0.5 * (w - t), 0);
    float a = sqrt(a2);
    float rs = (a2 + b2 - 2 * a * c + c * c) / (a2 + b2 + 2 * a * c + c * c);
    float st = s2 / c; // sin * tan
    float rp = rs * (a2 + b2 - 2 * a * st + st * st) / (a2 + b2 + 2 * a * st + st * st);
    return saturate(0.5 * (rs + rp));
}

// Tangent frame from screen-space derivatives (no vertex tangents needed).
float3x3 CotangentFrame(float3 N, float3 p, float2 uv)
{
    float3 dp1 = ddx(p), dp2 = ddy(p);
    float2 duv1 = ddx(uv), duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = rsqrt(max(dot(T, T), dot(B, B)) + 1e-20);
    return float3x3(T * invmax, B * invmax, N);
}

float4 PSMesh(MeshOut i) : SV_Target
{
    if (params.x > 0.5 && params.x < 1.5) return overrideColor;
    if (params.x > 2.5) // ground plane: darkens where the sun is blocked
    {
        float sh = Shadow(i.wpos, float3(0, 1, 0));
        return float4(0, 0, 0, (1 - sh) * 0.5 * saturate(-lightDir.y * 3));
    }
    int mode = (int)(params.y + 0.5);
    float3 n = normalize(i.nrm);
    if (params.x > 1.5) // ghost: simple lit tint with alpha
    {
        float3 gv = normalize(cameraPos.xyz - i.wpos);
        if (dot(n, gv) < 0) n = -n;
        float gl = saturate(dot(n, -lightDir.xyz));
        return float4(overrideColor.rgb * (0.45 + 0.55 * gl), overrideColor.a);
    }
    if (mode == 2) return float4(n * 0.5 + 0.5, 1);
    float3 v = normalize(cameraPos.xyz - i.wpos);
    if (params.z > 0.5 && dot(n, v) < 0) n = -n;
    float3 L = -lightDir.xyz;
    if (mode == 10 || mode == 11) // tangent frame used for the normal map (screen-space derivatives: mirrored UVs flip B)
    {
        float3x3 F = CotangentFrame(n, i.wpos, StageUV(1, i.uv, i.uv1));
        return float4(normalize(mode == 10 ? F[0] : F[1]) * 0.5 + 0.5, 1);
    }

    if (mode <= 1) // untextured lit / UV checker
    {
        float3 base = float3(0.62, 0.62, 0.66);
        if (mode == 1)
        {
            float2 c = floor(frac(i.uv) * 8);
            base = fmod(c.x + c.y, 2) < 1 ? float3(0.85, 0.85, 0.85) : float3(0.25, 0.35, 0.6);
            if (any(i.uv < 0) || any(i.uv > 1)) base *= float3(1, 0.5, 0.5);
        }
        base = lerp(base, i.col.rgb, i.col.a);
        float ndl = saturate(dot(n, L)) * Shadow(i.wpos, n);
        float3 h = normalize(L + v);
        float3 spec = pow(saturate(dot(n, h)), 32) * 0.08 * lightColor.rgb * ndl;
        float3 lin = ToLinear(base) * (Ambient(n) + lightColor.rgb * ndl) + spec;
        return float4(ToGamma(lin * groundColor.w), 1);
    }

    float2 uvD = StageUV(0, i.uv, i.uv1), uvN = StageUV(1, i.uv, i.uv1), uvS = StageUV(2, i.uv, i.uv1);
    float2 uvAs = StageUV(3, i.uv, i.uv1), uvM = StageUV(4, i.uv, i.uv1), uvT = StageUV(5, i.uv, i.uv1);
    float4 co = tDiffuse.Sample(samp, uvD);
    if (mParams.y > 0.5 && mParams.y < 1.5 && co.a < 0.5) discard;
    float4 nm = mul(nmSwz, tNormal.Sample(samp, uvN)) + nmSwzOff;
    float4 sm = tSpec.Sample(samp, uvS);
    float4 as = tAS.Sample(samp, uvAs);
    float4 mc = tMacro.Sample(samp, uvM);
    float4 dt = tDetail.Sample(samp, uvT);
    if (mMulti.x > 0.5) // Multi: 4 layers blended by the mask (black = layer 0, R / G / B = layers 1-3)
    {
        float4 msk = tM[4].Sample(samp, MultiUV(4, i.uv, i.uv1));
        float4 w = float4(saturate(1 - msk.r - msk.g - msk.b), msk.r, msk.g, msk.b);
        w /= max(w.x + w.y + w.z + w.w, 1e-3);
        float3 c = 0, n3 = 0;
        float2 sp = 0;
        [unroll] for (int k = 0; k < 4; ++k)
        {
            c += w[k] * tM[k].Sample(samp, MultiUV(k, i.uv, i.uv1)).rgb;
            float4 nk = mul(nmSwz, tM[9 + k].Sample(samp, MultiUV(9 + k, i.uv, i.uv1))) + nmSwzOff;
            n3 += w[k] * (nk.rgb * 2 - 1);
            sp += w[k] * tM[5 + k].Sample(samp, MultiUV(5 + k, i.uv, i.uv1)).gb;
        }
        co = float4(c, 1);
        nm = float4(normalize(n3) * 0.5 + 0.5, 1);
        sm = float4(sm.r, sp, sm.a);
        if (mode == 4) return float4(lerp(co.rgb, i.col.rgb, i.col.a), 1);
        if (mode == 5) return float4(nm.rgb, 1);
    }
    if (mode == 4) return float4(lerp(co.rgb, i.col.rgb, i.col.a), 1);
    if (mode == 5) return float4(nm.rgb, 1);
    if (mode == 6) return float4(sm.rgb, 1);
    if (mode == 7) return float4(as.rgb, 1);
    if (mode == 8) return float4(mc.rgb * mc.a, 1);
    if (mode == 9) return float4(dt.rgb, 1);

    // Approximation of the super shader: macro over diffuse, detail as x2 overlay, tangent-space normal map,
    // _smdi: G = specular intensity, B = glossiness (scales specularPower), _as: G = ambient occlusion.
    float3 albedo = lerp(co.rgb, mc.rgb, mc.a);
    albedo = saturate(albedo * dt.rgb * 2.0);
    albedo = ToLinear(albedo);
    float3 tn = nm.rgb * 2.0 - 1.0;
    if (params.w > 0.5) tn.y = -tn.y;
    float3 N = normalize(mul(tn, CotangentFrame(n, i.wpos, uvN)));
    if (mode == 12) return float4(N * 0.5 + 0.5, 1);
    float ndl = saturate(dot(N, L)) * Shadow(i.wpos, n);
    float3 h = normalize(L + v);
    float power = max(1.0, mParams.x * max(sm.b, 0.02));
    float3 lit;
    if (mSkin.x > 0.5)
    {
        // Skin (approximation): no emission - emmisive rgb = scatter colour, a = scatter amount: light wraps past
        // the terminator tinted by it; forcedDiffuse rgb x a = translucency at grazing angles (ears, nose edges)
        float wrap = saturate(mEmissive.a) * 0.6;
        float ndlRaw = dot(N, L);
        float ndlWrap = saturate((ndlRaw + wrap) / (1 + wrap)) * Shadow(i.wpos, n);
        float3 scatter = ToLinear(mEmissive.rgb) * max(ndlWrap - saturate(ndlRaw), 0);
        float rim = pow(1 - saturate(dot(N, v)), 3) * mForced.a;
        lit = albedo * (Ambient(N) * mAmbient.rgb * as.g * 0.5 + lightColor.rgb * (mDiffuse.rgb * 2.0 * ndlWrap + scatter))
            + albedo * ToLinear(mForced.rgb) * rim * (Ambient(N) + lightColor.rgb * 0.3)
            + mSpecular.rgb * sm.g * pow(saturate(dot(N, h)), power) * ndl * lightColor.rgb;
    }
    else
        lit = albedo * (Ambient(N) * mAmbient.rgb * as.g + lightColor.rgb * mDiffuse.rgb * ndl + mForced.rgb)
            + mSpecular.rgb * sm.g * pow(saturate(dot(N, h)), power) * ndl * lightColor.rgb
            + albedo * mEmissive.rgb;
    if (mEnv.x > 0.5 && ambientColor.w > 0.5) // reflections: env (sky) x fresnel x specular map (G) x specular colour
    {
        float3 R = reflect(-v, N);
        float F = FresnelNK(saturate(dot(N, v)), mEnv.y, mEnv.z);
        lit += EnvSky(R) * F * sm.g * mSpecular.rgb;
    }
    float3 outc = lerp(ToGamma(lit * groundColor.w), i.col.rgb, i.col.a * 0.85);
    return float4(outc, mParams.y > 1.5 ? co.a * mDiffuse.a : 1.0);
}

struct LineIn  { float3 pos : POSITION; float4 col : COLOR0; };
struct LineOut { float4 pos : SV_Position; float4 col : COLOR0; };

LineOut VSLine(LineIn i)
{
    LineOut o;
    o.pos = mul(viewProj, float4(i.pos, 1));
    o.col = i.col;
    return o;
}

float4 PSLine(LineOut i) : SV_Target { return i.col; }
)";

struct SceneConstants
{
    glm::mat4 viewProj;
    glm::vec4 lightDir;
    glm::vec4 cameraPos;
    glm::vec4 overrideColor;
    glm::vec4 params;
    glm::vec4 lightColor;
    glm::vec4 ambientColor;
    glm::vec4 groundColor;
    glm::mat4 lightViewProj;
    glm::vec4 shadowParams;
    glm::vec4 skinParams;
};

// Compiled shaders are kept in cache\shaders next to the exe, keyed by a hash of source + entry + target + flags.
std::filesystem::path ShaderCacheFile(const char* entry, const char* target, UINT flags)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(kShaderSource, strlen(kShaderSource));
    mix(entry, strlen(entry));
    mix(target, strlen(target));
    mix(&flags, sizeof(flags));
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path() / "cache" / "shaders" / std::format("{}_{:016x}.cso", entry, h);
}

bool Compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out, std::string& error)
{
    ComPtr<ID3DBlob> errors;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const auto cacheFile = ShaderCacheFile(entry, target, flags);
    if (std::ifstream f(cacheFile, std::ios::binary | std::ios::ate); f)
    {
        const auto size = size_t(f.tellg());
        f.seekg(0);
        if (size && SUCCEEDED(D3DCreateBlob(size, &out)) && f.read(static_cast<char*>(out->GetBufferPointer()), std::streamsize(size))) return true;
        out.Reset();
    }
    HRESULT hr = D3DCompile(kShaderSource, strlen(kShaderSource), "ArmaView.hlsl", nullptr, nullptr,
                            entry, target, flags, 0, &out, &errors);
    if (FAILED(hr))
    {
        error = std::format("Shader {} failed: {}", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(cacheFile.parent_path(), ec);
    std::ofstream o(cacheFile, std::ios::binary | std::ios::trunc);
    o.write(static_cast<const char*>(out->GetBufferPointer()), std::streamsize(out->GetBufferSize()));
    return true;
}


} // namespace

bool Renderer::Init(HWND hwnd, std::string& error)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    width_ = std::max<UINT>(1, rc.right - rc.left);
    height_ = std::max<UINT>(1, rc.bottom - rc.top);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = width_;
    sd.BufferDesc.Height = height_;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &swapChain_, &device_, nullptr, &ctx_);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) // debug layer not installed
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &swapChain_, &device_, nullptr, &ctx_);
    if (FAILED(hr)) // no GPU (a CI runner, a remote session): the WARP software rasterizer
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &swapChain_, &device_, nullptr, &ctx_);
    if (FAILED(hr))
    {
        error = std::format("D3D11CreateDeviceAndSwapChain failed (hr=0x{:08X})", unsigned(hr));
        return false;
    }

    {
        PerfNoteScope ps("startup: shaders (compile or cache)");
        if (!CreateShaders(error)) return false;
    }

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(SceneConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&cb, nullptr, &constants_);
    cb.ByteWidth = sizeof(MaterialConstants);
    device_->CreateBuffer(&cb, nullptr, &materialConstants_);

    D3D11_SAMPLER_DESC sdesc{};
    sdesc.Filter = D3D11_FILTER_ANISOTROPIC;
    sdesc.MaxAnisotropy = 8;
    sdesc.AddressU = sdesc.AddressV = sdesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sdesc.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sdesc, &sampler_);

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&bd, &blendAlpha_);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.DepthClipEnable = TRUE;
    const D3D11_CULL_MODE culls[3] = { D3D11_CULL_NONE, D3D11_CULL_BACK, D3D11_CULL_FRONT };
    for (int i = 0; i < 3; ++i)
    {
        rd.CullMode = culls[i];
        device_->CreateRasterizerState(&rd, &rsSolid_[i]);
    }
    rd.CullMode = D3D11_CULL_NONE;
    device_->CreateRasterizerState(&rd, &rsLines_);
    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.DepthBias = -200;
    rd.SlopeScaledDepthBias = -1.0f;
    device_->CreateRasterizerState(&rd, &rsWire_);

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    device_->CreateDepthStencilState(&dd, &dsDefault_);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device_->CreateDepthStencilState(&dd, &dsNoWrite_);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device_->CreateDepthStencilState(&dd, &dsNoDepth_);

    CreateTargets();
    return true;
}

bool Renderer::CreateShaders(std::string& error)
{
    ComPtr<ID3DBlob> vsMesh, psMesh, vsLine, psLine;
    if (!Compile("VSMesh", "vs_5_0", vsMesh, error) || !Compile("PSMesh", "ps_5_0", psMesh, error) ||
        !Compile("VSLine", "vs_5_0", vsLine, error) || !Compile("PSLine", "ps_5_0", psLine, error))
        return false;

    device_->CreateVertexShader(vsMesh->GetBufferPointer(), vsMesh->GetBufferSize(), nullptr, &meshVS_);
    device_->CreatePixelShader(psMesh->GetBufferPointer(), psMesh->GetBufferSize(), nullptr, &meshPS_);
    device_->CreateVertexShader(vsLine->GetBufferPointer(), vsLine->GetBufferSize(), nullptr, &lineVS_);
    device_->CreatePixelShader(psLine->GetBufferPointer(), psLine->GetBufferSize(), nullptr, &linePS_);

    const D3D11_INPUT_ELEMENT_DESC meshDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(MeshVertex, pos),    D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(MeshVertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(MeshVertex, uv),     D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(MeshVertex, uv1),    D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(MeshVertex, color),  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "BLENDINDICES", 0, DXGI_FORMAT_R32_UINT,       1, 0,                            D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    const D3D11_INPUT_ELEMENT_DESC lineDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(LineVertex, pos),   D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(LineVertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(device_->CreateInputLayout(meshDesc, 6, vsMesh->GetBufferPointer(), vsMesh->GetBufferSize(), &meshLayout_)) ||
        FAILED(device_->CreateInputLayout(lineDesc, 2, vsLine->GetBufferPointer(), vsLine->GetBufferSize(), &lineLayout_)))
    {
        error = "CreateInputLayout failed";
        return false;
    }
    return true;
}

void Renderer::CreateTargets()
{
    ComPtr<ID3D11Texture2D> back;
    swapChain_->GetBuffer(0, IID_PPV_ARGS(&back));
    device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R24G8_TYPELESS; // typeless: can be copied to a staging texture for CaptureDepth
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    depthTex_.Reset();
    depthStaging_.Reset();
    depthCpu_.clear();
    device_->CreateTexture2D(&td, nullptr, &depthTex_);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(depthTex_.Get(), &dd, &dsv_);
}

void Renderer::CaptureDepth()
{
    if (!depthTex_) return;
    if (!depthStaging_)
    {
        D3D11_TEXTURE2D_DESC td;
        depthTex_->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &depthStaging_))) return;
    }
    ctx_->CopyResource(depthStaging_.Get(), depthTex_.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(depthStaging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    depthW_ = width_;
    depthH_ = height_;
    depthCpu_.resize(size_t(depthW_) * depthH_);
    for (UINT y = 0; y < depthH_; ++y)
    {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
        float* out = depthCpu_.data() + size_t(y) * depthW_;
        for (UINT x = 0; x < depthW_; ++x) out[x] = float(row[x] & 0xFFFFFF) / 16777215.0f;
    }
    ctx_->Unmap(depthStaging_.Get(), 0);
}

float Renderer::DepthAt(int x, int y) const
{
    if (x < 0 || y < 0 || x >= int(depthW_) || y >= int(depthH_) || depthCpu_.empty()) return 1.0f;
    return depthCpu_[size_t(y) * depthW_ + x];
}

void Renderer::Resize(UINT width, UINT height)
{
    if (!swapChain_ || width == 0 || height == 0 || (width == width_ && height == height_)) return;
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    dsv_.Reset();
    width_ = width;
    height_ = height;
    swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateTargets();
}

void Renderer::Shutdown()
{
    if (ctx_) ctx_->ClearState();
    *this = Renderer{};
}

void Renderer::BeginFrame(const float clearColor[4])
{
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), dsv_.Get());
    ctx_->ClearRenderTargetView(rtv_.Get(), clearColor);
    ctx_->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
}

void Renderer::Present(bool vsync) { swapChain_->Present(vsync ? 1 : 0, 0); }

bool Renderer::BeginOffscreen(UINT w, UINT h, const float clearColor[4])
{
    if (!device_ || w == 0 || h == 0) return false;
    if (w != offW_ || h != offH_ || !offRtv_)
    {
        offTex_.Reset(); offDepth_.Reset(); offStaging_.Reset(); offRtv_.Reset(); offDsv_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &offTex_)) || FAILED(device_->CreateRenderTargetView(offTex_.Get(), nullptr, &offRtv_))) return false;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &offStaging_))) return false;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &offDepth_)) || FAILED(device_->CreateDepthStencilView(offDepth_.Get(), nullptr, &offDsv_))) return false;
        offW_ = w;
        offH_ = h;
    }
    // the scene passes bind rtv_ / dsv_ (also after the shadow pass): they point to the offscreen target meanwhile
    savedRtv_ = rtv_;
    savedDsv_ = dsv_;
    rtv_ = offRtv_;
    dsv_ = offDsv_;
    BeginFrame(clearColor);
    return true;
}

bool Renderer::ReadOffscreen(std::vector<uint8_t>& rgba)
{
    if (!offTex_ || !offStaging_) return false;
    ctx_->CopyResource(offStaging_.Get(), offTex_.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(offStaging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    rgba.resize(size_t(offW_) * offH_ * 4);
    for (UINT y = 0; y < offH_; ++y) std::memcpy(rgba.data() + size_t(y) * offW_ * 4, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(offW_) * 4);
    ctx_->Unmap(offStaging_.Get(), 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    return true;
}

void Renderer::EndOffscreen()
{
    if (!savedRtv_) return;
    rtv_ = savedRtv_;
    dsv_ = savedDsv_;
    savedRtv_.Reset();
    savedDsv_.Reset();
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), dsv_.Get());
    D3D11_VIEWPORT full{ 0, 0, float(width_), float(height_), 0, 1 };
    ctx_->RSSetViewports(1, &full);
}

bool Renderer::SaveBackbufferPng(const std::wstring& path, std::string& error)
{
    ComPtr<ID3D11Texture2D> back, staging;
    swapChain_->GetBuffer(0, IID_PPV_ARGS(&back));
    D3D11_TEXTURE2D_DESC td;
    back->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging))) { error = "CreateTexture2D(staging) failed"; return false; }
    ctx_->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) { error = "Map failed"; return false; }
    // Backbuffer is RGBA8; the PNG encoder takes BGRA8 -> swizzle into a tight buffer, force opaque alpha.
    std::vector<BYTE> pixels(size_t(td.Width) * td.Height * 4);
    for (UINT y = 0; y < td.Height; ++y)
    {
        const BYTE* src = static_cast<const BYTE*>(m.pData) + size_t(y) * m.RowPitch;
        BYTE* dst = pixels.data() + size_t(y) * td.Width * 4;
        for (UINT x = 0; x < td.Width; ++x)
        {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = 255;
        }
    }
    ctx_->Unmap(staging.Get(), 0);

    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(td.Width, td.Height);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr) && fmt != GUID_WICPixelFormat32bppBGRA) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = frame->WritePixels(td.Height, td.Width * 4, UINT(pixels.size()), pixels.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (FAILED(hr)) { error = std::format("WIC PNG encode failed (hr=0x{:08X})", unsigned(hr)); return false; }
    return true;
}

void Renderer::Upload(ComPtr<ID3D11Buffer>& vb, UINT& capacity, const void* data, size_t bytes)
{
    if (!bytes) return;
    if (!vb || bytes > capacity)
    {
        vb.Reset();
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = UINT(bytes + bytes / 4);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateBuffer(&bd, nullptr, &vb))) { capacity = 0; return; }
        capacity = bd.ByteWidth;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx_->Map(vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        std::memcpy(m.pData, data, bytes);
        ctx_->Unmap(vb.Get(), 0);
    }
}

void Renderer::SetMesh(std::span<const MeshVertex> v)
{
    // default usage: uploaded on geometry changes; SetMeshRange updates parts of it
    if (v.empty()) { meshCount_ = 0; return; }
    if (!meshVB_ || v.size_bytes() > meshCapacity_)
    {
        meshVB_.Reset();
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = UINT(v.size_bytes());
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        if (FAILED(device_->CreateBuffer(&bd, nullptr, &meshVB_))) { meshCapacity_ = meshCount_ = 0; return; }
        meshCapacity_ = bd.ByteWidth;
    }
    D3D11_BOX box{ 0, 0, 0, UINT(v.size_bytes()), 1, 1 };
    ctx_->UpdateSubresource(meshVB_.Get(), 0, &box, v.data(), 0, 0);
    meshCount_ = UINT(v.size());
}

void Renderer::SetMeshRange(UINT first, std::span<const MeshVertex> v)
{
    if (!meshVB_ || v.empty() || first + v.size() > meshCount_) return;
    D3D11_BOX box{ UINT(first * sizeof(MeshVertex)), 0, 0, UINT((first + v.size()) * sizeof(MeshVertex)), 1, 1 };
    ctx_->UpdateSubresource(meshVB_.Get(), 0, &box, v.data(), 0, 0);
}

void Renderer::SetVertexPoints(std::span<const uint32_t> idx)
{
    pointVB_.Reset();
    pointVBCount_ = 0;
    if (idx.empty()) return;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = UINT(idx.size_bytes());
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{ idx.data(), 0, 0 };
    if (SUCCEEDED(device_->CreateBuffer(&bd, &sd, &pointVB_))) pointVBCount_ = UINT(idx.size());
}

void Renderer::SetPointMatrices(std::span<const glm::mat4> m, bool appendIdentity)
{
    pointMatCount_ = UINT(m.size()) + (appendIdentity ? 1 : 0);
    if (m.empty()) { pointMatCount_ = 0; return; }
    const size_t total = pointMatCount_;
    if (!pointMatBuf_ || total > pointMatCapacity_)
    {
        pointMatBuf_.Reset();
        pointMatSRV_.Reset();
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = UINT(total * sizeof(glm::mat4));
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(glm::mat4);
        if (FAILED(device_->CreateBuffer(&bd, nullptr, &pointMatBuf_))) { pointMatCount_ = 0; return; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = UINT(total);
        device_->CreateShaderResourceView(pointMatBuf_.Get(), &sd, &pointMatSRV_);
        pointMatCapacity_ = UINT(total);
    }
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(ctx_->Map(pointMatBuf_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
    {
        std::memcpy(map.pData, m.data(), m.size_bytes());
        if (appendIdentity)
        {
            const glm::mat4 id(1.0f);
            std::memcpy(static_cast<uint8_t*>(map.pData) + m.size_bytes(), &id, sizeof(id));
        }
        ctx_->Unmap(pointMatBuf_.Get(), 0);
    }
}

// Binds the point-index stream and the matrices for mesh draws (on) or nothing (ghosts, ground: pre-transformed).
void Renderer::BindSkin(bool on)
{
    skinNow_ = on && pointMatCount_ && pointMatSRV_ && pointVB_ && pointVBCount_ >= meshCount_;
    ID3D11Buffer* vb = skinNow_ || pointVB_ ? pointVB_.Get() : nullptr;
    UINT stride = sizeof(uint32_t), offset = 0;
    ctx_->IASetVertexBuffers(1, 1, &vb, &stride, &offset);
    ID3D11ShaderResourceView* srv = skinNow_ ? pointMatSRV_.Get() : nullptr;
    ctx_->VSSetShaderResources(7, 1, &srv);
}

void Renderer::SetLines(LineLayer layer, std::span<const LineVertex> v)
{
    auto i = size_t(layer);
    Upload(lineVB_[i], lineCapacity_[i], v.data(), v.size_bytes());
    lineCount_[i] = lineVB_[i] ? UINT(v.size()) : 0;
}

void Renderer::SetGhosts(std::span<const MeshVertex> v, std::vector<Ghost> ghosts)
{
    ghosts_ = std::move(ghosts);
    if (v.empty()) { ghosts_.clear(); return; }
    Upload(ghostVB_, ghostCapacity_, v.data(), v.size_bytes());
}

void Renderer::UpdateConstants(const SceneDraw& s, float overrideMode, const glm::vec4& overrideColor)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    auto* c = static_cast<SceneConstants*>(m.pData);
    c->viewProj = s.viewProj;
    c->lightDir = glm::vec4(glm::normalize(s.lightDir), 0);
    c->cameraPos = glm::vec4(s.cameraPos, 1);
    c->overrideColor = overrideColor;
    c->params = glm::vec4(overrideMode, float(int(s.shading)), s.twoSided ? 1.0f : 0.0f, s.flipNormalGreen ? 1.0f : 0.0f);
    c->lightColor = glm::vec4(s.lightColor, 1);
    c->ambientColor = glm::vec4(s.ambientColor, s.reflections ? 1.0f : 0.0f);
    c->groundColor = glm::vec4(s.groundColor, s.exposure);
    c->lightViewProj = s.lightViewProj;
    c->skinParams = glm::vec4(skinNow_ ? 1.0f : 0.0f, 0, 0, 0);
    static const float taps[3] = { 2, 4, 5 };
    c->shadowParams = glm::vec4(s.shadows && shadowSRV_ ? 1.0f : 0.0f, s.shadowSoftness, shadowSize_ ? 1.0f / float(shadowSize_) : 0.0f,
                                taps[std::clamp(s.shadowQuality, 0, 2)]);
    ctx_->Unmap(constants_.Get(), 0);
}

void Renderer::UpdateMaterial(const MaterialConstants& m)
{
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx_->Map(materialConstants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    std::memcpy(map.pData, &m, sizeof(m));
    ctx_->Unmap(materialConstants_.Get(), 0);
}

const char* ShadingModeName(ShadingMode m)
{
    static const char* names[] = { "Lit (no textures)", "UV checker", "Normals as color", "Textured (super shader approx.)",
                                   "Map: diffuse _co", "Map: normal _nohq", "Map: specular _smdi", "Map: ambient shadow _as",
                                   "Map: macro _mc", "Map: detail _dt", "Diag: tangent (dP/du)", "Diag: bitangent (dP/dv)",
                                   "Diag: shading normal (with _nohq)" };
    return names[int(m)];
}

void Renderer::EnsureShadowMap(int size)
{
    if (shadowSize_ == size && shadowTex_) return;
    shadowTex_.Reset();
    shadowDSV_.Reset();
    shadowSRV_.Reset();
    shadowSize_ = 0;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = UINT(size);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &shadowTex_))) return;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(device_->CreateDepthStencilView(shadowTex_.Get(), &dd, &shadowDSV_)) ||
        FAILED(device_->CreateShaderResourceView(shadowTex_.Get(), &sd, &shadowSRV_)))
    {
        shadowTex_.Reset();
        return;
    }
    if (!shadowSampler_)
    {
        D3D11_SAMPLER_DESC smp{};
        smp.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        smp.BorderColor[0] = smp.BorderColor[1] = smp.BorderColor[2] = smp.BorderColor[3] = 1.0f;
        smp.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        device_->CreateSamplerState(&smp, &shadowSampler_);
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        rd.DepthBias = 0;
        rd.SlopeScaledDepthBias = 1.5f;
        device_->CreateRasterizerState(&rd, &rsShadow_);
    }
    shadowSize_ = size;
}

// Depth from the sun into the shadow map (all visible sections, alpha-tested ones as solid).
IDXGIAdapter* Renderer::Adapter() const
{
    static ComPtr<IDXGIAdapter> adapter;
    if (!adapter)
    {
        ComPtr<IDXGIDevice> dx;
        if (SUCCEEDED(device_.As(&dx))) dx->GetAdapter(&adapter);
    }
    return adapter.Get();
}

void Renderer::DrawShadowPass(const SceneDraw& s)
{
    Perf::Scope scope("render: shadow pass");
    static const int sizes[3] = { 1024, 2048, 4096 };
    EnsureShadowMap(sizes[std::clamp(s.shadowQuality, 0, 2)]);
    if (!shadowDSV_ || !meshCount_) return;
    ID3D11ShaderResourceView* none[1] = {};
    ctx_->PSSetShaderResources(6, 1, none);
    ctx_->OMSetRenderTargets(0, nullptr, shadowDSV_.Get());
    ctx_->ClearDepthStencilView(shadowDSV_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    D3D11_VIEWPORT vp{ 0, 0, float(shadowSize_), float(shadowSize_), 0, 1 };
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(rsShadow_.Get());
    ctx_->OMSetDepthStencilState(dsDefault_.Get(), 0);
    SceneDraw ls = s;
    ls.viewProj = s.lightViewProj;
    ls.shadows = false;
    BindSkin(true);
    UpdateConstants(ls, 0.0f, {});
    UINT stride = sizeof(MeshVertex), offset = 0;
    ctx_->IASetInputLayout(meshLayout_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetVertexBuffers(0, 1, meshVB_.GetAddressOf(), &stride, &offset);
    ctx_->VSSetShader(meshVS_.Get(), nullptr, 0);
    ctx_->PSSetShader(nullptr, nullptr, 0);
    if (sections_.empty()) ctx_->Draw(meshCount_, 0);
    else
        for (const auto& sec : sections_)
            if (sec.count && sec.first + sec.count <= meshCount_ && sec.material.params.y < 1.5f) ctx_->Draw(sec.count, sec.first);
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), dsv_.Get());
}

void Renderer::DrawScene(const SceneDraw& s)
{
    Perf::Scope scope("render: scene (incl. shadows)");
    if (s.shadows && s.drawMesh) DrawShadowPass(s);
    D3D11_VIEWPORT vp{ s.viewportX, s.viewportY, std::max(1.0f, s.viewportW), std::max(1.0f, s.viewportH), 0.0f, 1.0f };
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetDepthStencilState(dsDefault_.Get(), 0);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx_->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    ctx_->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());

    // Mesh passes
    if (s.drawMesh && meshCount_)
    {
        UINT stride = sizeof(MeshVertex), offset = 0;
        ctx_->IASetInputLayout(meshLayout_.Get());
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->IASetVertexBuffers(0, 1, meshVB_.GetAddressOf(), &stride, &offset);
        ctx_->VSSetShader(meshVS_.Get(), nullptr, 0);
        ctx_->PSSetShader(meshPS_.Get(), nullptr, 0);
        BindSkin(true);

        if (s.wire != WireMode::Only)
        {
            UpdateConstants(s, 0.0f, {});
            ctx_->RSSetState(rsSolid_[int(s.cull)].Get());
            ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
            ID3D11ShaderResourceView* sh = s.shadows ? shadowSRV_.Get() : nullptr;
            ctx_->PSSetShaderResources(6, 1, &sh);
            ctx_->PSSetSamplers(1, 1, shadowSampler_.GetAddressOf());
            ctx_->PSSetConstantBuffers(1, 1, materialConstants_.GetAddressOf());
            if (sections_.empty())
            {
                UpdateMaterial(MaterialConstants{});
                ctx_->Draw(meshCount_, 0);
            }
            else
                for (int pass = 0; pass < 2; ++pass) // 0: opaque + alpha test, 1: alpha blend
                {
                    if (pass == 1)
                    {
                        ctx_->OMSetBlendState(blendAlpha_.Get(), nullptr, 0xFFFFFFFF);
                        ctx_->OMSetDepthStencilState(dsNoWrite_.Get(), 0);
                    }
                    for (const auto& sec : sections_)
                    {
                        bool blend = sec.material.params.y > 1.5f;
                        if (blend != (pass == 1) || !sec.count || sec.first + sec.count > meshCount_) continue;
                        UpdateMaterial(sec.material);
                        ctx_->PSSetShaderResources(0, kMaterialSlots, sec.srv);
                        ctx_->PSSetShaderResources(8, kMultiSlots, sec.multi);
                        ctx_->Draw(sec.count, sec.first);
                    }
                }
            ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ctx_->OMSetDepthStencilState(dsDefault_.Get(), 0);
            ID3D11ShaderResourceView* none[kMaterialSlots] = {};
            ctx_->PSSetShaderResources(0, kMaterialSlots, none);
        }
        if (s.shadows && shadowSRV_) // ground plane receiving the shadow
        {
            const float g = s.groundSize, y = s.groundY;
            const glm::vec2 c0 = s.groundCenter;
            MeshVertex q[6];
            glm::vec3 corners[4] = { { c0.x - g, y, c0.y - g }, { c0.x + g, y, c0.y - g }, { c0.x + g, y, c0.y + g }, { c0.x - g, y, c0.y + g } };
            int idx[6] = { 0, 1, 2, 0, 2, 3 };
            for (int k = 0; k < 6; ++k) q[k] = { corners[idx[k]], glm::vec3(0, 1, 0), glm::vec2(0), glm::vec2(0), glm::vec4(0) };
            Upload(groundVB_, groundCapacity_, q, sizeof(q));
            ctx_->IASetVertexBuffers(0, 1, groundVB_.GetAddressOf(), &stride, &offset);
            BindSkin(false);
            UpdateConstants(s, 3.0f, {});
            ctx_->RSSetState(rsSolid_[0].Get());
            ctx_->OMSetBlendState(blendAlpha_.Get(), nullptr, 0xFFFFFFFF);
            ctx_->OMSetDepthStencilState(dsNoWrite_.Get(), 0);
            ID3D11ShaderResourceView* sh = shadowSRV_.Get();
            ctx_->PSSetShaderResources(6, 1, &sh);
            ctx_->Draw(6, 0);
            ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ctx_->OMSetDepthStencilState(dsDefault_.Get(), 0);
            ctx_->IASetVertexBuffers(0, 1, meshVB_.GetAddressOf(), &stride, &offset);
            BindSkin(true);
        }
        {
            ID3D11ShaderResourceView* none[1] = {};
            ctx_->PSSetShaderResources(6, 1, none);
        }
        if (!ghosts_.empty() && ghostVB_)
        {
            ctx_->IASetVertexBuffers(0, 1, ghostVB_.GetAddressOf(), &stride, &offset);
            BindSkin(false);
            ctx_->RSSetState(rsSolid_[0].Get());
            ctx_->OMSetBlendState(blendAlpha_.Get(), nullptr, 0xFFFFFFFF);
            ctx_->OMSetDepthStencilState(dsNoWrite_.Get(), 0);
            for (const auto& g : ghosts_)
            {
                UpdateConstants(s, 2.0f, g.color);
                ctx_->Draw(g.count, g.first);
            }
            ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ctx_->OMSetDepthStencilState(dsDefault_.Get(), 0);
            ctx_->IASetVertexBuffers(0, 1, meshVB_.GetAddressOf(), &stride, &offset);
            BindSkin(true);
        }
        if (s.wire != WireMode::Off)
        {
            glm::vec4 color = s.wire == WireMode::Only ? glm::vec4(0.75f, 0.8f, 0.85f, 1.0f) : s.wireColor;
            UpdateConstants(s, 1.0f, color);
            ctx_->RSSetState(rsWire_.Get());
            ctx_->Draw(meshCount_, 0);
        }
    }

    // Line layers
    skinNow_ = false;
    UpdateConstants(s, 0.0f, {});
    ctx_->IASetInputLayout(lineLayout_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ctx_->VSSetShader(lineVS_.Get(), nullptr, 0);
    ctx_->PSSetShader(linePS_.Get(), nullptr, 0);
    ctx_->RSSetState(rsLines_.Get());
    for (size_t i = 0; i < size_t(LineLayer::Count); ++i)
    {
        if (!s.drawLayer[i] || !lineCount_[i]) continue;
        ctx_->OMSetDepthStencilState(s.layerOnTop[i] ? dsNoDepth_.Get() : dsDefault_.Get(), 0);
        UINT stride = sizeof(LineVertex), offset = 0;
        ctx_->IASetVertexBuffers(0, 1, lineVB_[i].GetAddressOf(), &stride, &offset);
        ctx_->Draw(lineCount_[i], 0);
    }

    // Restore full viewport for ImGui
    D3D11_VIEWPORT full{ 0, 0, float(width_), float(height_), 0, 1 };
    ctx_->RSSetViewports(1, &full);
}
