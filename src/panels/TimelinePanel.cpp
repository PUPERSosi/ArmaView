#include "TimelinePanel.h"
#include "Lang.h"
#include "Moves.h"
#include "Text.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace {
const ImVec4 kWarnColor{ 1.0f, 0.8f, 0.3f, 1 };
} // namespace

void TimelinePanel::Draw(const std::string& windowName)
{
    if (!ui::Begin(windowName.c_str()))
    {
        ImGui::End();
        return;
    }
    const float dpi = host_.Dpi();
    if (s_.skeleton.Empty()) ui::TextColored(kWarnColor, "No skeleton - load a model with a model.cfg skeleton to play RTMs.");

    // Transport
    if (ui::Button(pb_.playing ? "Pause (Space)" : "Play (Space)")) pb_.playing = !pb_.playing;
    ImGui::SameLine();
    if (ui::Button("|<")) pb_.time = 0.0;
    ImGui::SameLine();
    if (ui::Button("< frame")) pb_.StepFrame(-1);
    ImGui::SameLine();
    if (ui::Button("frame >")) pb_.StepFrame(1);
    ImGui::SameLine();
    ui::Checkbox("Rest pose", &pb_.restPose);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160 * dpi);
    ui::SliderFloat("Speed", &pb_.speed, 0.0f, 3.0f, "x %.2f");
    ImGui::SameLine();
    if (ui::SmallButton("x1")) pb_.speed = 1.0f;

    DrawTimelineBar();
    DrawTools();
    DrawState();
    DrawLayers();
    if (ui::CollapsingHeader("Advanced##timeline")) DrawAdvanced();
    ImGui::End();
}

// the primary layer: name, frame / time, keyframe ticks, events, current position; click / drag scrubs
void TimelinePanel::DrawTimelineBar()
{
    const int p = pb_.PrimaryLayer();
    if (p < 0) return;
    const float dpi = host_.Dpi();
    const AnimLayer& l = s_.anim.layers[size_t(p)];
    const float phase = s_.anim.Phase(l, pb_.time);
    const size_t frames = l.anim->Frames();
    const float last = float(std::max<size_t>(frames, 2) - 1);
    const float cycle = l.duration / std::max(l.speed, 1e-4f);
    ui::Text("%s   frame %.1f / %zu   t %.3f / %.3f s%s", l.anim->Name().c_str(), phase * last, frames - 1, phase * cycle, cycle,
             l.pingPong ? "   ping-pong" : l.loop ? "   loop" : "");
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = std::max(50.0f, ImGui::GetContentRegionAvail().x), h = 30.0f * dpi;
    ImGui::InvisibleButton("##timeline", ImVec2(w, h));
    if (ImGui::IsItemActive()) pb_.ScrubTo((ImGui::GetIO().MousePos.x - p0.x) / w);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(30, 32, 38, 255), 3.0f);
    const int labelEvery = std::max(1, int(std::ceil(float(frames) * 34.0f * dpi / w)));
    for (size_t f = 0; f < frames; ++f)
    {
        const float x = p0.x + w * (float(f) / last);
        const bool label = f % size_t(labelEvery) == 0;
        dl->AddLine(ImVec2(x, p0.y + h * (label ? 0.45f : 0.65f)), ImVec2(x, p0.y + h), IM_COL32(150, 155, 165, 255));
        if (label) dl->AddText(ImVec2(x + 2, p0.y), IM_COL32(150, 155, 165, 255), std::to_string(f).c_str());
    }
    const float cx = p0.x + w * phase;
    dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p0.y + h), IM_COL32(255, 80, 255, 255), 2.0f);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) pb_.StepFrame(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) pb_.StepFrame(1);
    }
    pb_.UpdateSeam(); // loop seam of the primary layer
}

