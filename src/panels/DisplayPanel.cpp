#include "DisplayPanel.h"
#include "Lang.h"
#include "Log.h"
#include "Text.h"

#include <imgui.h>

#include <glm/glm.hpp>

#include <format>

const char* FaceDiagName(FaceDiag d)
{
    switch (d)
    {
    case FaceDiag::None: return "None";
    case FaceDiag::MirroredUV: return "Mirrored UV (magenta), zero UV area (yellow)";
    case FaceDiag::FlippedNormals: return "Normals against the face (red all, orange some)";
    case FaceDiag::RvmatPerFace: return "RVMAT per face";
    case FaceDiag::TexturePerFace: return "Texture per face";
    default: return "?";
    }
}

void DisplayPanel::DrawViewOptions()
{
    const float dpi = host_.Dpi();
    ImGui::SetNextItemWidth(220 * dpi);
    if (ui::BeginCombo("Shading", ShadingModeName(d_.shading)))
    {
        for (int m = 0; m < int(ShadingMode::Count); ++m)
            if (ui::SelectableRaw(ShadingModeName(ShadingMode(m)), int(d_.shading) == m)) d_.shading = ShadingMode(m);
        ImGui::EndCombo();
    }
    const char* wires[] = { "Off", "Overlay", "Wireframe only" };
    int w = int(d_.wire);
    ImGui::SetNextItemWidth(220 * dpi);
    if (ui::Combo("Wireframe (W)", &w, wires, 3)) d_.wire = WireMode(w);
    ui::Checkbox("Reflections (E)", &d_.reflections);
    if (ui::Hovered()) ui::SetTooltip("Super shader env map x fresnel(n,k): the env map is a sky built from the current lighting.");
    ImGui::SetNextItemWidth(220 * dpi);
    if (ui::BeginCombo("Face diagnostics", FaceDiagName(d_.faceDiag)))
    {
        for (int d = 0; d < int(FaceDiag::Count); ++d)
            if (ui::SelectableRaw(FaceDiagName(FaceDiag(d)), int(d_.faceDiag) == d))
            {
                d_.faceDiag = FaceDiag(d);
                host_.RebuildGeometry();
            }
        ImGui::EndCombo();
    }
    if (ui::Hovered()) ui::SetTooltip("Face colours over the current shading. With Shading = Diag: tangent / bitangent / shading normal\nyou see the frame the normal map is applied in.");

    ui::SeparatorText("Show");
    ui::Checkbox("Show axes", &d_.showAxes);
    ImGui::SameLine();
    {
        int lab = d_.axisLabelsBlender ? 1 : 0;
        const char* labs[] = { "Arma (Y-up)", "Blender (Z-up)" };
        ImGui::SetNextItemWidth(130 * dpi);
        if (ui::Combo("##axislabels", &lab, labs, 2)) d_.axisLabelsBlender = lab == 1;
        if (ui::Hovered()) ui::SetTooltip("Axis names of the gizmo: P3D / Arma or Blender after an a3ob import (P3D y = Blender z, P3D z = Blender y)");
    }
    ui::Checkbox("Skeleton on top", &d_.skeletonOnTop);
    ImGui::SameLine();
    ui::Checkbox("Skeleton only (hide mesh, floor)", &d_.skeletonOnly);
    if (ui::Checkbox("Show normal lines (N)", &d_.showNormals)) host_.PoseChanged();
    ImGui::SameLine();
    if (ui::Checkbox("Show proxy triangles", &d_.showProxies)) host_.RebuildGeometry();

    ui::SeparatorText("Points");
    points_.DrawControls(s_.hasModel ? &s_.model.lods[size_t(s_.lod)] : nullptr);

    if (ui::CollapsingHeader("Advanced##view"))
    {
        const char* normalModes[] = { "From file", "Recomputed (#SharpEdges#)" };
        int nm = int(d_.normalsMode);
        ImGui::SetNextItemWidth(220 * dpi);
        if (ui::Combo("Normals", &nm, normalModes, 2))
        {
            d_.normalsMode = NormalsMode(nm);
            host_.RebuildGeometry();
        }
        ImGui::SetNextItemWidth(220 * dpi);
        if (ui::SliderFloat("Normal line length", &d_.normalLength, 0.002f, 0.5f, "%.3f m", ImGuiSliderFlags_Logarithmic)) host_.RebuildGeometry();
        const char* culls[] = { "None", "Back (CW front)", "Front faces" };
        int c = int(d_.cull);
        ImGui::SetNextItemWidth(220 * dpi);
        if (ui::Combo("Culling", &c, culls, 3)) d_.cull = CullMode(c);
        ui::Checkbox("Two-sided lighting", &d_.twoSided);
        ui::Checkbox("Flip normal-map green", &d_.flipNormalGreen);
        if (ui::Checkbox("Apply PAA channel swizzle (SWIZ) to normal maps", &d_.applySwizzle)) host_.RebuildDrawSections();
        if (ui::Hovered()) ui::SetTooltip("_nohq from TexConvert keep X as 1-X in alpha (SWIZ 5 4 2 3). Off: X read from red = wrong tilt (A/B check).");
    }
    ui::SeparatorText("Camera");
    ui::Text("Target %.2f %.2f %.2f   distance %.2f   yaw %.0f   pitch %.0f", camera_.target.x, camera_.target.y, camera_.target.z, camera_.distance,
                glm::degrees(camera_.yaw), glm::degrees(camera_.pitch));
    ui::TextDisabled("LMB rotate, RMB/MMB pan, wheel zoom; 1 Front 2 Back 3 Left 4 Right 5 Top, F frame");
}

