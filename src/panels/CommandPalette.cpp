#include "CommandPalette.h"
#include "Lang.h"
#include "Text.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <string>

namespace {
bool ContainsNoCase(const std::string& hay, const std::string& needle)
{
    return needle.empty() || ToLower(hay).find(ToLower(needle)) != std::string::npos;
}
} // namespace

void CommandPalette::Draw(float dpi)
{
    if (!open) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 40 * dpi), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(620 * dpi, 0));
    if (focus) ImGui::SetNextWindowFocus();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize;
    std::function<void()> run;
    if (ui::Begin("##commandpalette", nullptr, flags))
    {
        if (focus)
        {
            ImGui::SetKeyboardFocusHere();
            focus = false;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ui::InputTextWithHint("##filter", "type a command (Enter runs, Esc closes)", filter_, sizeof(filter_))) sel_ = 0;

        // every word must occur in "group: description key"
        std::vector<std::string> words;
        for (size_t i = 0, n = strlen(filter_); i < n;)
        {
            while (i < n && filter_[i] == ' ') ++i;
            size_t j = i;
            while (j < n && filter_[j] != ' ') ++j;
            if (j > i) words.emplace_back(filter_ + i, j - i);
            i = j;
        }
        std::vector<int> hits;
        for (int i = 0; i < int(hotkeys_.size()); ++i)
        {
            const Hotkey& h = hotkeys_[i];
            if (!h.run) continue;
            // both languages are searched
            std::string text = std::string(h.group) + ": " + h.description + " " + T(h.group) + ": " + T(h.description) + " " +
                               (h.chord != ImGuiKey_None ? ChordName(h.chord) : "");
            bool ok = true;
            for (const auto& w : words) ok = ok && ContainsNoCase(text, w);
            if (ok) hits.push_back(i);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ++sel_;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) --sel_;
        sel_ = hits.empty() ? 0 : std::clamp(sel_, 0, int(hits.size()) - 1);
        const float rows = float(std::min<size_t>(hits.size(), 14));
        if (ImGui::BeginChild("##cmds", ImVec2(0, std::max(1.0f, rows) * ImGui::GetTextLineHeightWithSpacing() + 4 * dpi)))
        {
            for (int r = 0; r < int(hits.size()); ++r)
            {
                const Hotkey& h = hotkeys_[hits[r]];
                const bool enabled = !h.needsModel || hasModel_();
                ImGui::PushID(r);
                ImGui::BeginDisabled(!enabled);
                if (ui::SelectableRaw(std::format("{}: {}", T(h.group), T(h.description)).c_str(), r == sel_) && enabled) run = h.run;
                ImGui::EndDisabled();
                if (r == sel_ && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))) ImGui::SetScrollHereY();
                if (h.chord != ImGuiKey_None)
                {
                    std::string key = ChordName(h.chord);
                    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(key.c_str()).x);
                    ui::TextDisabled("%s", key.c_str());
                }
                ImGui::PopID();
            }
            if (hits.empty()) ui::TextDisabled("no command matches");
        }
        ImGui::EndChild();
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
            if (!hits.empty() && (!hotkeys_[hits[sel_]].needsModel || hasModel_())) run = hotkeys_[hits[sel_]].run;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !focus)) open = false;
    }
    ImGui::End();
    if (run)
    {
        open = false;
        filter_[0] = 0;
        sel_ = 0;
        run();
    }
}
