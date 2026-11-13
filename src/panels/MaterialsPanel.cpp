#include "MaterialsPanel.h"
#include "Lang.h"
#include "Material.h" // TextureEntry::Status
#include "Text.h"

#include <imgui.h>

#include <algorithm>
#include <format>

namespace {
const ImVec4 kTextColor{ 0.9f, 0.9f, 0.9f, 1 }, kOkColor{ 0.45f, 0.85f, 0.45f, 1 }, kWarnColor{ 1.0f, 0.8f, 0.3f, 1 },
    kErrorColor{ 1.0f, 0.4f, 0.4f, 1 };
} // namespace

void MaterialsPanel::Draw(const std::string& windowName, const std::function<void()>& shaderSurvey)
{
    if (!ui::Begin(windowName.c_str()))
    {
        ImGui::End();
        return;
    }
    if (!session_.hasModel)
    {
        ui::TextDisabled("No model loaded.");
        ImGui::End();
        return;
    }
    ui::Text("LOD %d: %zu sections, %zu textures cached", session_.lod, view_.Sections().size(), view_.CachedTextures());
    if (!session_.configSkins.empty())
    {
        auto skinName = [&](int i) { return i < 0 ? std::string("<model textures>") : session_.configSkins[i].cfgRoot + "::" + session_.configSkins[i].className; };
        if (ui::BeginCombo("Config skin", skinName(session_.activeSkin).c_str()))
        {
            for (int i = -1; i < int(session_.configSkins.size()); ++i)
                if (ui::SelectableRaw(skinName(i).c_str(), i == session_.activeSkin))
                {
                    session_.activeSkin = i;
                    host_.RebuildGeometry();
                }
            ImGui::EndCombo();
        }
        if (session_.activeSkin >= 0)
        {
            const ConfigSkin& s = session_.configSkins[session_.activeSkin];
            ui::TextDisabled("%s", s.where.c_str());
            const P3DLod& l = session_.model.lods[session_.lod];
            for (size_t i = 0; i < s.selections.size(); ++i)
            {
                size_t faces = 0;
                for (const auto& sel : l.selections)
                    if (IEquals(sel.name, s.selections[i])) faces = sel.faceCount;
                bool inSections = false;
                for (const auto& sec : session_.modelInfo.sections) inSections |= IEquals(sec, s.selections[i]);
                ui::BulletText("%s (%zu faces)%s -> %s%s%s", s.selections[i].c_str(), faces, inSections ? "" : " [not in sections]",
                                  i < s.textures.size() ? s.textures[i].c_str() : "-", i < s.materials.size() && !s.materials[i].empty() ? " + " : "",
                                  i < s.materials.size() ? s.materials[i].c_str() : "");
            }
        }
    }
    else if (!session_.configCpp.empty())
        ui::TextDisabled("config.cpp: no class with hiddenSelectionsTextures for this model");
    if (view_.DamageMaterials() || view_.DamageTextures())
    {
        const char* states[] = { "intact", "damaged (mat[] 2nd)", "destroyed (mat[] 3rd)" };
        ImGui::SetNextItemWidth(200 * host_.Dpi());
        if (ui::Combo(std::format("Damage state ({} RVMATs)", view_.DamageMaterials()).c_str(), &damageState, states, 3)) host_.RebuildGeometry();
        if (ui::Hovered()) ui::SetTooltip("CfgVehicles >> %s >> Damage: mat[] triples (intact, damaged, destroyed), tex[] pairs", session_.attach.VehicleClass().c_str());
    }
    else
        ui::TextDisabled("no class Damage (mat[] / tex[]) for this vehicle");
    if (shaderSurvey) shaderSurvey(); // the RVMAT shader survey of the viewer
    auto statusColor = [&](TextureEntry::Status s) {
        return s == TextureEntry::Status::Ok ? kOkColor : s == TextureEntry::Status::Procedural ? kTextColor : s == TextureEntry::Status::Missing ? kWarnColor : kErrorColor;
    };
    auto statusName = [](TextureEntry::Status s) {
        return s == TextureEntry::Status::Ok ? "ok" : s == TextureEntry::Status::Procedural ? "procedural" : s == TextureEntry::Status::Missing ? "missing" : s == TextureEntry::Status::Loading ? "loading" : "error";
    };

    if (ImGui::BeginTable("sections", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, std::max(120.0f * host_.Dpi(), ImGui::GetContentRegionAvail().y))))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ui::TableSetupColumn("Texture");
        ui::TableSetupColumn("Material");
        ui::TableSetupColumn("Faces", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(view_.Sections().size()); ++i)
        {
            const MeshSectionInfo& s = view_.Sections()[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ui::SelectableRaw(s.texture.empty() ? "<none>" : s.texture.c_str(), view_.SelectedSection() == i, ImGuiSelectableFlags_SpanAllColumns))
                view_.SelectSection(i);
            ImGui::TableNextColumn();
            ui::TextUnformatted(s.material.empty() ? "<none>" : s.material.c_str());
            ImGui::TableNextColumn();
            ui::Text("%zu", s.faces);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}