// onion skin, bone trail, A/B compare
void TimelinePanel::DrawTools()
{
    const float dpi = host_.Dpi();
    bool changed = ui::Checkbox("Onion skin", &opt_.onionSkin);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * dpi);
    changed |= ui::SliderInt("##onionstep", &opt_.onionStep, 1, 5, "+-%d fr");
    if (ui::Hovered()) ui::SetTooltip("Previous (blue) and next (orange) frame of the primary layer");
    ImGui::SameLine();
    changed |= ui::Checkbox("Bone trail", &opt_.showTrail);
    if (ui::Hovered()) ui::SetTooltip("Path of the selected bone over one cycle; red = jump (segment > 4x median), cyan = keyframes");
    ImGui::SameLine();
    const char* cmp[] = { "A/B off", "B overlay", "B side by side" };
    ImGui::SetNextItemWidth(130 * dpi);
    changed |= ui::Combo("##cmp", &pb_.compareMode, cmp, 3);
    ImGui::SameLine();
    const std::string bName = pb_.compare.layers.empty() ? std::string("B: none") : "B: " + pb_.compare.layers[0].anim->Name();
    ImGui::SetNextItemWidth(220 * dpi);
    if (ui::BeginCombo("##bpick", bName.c_str()))
    {
        for (int i = 0; i < int(s_.anim.layers.size()); ++i)
            if (ui::SelectableRaw(std::format("{}##b{}", s_.anim.layers[size_t(i)].anim->Name(), i).c_str()))
            {
                changed = pb_.MoveToCompare(i); // the layer becomes B (A = everything else)
                break;
            }
        if (!pb_.compare.layers.empty() && ui::SelectableRaw("(return B to the layers)")) changed = pb_.ReturnCompare();
        ImGui::EndCombo();
    }
    if (changed) host_.PoseChanged();
}

// CfgMoves state: RTM + speed / looped from the config, links to the next states
void TimelinePanel::DrawState()
{
    ImGui::SetNextItemWidth(260 * host_.Dpi());
    bool go = ui::InputTextWithHint("##state", "CfgMovesMaleSdr state, e.g. AmovPercMstpSrasWrflDnon", stateEdit_, sizeof(stateEdit_),
                                    ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    go |= ui::Button("Load state");
    if (go && stateEdit_[0]) pb_.LoadState(stateEdit_);
    const MovesStateInfo& st = pb_.state;
    if (!st.found) return;
    ImGui::SameLine();
    ui::TextDisabled("speed %g (%.2f s), interpolationSpeed %g", st.speed, DurationFromMovesSpeed(st.speed), st.interpolationSpeed);
    ImGui::SameLine();
    if (ui::SmallButton("open##st")) host_.OpenInEditor(st.cfgFile, st.cfgLine);
    std::string pick;
    auto chips = [&](const char* label, const std::vector<std::string>& list) {
        if (list.empty()) return;
        ui::TextDisabled("%s", label);
        ImGui::PushID(label); // one state can be in several lists (connectTo + interpolateTo) and twice in one
        for (size_t k = 0; k < list.size(); ++k)
        {
            const std::string& n = list[k];
            ImGui::SameLine();
            ImGui::PushID(int(k));
            if (ui::SmallButtonRaw(n.c_str())) pick = n;
            if (ui::Hovered()) ui::SetTooltip("Loads the state %s as a layer", n.c_str());
            ImGui::PopID();
        }
        ImGui::PopID();
    };
    std::vector<std::string> acts;
    for (const auto& [k, v] : st.actionEntries)
        if (std::find(acts.begin(), acts.end(), v) == acts.end() && acts.size() < 12) acts.push_back(v);
    chips("ConnectTo:", st.connectTo);
    chips("InterpolateTo:", st.interpolateTo);
    chips(std::format("actions {}:", st.actions).c_str(), acts);
    if (!pick.empty()) // after drawing: loading replaces the lists drawn above
    {
        strncpy_s(stateEdit_, pick.c_str(), _TRUNCATE);
        pb_.LoadState(pick);
    }
}

void TimelinePanel::DrawLayers()
{
    const float dpi = host_.Dpi();
    auto& layers = s_.anim.layers;
    ui::SeparatorText(TF("Layers ({})", layers.size()).c_str());
    int removeAt = -1, moveUp = -1;
    for (int i = 0; i < int(layers.size()); ++i)
    {
        AnimLayer& l = layers[size_t(i)];
        ImGui::PushID(i);
        ui::Checkbox("##on", &l.enabled);
        ImGui::SameLine();
        ui::TextUnformatted(l.anim->Name().c_str());
        if (ui::Hovered())
            ui::SetTooltip("%s\n%zu frames, %zu bones, %d match skeleton\nmove %.3f %.3f %.3f", PathToUtf8(l.anim->path).c_str(), l.anim->Frames(),
                           l.anim->Bones(), l.matched, l.anim->info.move.x, l.anim->info.move.y, l.anim->info.move.z);
        ImGui::SameLine();
        ui::TextDisabled("%d/%zu bones", l.matched, s_.skeleton.bones.size());
        ImGui::SameLine();
        if (ui::SmallButton("up") && i > 0) moveUp = i;
        ImGui::SameLine();
        if (ui::SmallButton("remove")) removeAt = i;

        const char* modes[] = { "Blend", "Override" };
        int mode = int(l.mode);
        ImGui::SetNextItemWidth(90 * dpi);
        if (ui::Combo("##mode", &mode, modes, 2)) l.mode = LayerMode(mode);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * dpi);
        ui::SliderFloat("##w", &l.weight, 0.0f, 1.0f, "weight %.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100 * dpi);
        ui::DragFloat("##speed", &l.speed, 0.01f, 0.0f, 10.0f, "speed %.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100 * dpi);
        ui::DragFloat("##dur", &l.duration, 0.01f, 0.05f, 60.0f, "%.2f s");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100 * dpi);
        ui::SliderFloat("##off", &l.offset, 0.0f, 1.0f, "offset %.2f");
        ImGui::SameLine();
        if (ui::Checkbox("loop", &l.loop)) pb_.seamAnim = nullptr;
        ImGui::SameLine();
        if (ui::Checkbox("ping-pong", &l.pingPong)) pb_.seamAnim = nullptr;
        ImGui::PopID();
    }
    if (moveUp > 0) std::swap(layers[size_t(moveUp)], layers[size_t(moveUp - 1)]);
    if (removeAt >= 0) layers.erase(layers.begin() + removeAt);

    // add from the RTM folder
    std::vector<const RtmBoneCheck*> candidates;
    for (const auto& c : view_.RtmChecks())
        if (c.error.empty()) candidates.push_back(&c);
    if (!candidates.empty())
    {
        addRtmIndex_ = std::clamp(addRtmIndex_, 0, int(candidates.size()) - 1);
        ImGui::SetNextItemWidth(220 * dpi);
        if (ui::BeginCombo("##add", PathToUtf8(candidates[size_t(addRtmIndex_)]->file.filename()).c_str()))
        {
            for (int i = 0; i < int(candidates.size()); ++i)
                if (ui::SelectableRaw(PathToUtf8(candidates[size_t(i)]->file.filename()).c_str(), i == addRtmIndex_)) addRtmIndex_ = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ui::Button("Add layer")) pb_.AddLayer(candidates[size_t(addRtmIndex_)]->file);
        ImGui::SameLine();
    }
    if (ui::Button("Open RTM...")) view_.ShowOpenDialog();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * dpi);
    ui::DragFloat("fps for new layers", &pb_.fps, 0.5f, 1.0f, 120.0f, "%.0f");
}

