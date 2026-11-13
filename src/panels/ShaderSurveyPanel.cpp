#include "ShaderSurveyPanel.h"
#include "Lang.h"

#include <imgui.h>

#include <format>

namespace {
const ImVec4 kWarnColor = { 1.0f, 0.8f, 0.3f, 1 };
const ImVec4 kErrColor = { 1.0f, 0.4f, 0.4f, 1 };
} // namespace

void ShaderSurveyPanel::Draw()
{
    if (!ui::CollapsingHeader("PixelShaderID survey of P:\\ (a3 + mods)")) return;
    survey_.Poll();
    ImGui::BeginDisabled(survey_.Busy());
    if (ui::Button(survey_.Busy() ? std::format("Scanning... {} files", survey_.progress.load()).c_str() : "Scan *.rvmat on P:\\")) survey_.Start(s_.projectRoot);
    ImGui::EndDisabled();
    if (!survey_.results.empty())
    {
        ImGui::SameLine();
        if (ui::SmallButton("Copy table")) ImGui::SetClipboardText(survey_.text.c_str());
        if (ImGui::BeginTable("##survey", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                              ImVec2(0, 300 * host_.Dpi())))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ui::TableSetupColumn("PixelShaderID");
            ui::TableSetupColumn("Status");
            ui::TableSetupColumn("a3", ImGuiTableColumnFlags_WidthFixed);
            ui::TableSetupColumn("mods", ImGuiTableColumnFlags_WidthFixed);
            ui::TableSetupColumn("Note / example");
            ImGui::TableHeadersRow();
            for (const auto& u : survey_.results)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ui::TextUnformatted(u.name.c_str());
                ImGui::TableNextColumn();
                const ImVec4 c = u.status[0] == 'i' ? ImVec4(0.45f, 0.85f, 0.45f, 1) : u.status[0] == 'a' ? kWarnColor : kErrColor;
                ui::TextColored(c, "%s", u.status.c_str());
                ImGui::TableNextColumn();
                ui::Text("%d", u.a3);
                ImGui::TableNextColumn();
                ui::Text("%d", u.mods);
                ImGui::TableNextColumn();
                ui::TextUnformatted(u.note.c_str());
                if (ui::Hovered()) ui::SetTooltip("a3: %s\nmods: %s", u.exampleA3.c_str(), u.exampleMod.c_str());
            }
            ImGui::EndTable();
        }
    }
}
