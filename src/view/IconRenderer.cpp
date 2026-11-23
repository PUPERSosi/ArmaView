#include "IconRenderer.h"
#include "Log.h"
#include "Text.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace
{
constexpr float kPi = 3.14159265f;
// the grey-green of BI's editor previews (VR sphere color(0.93,1,0.98,0.1) as it comes out in their JPGs)
constexpr float kVrBackground[4] = { 0.541f, 0.596f, 0.588f, 1.0f };
} // namespace

IconSettings DefaultIconSettings(IconPreset p, bool vehicle, bool character)
{
    const IconStandard& st = Standard(p);
    IconSettings s;
    s.preset = p;
    s.width = st.width;
    s.height = st.height;
    s.background = st.background;
    s.whiteSilhouette = st.whiteSilhouette;
    s.yawDeg = st.yawDeg;
    s.pitchDeg = st.pitchDeg;
    s.fovDeg = st.fovDeg;
    s.marginL = st.marginL;
    s.marginR = st.marginR;
    s.marginT = st.marginT;
    s.marginB = st.marginB;
    s.align = st.align;
    s.barrelSide = p == IconPreset::Weapon || p == IconPreset::Attachment;
    if (p == IconPreset::EditorPreview)
    {
        // BIS_fnc_exportEditorPreviews: vehicles setDir 90, other objects 270, the camera at bearing 135, 15 deg up
        s.yawDeg = vehicle ? 45.0f : -135.0f;
        s.characterCloseUp = character;
    }
    return s;
}

