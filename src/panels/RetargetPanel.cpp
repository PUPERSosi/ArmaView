#include "RetargetPanel.h"
#include "Lang.h"
#include "Moves.h"
#include "Text.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <format>

namespace fs = std::filesystem;

namespace {
const ImVec4 kWarnColor{ 1.0f, 0.8f, 0.3f, 1 };
} // namespace

void RetargetPanel::Draw(const std::string& windowName, const std::string& timelineName)
{
    if (ImGuiWindow* tl = ImGui::FindWindowByName(timelineName.c_str()); tl && tl->DockId)
        ImGui::SetNextWindowDockID(tl->DockId, ImGuiCond_FirstUseEver);
    if (!ui::Begin(windowName.c_str()))
    {
        ImGui::End();
        return;
    }
    ui::HelpMarker("Retarget: an RTM of another skeleton on this model.\n"
                   "1. Source rig: a character .p3d of the source skeleton (BI's sample man by default) - Load source rig.\n"
                   "2. Source animation: an .rtm, or a CfgMovesMaleSdr state name + Use state.\n"
                   "3. Retarget + preview: the result plays on the model; the source skeleton is drawn beside or over it.\n"
                   "4. Bone mapping: same names are mapped automatically; fix the rest and Save preset.\n"
                   "5. Export RTM_0101 + verify writes a NEW file and compares it with the preview.");
    ImGui::SameLine();
    ui::TextDisabled("Quick check: an RTM of another skeleton on this model. Export writes a NEW RTM_0101 and verifies it.");
    if (session_.skeleton.Empty()) ui::EmptyState("Open the target character model first (a model with a model.cfg skeleton).");
    ui::Text("Target: %s", session_.skeleton.Empty() ? "(no skeleton - open a character model)" : std::format("{} ({} bones)", session_.skeleton.name, session_.skeleton.bones.size()).c_str());
    ImGui::SetNextItemWidth(-200 * host_.Dpi());
    ui::InputTextWithHint("##rtsrc", "source character model .p3d (its model.cfg skeleton + pivots)", job_.srcModel, sizeof(job_.srcModel));
    ImGui::SameLine();
    if (ui::Button("Load source rig")) job_.LoadSource(session_);
    ui::TextColored(job_.src.ok ? ImVec4(0.45f, 0.85f, 0.45f, 1) : kWarnColor, "Source: %s", job_.src.info.empty() ? "-" : job_.src.info.c_str());

    ImGui::SetNextItemWidth(-200 * host_.Dpi());
    if (ui::InputTextWithHint("##rtanim", "source animation .rtm (or a CfgMoves state name + Use state)", job_.anim, sizeof(job_.anim))) {}
    ImGui::SameLine();
    if (ui::Button("Use state") && session_.game)
    {
        // a CfgMovesMaleSdr state name -> its RTM
        const MovesStateInfo s = LookupMovesState(session_.game.get(), job_.anim);
        if (s.found)
        {
            fs::path p = ResolveArmaPath(s.file, session_.projectRoot.empty() ? fs::path("P:\\") : session_.projectRoot);
            if (!p.has_extension()) p += ".rtm";
            job_.duration = DurationFromMovesSpeed(s.speed) > 0 ? DurationFromMovesSpeed(s.speed) : 1.0f;
            strncpy_s(job_.anim, PathToUtf8(p).c_str(), _TRUNCATE);
        }
        else
            job_.msg = std::format("{}: no such state in CfgMovesMaleSdr", job_.anim);
    }
    if (ui::Hovered()) ui::SetTooltip("Type a CfgMovesMaleSdr state (e.g. AmovPercMwlkSrasWrflDf) and press: its RTM and cycle duration");
    ui::Checkbox("Scale pelvis motion by leg length", &job_.opt.scalePelvis);
    ImGui::SameLine();
    ui::Checkbox("Foot IK (feet on the scaled source footprints)", &job_.opt.footIK);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * host_.Dpi());
    ui::DragFloat("cycle s", &job_.duration, 0.02f, 0.1f, 60.0f, "%.2f");
    ImGui::SameLine();
    const char* pv[] = { "no source", "source beside", "source overlaid" };
    ImGui::SetNextItemWidth(130 * host_.Dpi());
    ui::Combo("##rtpreview", &job_.previewMode, pv, 3);
    if (ui::Button("Retarget + preview")) run_();
    ImGui::SameLine();
    ImGui::BeginDisabled(!job_.previewOn);
    if (ui::Button("Stop preview")) stop_();
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-230 * host_.Dpi());
    if (ui::InputTextWithHint("##rtout", "output .rtm (new file)", job_.out, sizeof(job_.out))) job_.outAuto = false;
    ImGui::SameLine();
    ImGui::BeginDisabled(!job_.res.anim);
    if (ui::Button("Export RTM_0101 + verify")) job_.Export(false);
    ImGui::EndDisabled();
    if (!job_.msg.empty()) ui::TextWrapped("%s", job_.msg.c_str());

    if (job_.src.ok && !session_.skeleton.Empty() && ui::CollapsingHeader(TF("Bone mapping ({} of {} mapped)", int(std::count_if(job_.map.srcOf.begin(), job_.map.srcOf.end(), [](int s) { return s >= 0; })),
                                                                               session_.skeleton.bones.size()).c_str()))
    {
        if (job_.map.srcOf.size() != session_.skeleton.bones.size()) job_.map = AutoMapBones(job_.src.skel, session_.skeleton);
        if (ui::Button("Auto (same names)")) job_.map = AutoMapBones(job_.src.skel, session_.skeleton);
        ImGui::SameLine();
        if (ui::Button("Save preset")) job_.msg = SaveMappingPreset(job_.PresetFile(session_.skeleton), job_.src.skel, session_.skeleton, job_.map) ? "saved " + PathToUtf8(job_.PresetFile(session_.skeleton)) : "cannot save the preset";
        ImGui::SameLine();
        if (ui::Button("Load preset")) job_.msg = LoadMappingPreset(job_.PresetFile(session_.skeleton), job_.src.skel, session_.skeleton, job_.map) ? "loaded " + PathToUtf8(job_.PresetFile(session_.skeleton)) : "no preset for this pair yet";
        ImGui::SameLine();
        ui::TextDisabled("%s", PathToUtf8(job_.PresetFile(session_.skeleton).filename()).c_str());
        if (ImGui::BeginTable("##rtmap", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, 260 * host_.Dpi())))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ui::TableSetupColumn(std::format("target {}", session_.skeleton.name).c_str());
            ui::TableSetupColumn(std::format("source {}", job_.src.skel.name).c_str());
            ImGui::TableHeadersRow();
            ImGuiListClipper clip;
            clip.Begin(int(session_.skeleton.bones.size()));
            while (clip.Step())
                for (int b = clip.DisplayStart; b < clip.DisplayEnd; ++b)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const int s = job_.map.srcOf[b];
                    ui::TextColored(s < 0 ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : job_.map.manual[b] ? kWarnColor : ImVec4(1, 1, 1, 1), "%s",
                                       session_.skeleton.bones[b].name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::PushID(b);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ui::BeginCombo("##src", s < 0 ? "- (follows parent)" : job_.src.skel.bones[s].name.c_str()))
                    {
                        if (ui::SelectableRaw("- (follows parent)", s < 0)) { job_.map.srcOf[b] = -1; job_.map.manual[b] = 1; }
                        for (int k = 0; k < int(job_.src.skel.bones.size()); ++k)
                            if (ui::SelectableRaw(job_.src.skel.bones[k].name.c_str(), k == s)) { job_.map.srcOf[b] = k; job_.map.manual[b] = 1; }
                        ImGui::EndCombo();
                    }
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}
