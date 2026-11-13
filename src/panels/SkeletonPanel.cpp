#include "SkeletonPanel.h"
#include "Lang.h"
#include "Text.h"

#include <imgui.h>

#include <cfloat>
#include <cstdint>
#include <cstring>
#include <format>

namespace {
const ImVec4 kWarnColor{ 1.0f, 0.8f, 0.3f, 1 };
bool ContainsNoCase(const std::string& hay, const std::string& needle)
{
    return needle.empty() || ToLower(hay).find(ToLower(needle)) != std::string::npos;
}
} // namespace

void SkeletonPanel::Draw()
{
    if (session_.skeleton.Empty())
    {
        ui::TextWrapped(session_.hasModel ? "No skeleton: needs model.cfg with CfgModels::<model name> and skeletonName (see Log)."
                                     : "No model loaded.");
        return;
    }
    ui::Text("%s  (CfgModels::%s)", session_.skeleton.name.c_str(), session_.modelInfo.modelClass.c_str());
    ui::Text("Bones %zu   isDiscrete %d", session_.skeleton.bones.size(), session_.skeleton.isDiscrete ? 1 : 0);
    if (!session_.skeleton.inherit.empty()) ui::Text("skeletonInherit: %s", session_.skeleton.inherit.c_str());
    if (!session_.skeleton.pivotsModel.empty())
    {
        ui::TextWrapped("pivotsModel: %s", session_.skeleton.pivotsModel.c_str());
        if (!session_.hasPivots) ui::TextColored(kWarnColor, "  not found - centroids used");
    }
    int fromPivots = 0, fromCentroid = 0, none = 0;
    for (const auto& b : session_.skeleton.bones)
        (b.pivotSource == PivotSource::PivotsModel ? fromPivots : b.pivotSource == PivotSource::SelectionCentroid ? fromCentroid : none)++;
    ui::Text("Pivots: %d pivotsModel, %d centroid, %d none", fromPivots, fromCentroid, none);
    ui::TextDisabled("click a bone: select + focus camera; red = no RTM data");
    ui::TextDisabled("yellow = pivotsModel, orange = centroid, grey name = no selection");

    ImGui::SetNextItemWidth(-FLT_MIN);
    char buf[128];
    strncpy_s(buf, filter_.c_str(), _TRUNCATE);
    if (ui::InputTextWithHint("##bonefilter", "filter (flat list)", buf, sizeof(buf))) filter_ = buf;

    ImGui::BeginChild("bones", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (filter_.empty())
    {
        for (int i = 0; i < int(session_.skeleton.bones.size()); ++i)
            if (session_.skeleton.bones[i].parent < 0) DrawBoneNode(i, 0);
    }
    else
    {
        for (int i = 0; i < int(session_.skeleton.bones.size()); ++i)
        {
            const Bone& b = session_.skeleton.bones[i];
            if (!ContainsNoCase(b.name, filter_)) continue;
            ImGui::PushID(i);
            if (ui::SelectableRaw(b.name.c_str(), host_.SelectedBone() == i)) host_.PickBone(i, true);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void SkeletonPanel::DrawBoneNode(int bone, int depth)
{
    const Bone& b = session_.skeleton.bones[bone];
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (b.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (bone == host_.SelectedBone()) flags |= ImGuiTreeNodeFlags_Selected;
    if (depth < 4) ImGui::SetNextItemOpen(true, ImGuiCond_Once);

    int sel = bone < int(host_.BoneSelection().size()) ? host_.BoneSelection()[bone] : -1;
    if (sel < 0) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(intptr_t(bone)), flags, "%s", b.name.c_str());
    if (sel < 0) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) host_.PickBone(bone, true);
    if (ui::Hovered()) ui::SetTooltip("parent: %s\npivot: %.3f %.3f %.3f (%s)\nselection: %s", b.parentName.empty() ? "-" : b.parentName.c_str(),
                          b.pivot.x, b.pivot.y, b.pivot.z,
                          b.pivotSource == PivotSource::PivotsModel ? "pivotsModel" : b.pivotSource == PivotSource::SelectionCentroid ? "centroid" : "none",
                          sel >= 0 ? std::format("{} points", session_.model.lods[session_.lod].selections[sel].pointCount).c_str() : "none");
    if (open && !b.children.empty())
    {
        for (int c : b.children) DrawBoneNode(c, depth + 1);
        ImGui::TreePop();
    }
}