void TimelinePanel::DrawAdvanced()
{
    const char* modes[] = { "Engine (identity, not confirmed for Arma 3)", "Follow parent" };
    int mb = int(s_.anim.missing);
    ImGui::SetNextItemWidth(160 * host_.Dpi());
    if (ui::Combo("Missing bones", &mb, modes, 2)) s_.anim.missing = MissingBones(mb);
    if (ui::Hovered())
        ui::SetTooltip("Skeleton bones without data in a layer.\nFollow parent (default): move rigidly with the parent bone.\n"
                       "Engine (identity): Blend layers put them in the bind pose, as OFP / CWR does (RtAnimation.cpp:236) - not confirmed for Arma 3.\n"
                       "Gesture / handAnim layers never override a bone they have no data for.\nSuch bones are drawn red on the skeleton.");
    ImGui::SameLine();
    ui::Checkbox("Subtract RTM step", &s_.anim.subtractStep);
    if (ui::Hovered()) ui::SetTooltip("OFP IntroduceStep: position -= move * phase (CWR RtAnimation.cpp:771).\nArma 3 RTMs are already in place - keep off.");
    // skinning options
    bool skinChanged = ui::Checkbox("Normalize weights", &opt_.skinNormalize);
    ImGui::SameLine();
    skinChanged |= ui::Checkbox("Max 4 bones/point", &opt_.skinLimit4);
    if (skinChanged) host_.RebuildGeometry();
    ImGui::SameLine();
    ui::TextDisabled("(?)");
    if (ui::Hovered())
        ui::SetTooltip("Approximation of binarize: strongest 4 bones, weights scaled to sum 1.\nUnticked: raw weights; the missing part of the sum stays at rest pose.");
}
