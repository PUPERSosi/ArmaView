#include "SourcesPanel.h"
#include "Lang.h"
#include "Validation.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <format>

namespace {
const ImVec4 kOkColor{ 0.45f, 0.85f, 0.45f, 1 }, kErrorColor{ 1.0f, 0.4f, 0.4f, 1 };
} // namespace

void SourcesPanel::Draw(const std::string& windowName)
{
    if (!ui::Begin(windowName.c_str()))
    {
        ImGui::End();
        return;
    }
    if (session_.cfgAnims.empty())
    {
        ui::TextDisabled(session_.skeleton.Empty() ? "No skeleton - model.cfg animations need CfgSkeletons bones."
                                              : "No CfgModels animations for this model.");
        ImGui::End();
        return;
    }
    ui::Text("CfgModels::%s: %zu animations, %zu sources", session_.modelInfo.modelClass.c_str(), session_.cfgAnims.size(), session_.cfgSources.size());
    if (ui::Button("Reset##sources"))
    {
        for (auto& s : session_.cfgSources) { s.value = std::clamp(0.0f, s.minValue, s.maxValue); s.animate = false; }
        host_.PoseChanged();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * host_.Dpi());
    ui::SliderFloat("oscillation period", &oscillatePeriod, 0.5f, 10.0f, "%.1f s");
    if (ImGui::BeginTable("src", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0, std::min(260.0f * host_.Dpi(), 26.0f * host_.Dpi() * (session_.cfgSources.size() + 1)))))
    {
        ui::TableSetupColumn("anim", ImGuiTableColumnFlags_WidthFixed);
        ui::TableSetupColumn("value");
        for (int i = 0; i < int(session_.cfgSources.size()); ++i)
        {
            CfgSource& s = session_.cfgSources[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ui::Checkbox("##osc", &s.animate);
            if (ui::Hovered()) ui::SetTooltip("Oscillate between min and max");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ui::SliderFloat("##v", &s.value, s.minValue, s.maxValue, std::format("{}  %.3f", s.name).c_str())) host_.PoseChanged();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (ui::CollapsingHeader("Advanced##sources"))
    {
    if (ui::Checkbox("memory=0: also look in the Memory LOD", &memoryAxisFallback)) host_.LodChanged();
    if (ui::Hovered()) ui::SetTooltip("Some configs set memory=0 while the axis exists only in the Memory LOD.\nOn: animate anyway (orange warning). Off: strictly as written (red error, no motion).");

    }
    ui::SeparatorText("Animations");
    if (ImGui::BeginTable("anims", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable,
                          ImVec2(0, ImGui::GetContentRegionAvail().y)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ui::TableSetupColumn("Name");
        ui::TableSetupColumn("Type");
        ui::TableSetupColumn("Selection");
        ui::TableSetupColumn("Source");
        ui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthFixed);
        ui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(int(session_.cfgAnims.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
            {
                const CfgAnimation& a = session_.cfgAnims[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ui::SelectableRaw(a.name.c_str(), a.bone >= 0 && a.bone == host_.SelectedBone(), ImGuiSelectableFlags_SpanAllColumns) && a.bone >= 0)
                    host_.PickBone(a.bone);
                if (ui::Hovered()) ui::SetTooltip("%s", session_.cfg.Where(a.file, a.line).c_str());
                ImGui::TableNextColumn();
                ui::TextUnformatted(a.typeName.c_str());
                ImGui::TableNextColumn();
                ui::TextUnformatted(a.selection.c_str());
                ImGui::TableNextColumn();
                ui::TextUnformatted(a.source.c_str());
                ImGui::TableNextColumn();
                if (a.sourceIndex >= 0) ui::Text("%.2f", a.Phase(session_.cfgSources[a.sourceIndex].value));
                ImGui::TableNextColumn();
                AnimStatus st = AnimStatusOf(a, host_.BoneSelection(), session_.lod);
                if (st.severity == Severity::Error) ui::TextColored(kErrorColor, "%s", st.message.c_str());
                else if (st.severity == Severity::Inactive) ui::TextDisabled("%s", st.message.c_str());
                else ui::TextColored(kOkColor, "ok");
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    ImGui::End();
}