bool IconRenderer::Render(const IconSettings& in, RgbaImage& out, std::string& error)
{
    if (!s_.hasModel) { error = "no model"; return false; }
    const bool preview = in.background == IconBackground::VrGround;
    const int ss = std::clamp(in.supersample, 1, 8);
    int rw = in.width * ss, rh = in.height * ss;
    const float maxSide = 4096.0f;
    if (rw > maxSide || rh > maxSide)
    {
        const float k = maxSide / float(std::max(rw, rh));
        rw = int(rw * k);
        rh = int(rh * k);
    }
    const float aspect = float(rw) / float(rh);
    glm::vec3 bmin = view_.boundsMin, bmax = view_.boundsMax;
    const glm::vec3 center = (bmin + bmax) * 0.5f;
    const float size = std::max(glm::length(bmax - bmin), 0.05f);

    OrbitCamera cam;
    cam.yaw = kPi + glm::radians(in.yawDeg); // 0: the model's front (Front view), +90: its right side
    if (in.barrelSide)
    {
        // the barrel: muzzle memory point minus its end point (the weapon's config names), else the longer axis
        glm::vec3 axis(0.0f);
        const P3DLod* memory = nullptr;
        for (const auto& l : s_.model.lods)
            if (LodResolutionName(l.resolution) == "Memory") memory = &l;
        auto pointOf = [&](const std::string& name, glm::vec3& out) {
            if (!memory || name.empty()) return false;
            for (const auto& sel : memory->selections)
                if (IEquals(sel.name, name))
                    for (size_t p = 0; p < sel.pointBytes.size() && p < memory->points.size(); ++p)
                        if (sel.pointBytes[p]) { out = memory->points[p].pos; return true; }
            return false;
        };
        glm::vec3 muzzle(0.0f), end(0.0f);
        if (pointOf(s_.weapon.muzzlePos.empty() ? "usti hlavne" : s_.weapon.muzzlePos, muzzle) &&
            pointOf(s_.weapon.muzzleEnd.empty() ? "konec hlavne" : s_.weapon.muzzleEnd, end))
            axis = glm::vec3(muzzle.x - end.x, 0.0f, muzzle.z - end.z);
        if (glm::length(axis) < 1e-4f) axis = (bmax.x - bmin.x) > (bmax.z - bmin.z) ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, -1);
        axis = glm::normalize(axis);
        // the camera across it, on the side that puts the muzzle on the left of the picture
        glm::vec3 c(axis.z, 0.0f, -axis.x);
        const glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0, 1, 0), -c));
        if (glm::dot(right, axis) > 0) c = -c;
        cam.yaw = std::atan2(c.x, c.z) + glm::radians(in.yawDeg);
    }
    cam.pitch = glm::radians(std::clamp(in.pitchDeg, -89.5f, 89.5f));
    cam.target = center;
    if (preview)
    {
        cam.distance = std::clamp(1.5f * size, 0.2f, 124.0f); // BI: 1.5 x sizeOf
        if (in.characterCloseUp)
        {
            // BI: the torso (1.25 m above the feet), a narrow lens: head to thighs
            cam.target = glm::vec3(center.x, bmin.y + 1.25f, center.z);
            cam.fovY = 2.0f * std::atan(0.68f / cam.distance);
        }
        else
        {
            // BI: the narrowest field of view that keeps every corner of the bounding box in the picture
            const glm::mat4 v = cam.View();
            float need = 0.01f;
            for (int c = 0; c < 8; ++c)
            {
                const glm::vec3 p((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y, (c & 4) ? bmax.z : bmin.z);
                const glm::vec3 q = glm::vec3(v * glm::vec4(p, 1.0f));
                const float depth = std::max(std::abs(q.z), 0.01f);
                need = std::max({ need, std::abs(q.y) / depth, std::abs(q.x) / depth / aspect });
            }
            cam.fovY = 2.0f * std::atan(need * 1.03f);
        }
    }
    else
    {
        cam.fovY = glm::radians(std::clamp(in.fovDeg, 1.0f, 90.0f));
        cam.distance = size * 0.5f / std::sin(cam.fovY * 0.5f) * 1.05f; // the bounding sphere in view: fitted after
    }
    cam.distance /= std::clamp(in.zoom, 0.05f, 50.0f);
    cam.target += glm::vec3(in.offsetX, in.offsetY, in.offsetZ);
    cam.nearZ = std::max(cam.distance - size, cam.distance * 0.01f);
    cam.farZ = cam.distance + size * 2.0f + 10.0f;

    // light: BI's editor previews at 10:00 in late May; pictures lit from the camera, evenly
    Lighting light;
    light.timeOfDay = 10.0f;
    light.declination = 21.5f;
    light.headlight = !preview;
    const LightingResult lit = light.Evaluate(cam.Forward(), cam.Up(), cam.Right());

    DisplaySettings display;
    display.shadows = preview;
    display.showGrid = false;
    display.showNormals = false;
    display.showSkeleton = false;
    display.skeletonOnly = false;
    display.wire = WireMode::Off;
    SceneDraw s;
    fill_(s, lit, 0, 0, float(rw), float(rh), &display);
    const glm::mat4 roll = glm::rotate(glm::mat4(1.0f), glm::radians(in.rollDeg), glm::vec3(0, 0, 1));
    s.viewProj = cam.Projection(aspect) * roll * cam.View();
    {
        const glm::mat4 inv = glm::inverse(roll * cam.View());
        lastRight = glm::normalize(glm::vec3(inv[0]));
        lastUp = glm::normalize(glm::vec3(inv[1]));
        lastSize = size;
    }
    s.cameraPos = cam.Position();
    for (bool& l : s.drawLayer) l = false;
    s.wire = WireMode::Off;

    const float transparent[4] = { 0, 0, 0, 0 };
    std::vector<uint8_t> rgba;
    bool ok = renderer_.BeginOffscreen(UINT(rw), UINT(rh), preview ? kVrBackground : transparent);
    if (ok)
    {
        renderer_.DrawScene(s);
        ok = renderer_.ReadOffscreen(rgba, true);
    }
    renderer_.EndOffscreen();
    if (!ok) { error = "offscreen draw failed"; return false; }
    RgbaImage raw{ rw, rh, std::move(rgba) };

    if (preview)
    {
        out = Downsample(raw, std::max(1, rw / in.width));
        if (out.w != in.width || out.h != in.height) out = FitToFrame(out, in.width, in.height, 0, 0, 0, 0, false); // only when clamped
        AdjustColor(out, in.exposure, in.saturation, in.contrast);
        FlattenOn(out, uint8_t(kVrBackground[0] * 255), uint8_t(kVrBackground[1] * 255), uint8_t(kVrBackground[2] * 255));
        return true;
    }
    if (AlphaBounds(raw).Empty()) { error = "nothing visible from this view"; return false; }
    if (in.fitToBorder)
        out = FitToFrame(raw, in.width, in.height, in.marginL, in.marginR, in.marginT, in.marginB, in.align == IconAlign::Bottom);
    else
    {
        // framed by the camera itself (zoom / target): the picture as rendered
        out = Downsample(raw, std::max(1, rw / in.width));
        if (out.w != in.width || out.h != in.height) out = FitToFrame(out, in.width, in.height, 0, 0, 0, 0, false);
    }
    AdjustColor(out, in.exposure, in.saturation, in.contrast);
    if (in.whiteSilhouette) Silhouette(out, 255, 255, 255);
    Outline(out, in.outline, in.outlineColor[0], in.outlineColor[1], in.outlineColor[2], in.outlineColor[3]);
    return true;
}

ID3D11ShaderResourceView* IconRenderer::Upload(const RgbaImage& img)
{
    preview_.Reset();
    ID3D11Device* dev = renderer_.Device();
    if (!dev || img.Empty()) return nullptr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(img.w);
    td.Height = UINT(img.h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ img.px.data(), UINT(img.w) * 4, 0 };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &tex))) return nullptr;
    if (FAILED(dev->CreateShaderResourceView(tex.Get(), nullptr, &preview_))) return nullptr;
    return preview_.Get();
}
