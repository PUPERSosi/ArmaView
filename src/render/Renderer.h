#pragma once
// D3D11 device/swap chain plus the scene passes: lit mesh, wireframe overlay, line layers (grid, normals).
#include "Mesh.h" // MeshVertex, LineVertex
#include <d3d11.h>
#include <wrl/client.h>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// Shader mode ids (keep in sync with PSMesh): untextured lit, UV checker, normals, textured (super-shader approximation),
// then single texture maps on the model.
enum class ShadingMode { Lit, UVChecker, NormalColor, Textured, MapDiffuse, MapNormal, MapSpecular, MapAmbientShadow, MapMacro, MapDetail,
                         DiagTangent, DiagBitangent, DiagShadedNormal, Count };
const char* ShadingModeName(ShadingMode m);

constexpr int kMaterialSlots = 6; // diffuse, normal, specular(smdi), ambient shadow, macro, detail
// Multi (multimaterial) layers, registers t8..: 4 x _co, mask, 4 x _dtsmdi, 4 x _nohq (stages 0-3, 4, 5-8, 11-14)
constexpr int kMultiSlots = 13;

// Per-section material constants (register b1).
struct MaterialConstants
{
    glm::vec4 ambient{ 1 }, diffuse{ 1 }, forcedDiffuse{ 0 }, emissive{ 0 }, specular{ 0 };
    glm::vec4 params{ 1, 0, 0, 0 }; // x specularPower, y alpha mode (0 opaque, 1 test, 2 blend)
    glm::vec4 uvA[kMaterialSlots];  // (aside.x, aside.y, up.x, up.y)
    glm::vec4 uvB[kMaterialSlots];  // (pos.x, pos.y, uvSource 0/1, 0)
    glm::vec4 env{ 0 };             // x 1 = reflections (fresnel + env map), y fresnel n, z fresnel k
    glm::mat4 nmSwizzle{ 1.0f };    // normal map: original rgba = nmSwizzle * sampled + nmSwizzleOffset (PAA SWIZ)
    glm::vec4 nmSwizzleOffset{ 0 };
    glm::vec4 multi{ 0 };           // x 1 = Multi shader (layers blended by the mask)
    glm::vec4 skin{ 0 };            // x 1 = Skin shader: emmisive = scatter colour / amount, forcedDiffuse = rim translucency
    glm::vec4 muvA[kMultiSlots];    // per Multi slot uvTransform, as uvA / uvB
    glm::vec4 muvB[kMultiSlots];
    MaterialConstants()
    {
        for (int i = 0; i < kMaterialSlots; ++i) { uvA[i] = { 1, 0, 0, 1 }; uvB[i] = { 0, 0, 0, 0 }; }
        for (int i = 0; i < kMultiSlots; ++i) { muvA[i] = { 1, 0, 0, 1 }; muvB[i] = { 0, 0, 0, 0 }; }
    }
};

// A range of the mesh vertex buffer drawn with one material.
struct DrawSection
{
    UINT first = 0, count = 0;
    ID3D11ShaderResourceView* srv[kMaterialSlots] = {};
    ID3D11ShaderResourceView* multi[kMultiSlots] = {};
    MaterialConstants material;
};
enum class WireMode { Off, Overlay, Only };
enum class CullMode { None, Back, Front };
enum class LineLayer { Grid, Normals, Skeleton, Proxies, Trails, LodOverlay, Retarget, Footsteps, Weapon, Count };

struct SceneDraw
{
    glm::mat4 viewProj{ 1.0f };
    glm::vec3 cameraPos{ 0.0f };
    glm::vec3 lightDir{ 0.0f, -1.0f, 0.0f }; // direction light travels
    float viewportX = 0, viewportY = 0, viewportW = 1, viewportH = 1;
    ShadingMode shading = ShadingMode::Lit;
    WireMode wire = WireMode::Off;
    CullMode cull = CullMode::None;
    bool twoSided = true;
    bool flipNormalGreen = false;
    bool reflections = true;        // env map (sky from the lighting) x fresnel on materials that have them
    // sun shadows: one shadow map fitted to the model, PCF; a ground plane at groundY receives them
    bool shadows = false;
    float shadowSoftness = 1.5f;    // filter radius in shadow-map texels
    int shadowQuality = 1;          // 0: 1024, 1: 2048, 2: 4096 (map size and filter taps)
    glm::mat4 lightViewProj{ 1.0f };
    float groundY = 0.0f, groundSize = 10.0f;
    glm::vec2 groundCenter{ 0.0f };
    glm::vec3 lightColor{ 1.0f }, ambientColor{ 0.35f }, groundColor{ 0.15f }; // ambient: sky (from above) / ground (below)
    float exposure = 1.0f;
    bool drawMesh = true;
    bool drawLayer[size_t(LineLayer::Count)] = { true, false, false, true, true };
    bool layerOnTop[size_t(LineLayer::Count)] = { false, false, true, false, true }; // draw without depth test
    glm::vec4 wireColor{ 0.05f, 0.05f, 0.05f, 1.0f };
};

class Renderer
{
public:
    bool Init(HWND hwnd, std::string& error);
    void Shutdown();
    void Resize(UINT width, UINT height);

