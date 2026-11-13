#include "ClosePrompt.h"
#include "Lang.h"
#include "Text.h"

#include <imgui.h>

std::vector<std::string> ClosePrompt::UnsavedEdits() const
{
    std::vector<std::string> out;
    if (s_.movesDoc.Dirty()) out.push_back(TF("Move config {}: changed, not saved (Move editor: Diff & Save...)", moves_.preferredFile));
    if (!mats_.edits.empty())
    {
        std::string names;
        for (const auto& [k, m] : mats_.edits) names += (names.empty() ? "" : ", ") + PathToUtf8(m.path.filename());
        out.push_back(TF("RVMAT edits: {} (Materials: Save)", names));
    }
    if (!reload_.windowEdits_.empty()) out.push_back(TF("model.cfg windows: {} changed (Reload sync: Diff... / Save)", reload_.windowEdits_.size()));
    if (weapon_.ConfigDirty()) out.push_back(T("Weapon config.cpp: changed, not saved"));
    if (layers_.ProjectDirty()) out.push_back(T("Animation layers project: changed, not saved (Layers: Save project)"));
    return out;
}

bool ClosePrompt::AllowClose(bool automation)
{
    if (force_ || automation) return true;
    if (UnsavedEdits().empty()) return true;
    show_ = true;
    return false;
}

void ClosePrompt::Draw()
{
    if (show_)
    {
        ImGui::OpenPopup(T("Unsaved changes###closeprompt"));
        show_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(T("Unsaved changes###closeprompt"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ui::TextUnformatted("These changes are only in the viewer and will be lost:");
    for (const auto& u : UnsavedEdits()) ImGui::BulletText("%s", u.c_str());
    ImGui::Separator();
    if (ui::Button("Close without saving"))
    {
        force_ = true;
        ImGui::CloseCurrentPopup();
        close_();
    }
    ImGui::SameLine();
    if (ui::Button("Cancel##close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
