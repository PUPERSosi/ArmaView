#include "LogPanel.h"
#include "Lang.h"

#include <imgui.h>

#include <vector>

namespace {
const char* const kLevelNames[] = { "Debug", "Info", "Warn", "Error" };
const ImVec4 kLevelColors[] = { { 0.6f, 0.6f, 0.6f, 1 }, { 0.9f, 0.9f, 0.9f, 1 }, { 1.0f, 0.8f, 0.3f, 1 }, { 1.0f, 0.4f, 0.4f, 1 } };
} // namespace

void LogPanel::Draw(const std::string& windowName)
{
    if (!ui::Begin(windowName.c_str()))
    {
        ImGui::End();
        return;
    }
    for (int i = 0; i < 4; ++i)
    {
        ui::Checkbox(kLevelNames[i], &showLevel[i]);
        ImGui::SameLine();
    }
    ui::Checkbox("Auto-scroll", &autoScroll);
    ImGui::SameLine();
    if (ui::Button("Clear")) { entries_.Clear(); Log::Get().ResetCounts(); }
    ImGui::SameLine();
    if (ui::Button("Copy"))
    {
        std::string all;
        for (const auto& e : entries_.Entries())
            if (showLevel[int(e.level)]) all += e.text + "\n";
        ImGui::SetClipboardText(all.c_str());
    }

    ImGui::BeginChild("logscroll", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    std::vector<int> visible;
    const auto& entries = entries_.Entries();
    for (int i = 0; i < int(entries.size()); ++i)
        if (showLevel[int(entries[i].level)]) visible.push_back(i);
    ImGuiListClipper clipper;
    clipper.Begin(int(visible.size()));
    while (clipper.Step())
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
        {
            const LogEntry& e = entries[size_t(visible[size_t(r)])];
            ImGui::PushStyleColor(ImGuiCol_Text, kLevelColors[int(e.level)]);
            ui::TextUnformatted(e.text.c_str());
            ImGui::PopStyleColor();
        }
    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}