void DisplayPanel::SetWorld(const std::string& name)
{
    if (name.empty() || IEquals(name, "procedural")) { world_ = WorldLighting{}; return; }
    const ArmaConfig* cfg = s_.game.get();
    if (!cfg || !world_.Load(*cfg, name)) { Log::Get().Warn("World lighting: {} not found in CfgWorlds (LightingNew)", name); world_ = WorldLighting{}; return; }
    light_.latitude = world_.Latitude();
    light_.headlight = false;
    Log::Get().Info("World lighting: {} - {} LightingNew entries, latitude {:.1f}", name, world_.Entries(), world_.Latitude());
}

void DisplayPanel::DrawLightOptions()
{
    const float dpi = host_.Dpi();
    int mode = light_.headlight ? 1 : 0;
    if (ui::RadioButton("Sun", mode == 0)) light_.headlight = false;
    ImGui::SameLine();
    if (ui::RadioButton("Headlight", mode == 1)) light_.headlight = true;
    ImGui::SameLine(0, 20 * dpi);
    if (ui::Button("Noon")) light_.Apply(LightPreset::Noon);
    ImGui::SameLine();
    if (ui::Button("Sunset")) light_.Apply(LightPreset::Sunset);
    ImGui::SameLine();
    if (ui::Button("Night")) light_.Apply(LightPreset::Night);
    ImGui::SameLine();
    if (ui::Button("Overcast")) light_.Apply(LightPreset::Overcast);
    const float w = 260 * dpi;
    ImGui::SetNextItemWidth(w);
    if (ui::BeginCombo("World lighting", world_.Loaded() ? world_.Name().c_str() : "procedural"))
    {
        if (ui::SelectableRaw("procedural", !world_.Loaded())) SetWorld("");
        for (const auto& name : worlds_)
            if (ui::SelectableRaw(name.c_str(), world_.Loaded() && IEquals(world_.Name(), name))) SetWorld(name);
        ImGui::EndCombo();
    }
    if (ui::Hovered()) ui::SetTooltip("CfgWorlds >> <world> >> Weather >> LightingNew: sun / ambient / ground / sky by sun angle and overcast,\nlatitude of the world. Exposure: approximate auto exposure (the game adapts its aperture).");
    if (world_.Loaded()) ui::TextDisabled("%s", world_.lastInfo.c_str());
    if (!light_.headlight)
    {
        int hh = int(light_.timeOfDay), mm = int((light_.timeOfDay - hh) * 60.0f);
        ImGui::SetNextItemWidth(w);
        ui::SliderFloat("Time of day", &light_.timeOfDay, 0.0f, 24.0f, std::format("{:02}:{:02}", hh % 24, mm).c_str());
        ImGui::SetNextItemWidth(w);
        ui::SliderFloat("Overcast", &light_.overcast, 0.0f, 1.0f, "%.2f");
    }
    ImGui::SetNextItemWidth(w);
    ui::SliderFloat("Sun / light", &light_.sunIntensity, 0.0f, 3.0f, "%.2f");
    ImGui::SetNextItemWidth(w);
    ui::SliderFloat("Ambient", &light_.ambientIntensity, 0.0f, 3.0f, "%.2f");
    ImGui::SetNextItemWidth(w);
    ui::SliderFloat("Exposure", &light_.exposure, 0.1f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ui::Checkbox("Sun shadows", &d_.shadows);
    if (d_.shadows)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * dpi);
        ui::SliderFloat("softness", &d_.shadowSoftness, 0.0f, 6.0f, "%.1f");
        ImGui::SameLine();
        const char* q[] = { "low", "medium", "high" };
        ImGui::SetNextItemWidth(80 * dpi);
        ui::Combo("quality", &d_.shadowQuality, q, 3);
        if (ui::Hovered()) ui::SetTooltip("Shadow map 1024 / 2048 / 4096, 4 / 16 / 25 filter taps");
    }
    ui::Checkbox("Background = sky", &d_.backgroundFromSky);
    if (!d_.backgroundFromSky || light_.headlight)
    {
        ImGui::SetNextItemWidth(w);
        ui::ColorEdit3("Background", d_.background);
    }
    if (!light_.headlight && ui::CollapsingHeader("Advanced##light"))
    {
        ImGui::SetNextItemWidth(w);
        ui::SliderFloat("Sun rotation", &light_.rotation, -180.0f, 180.0f, "%.0f deg");
        ImGui::SetNextItemWidth(w);
        ui::SliderFloat("Latitude", &light_.latitude, -70.0f, 70.0f, "%.0f deg");
        ImGui::SetNextItemWidth(w);
        ui::SliderFloat("Season", &light_.declination, -23.4f, 23.4f, "decl. %.1f deg");
    }
    if (!light_.headlight) ui::TextDisabled("%s elevation %.1f, azimuth %.0f", lit_.moon ? "Moon (sun below the horizon):" : "Sun", lit_.sunElevation, lit_.sunAzimuth);
}