    void BeginFrame(const float clearColor[4]);
    void DrawScene(const SceneDraw& s);
    void Present(bool vsync);
    bool SaveBackbufferPng(const std::wstring& path, std::string& error); // call after drawing, before Present
    // Offscreen target of any size (recording without the UI): DrawScene draws there between Begin and End.
    bool BeginOffscreen(UINT width, UINT height, const float clearColor[4]);
    bool ReadOffscreen(std::vector<uint8_t>& rgba, bool keepAlpha = false); // RGBA8, tight rows; keepAlpha: the drawn alpha (pictures), else opaque
    void EndOffscreen();
    // Copies the scene depth buffer to the CPU (after DrawScene); DepthAt reads it (0 near .. 1 far, 1 outside).
    void CaptureDepth();
    float DepthAt(int x, int y) const;
    bool HasDepth() const { return !depthCpu_.empty(); }

    // Dynamic buffers: cheap to call every frame (re-created only when they must grow).
    void SetMesh(std::span<const MeshVertex> vertices);           // whole mesh (on geometry changes)
    void SetMeshRange(UINT first, std::span<const MeshVertex> vertices); // part of it (attachments per frame)
    // GPU skinning: per vertex the index of its point matrix (static), per frame the point matrices.
    // An empty matrix list switches skinning off (rest pose).
    void SetVertexPoints(std::span<const uint32_t> pointIndex);
    void SetPointMatrices(std::span<const glm::mat4> matrices, bool appendIdentity = false);
    void SetLines(LineLayer layer, std::span<const LineVertex> vertices);
    // Material ranges of the mesh (opaque/alpha-test first, then blended sections are drawn last).
    void SetSections(std::vector<DrawSection> sections) { sections_ = std::move(sections); }
    // Semi-transparent extra poses (onion skin, A/B compare): vertex ranges drawn lit with a tint and alpha.
    struct Ghost { UINT first = 0, count = 0; glm::vec4 color{ 1 }; };
    void SetGhosts(std::span<const MeshVertex> vertices, std::vector<Ghost> ghosts);

    ID3D11Device* Device() const { return device_.Get(); }
    IDXGIAdapter* Adapter() const; // for the VRAM report
    ID3D11DeviceContext* Context() const { return ctx_.Get(); }
    UINT Width() const { return width_; }
    UINT Height() const { return height_; }

private:
    bool CreateShaders(std::string& error);
    void CreateTargets();
    void UpdateConstants(const SceneDraw& s, float overrideMode, const glm::vec4& overrideColor);
    void UpdateMaterial(const MaterialConstants& m);
    void Upload(ComPtr<ID3D11Buffer>& vb, UINT& capacity, const void* data, size_t bytes);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain> swapChain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11DepthStencilView> dsv_;
    ComPtr<ID3D11Texture2D> offTex_, offDepth_, offStaging_;
    ComPtr<ID3D11RenderTargetView> offRtv_, savedRtv_;
    ComPtr<ID3D11DepthStencilView> offDsv_, savedDsv_;
    UINT offW_ = 0, offH_ = 0;
    ComPtr<ID3D11Texture2D> depthTex_, depthStaging_;
    std::vector<float> depthCpu_;
    UINT depthW_ = 0, depthH_ = 0;
    UINT width_ = 0, height_ = 0;

    ComPtr<ID3D11VertexShader> meshVS_, lineVS_;
    ComPtr<ID3D11PixelShader> meshPS_, linePS_;
    ComPtr<ID3D11InputLayout> meshLayout_, lineLayout_;
    ComPtr<ID3D11Buffer> constants_, materialConstants_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blendAlpha_;
    ComPtr<ID3D11DepthStencilState> dsNoWrite_;
    std::vector<DrawSection> sections_;
    ComPtr<ID3D11RasterizerState> rsSolid_[3], rsWire_, rsLines_;
    ComPtr<ID3D11DepthStencilState> dsDefault_, dsNoDepth_;

    void EnsureShadowMap(int size);
    void DrawShadowPass(const SceneDraw& s);
    ComPtr<ID3D11Texture2D> shadowTex_;
    ComPtr<ID3D11DepthStencilView> shadowDSV_;
    ComPtr<ID3D11ShaderResourceView> shadowSRV_;
    ComPtr<ID3D11SamplerState> shadowSampler_;
    ComPtr<ID3D11RasterizerState> rsShadow_;
    ComPtr<ID3D11Buffer> groundVB_;
    UINT groundCapacity_ = 0;
    int shadowSize_ = 0;
    ComPtr<ID3D11Buffer> ghostVB_;
    UINT ghostCapacity_ = 0;
    std::vector<Ghost> ghosts_;
    ComPtr<ID3D11Buffer> pointVB_, pointMatBuf_;
    ComPtr<ID3D11ShaderResourceView> pointMatSRV_;
    UINT pointVBCount_ = 0, pointMatCapacity_ = 0, pointMatCount_ = 0;
    bool skinNow_ = false; // skinning flag written by UpdateConstants
    void BindSkin(bool on);
    ComPtr<ID3D11Buffer> meshVB_;
    UINT meshCount_ = 0, meshCapacity_ = 0;
    ComPtr<ID3D11Buffer> lineVB_[size_t(LineLayer::Count)];
    UINT lineCount_[size_t(LineLayer::Count)] = {};
    UINT lineCapacity_[size_t(LineLayer::Count)] = {};
};
