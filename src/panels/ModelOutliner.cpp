#include "ModelOutliner.h"
#include "Lang.h"
#include "Text.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <format>

namespace {
const ImVec4 kWarnColor = { 1.0f, 0.8f, 0.3f, 1 };

std::string Utf8(uint32_t cp)
{
    std::string s;
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
    else { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
    return s;
}

bool ContainsNoCase(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                       [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); }) != hay.end();
}
} // namespace

bool EyeButton(const char* id, bool visible, const char* tip, bool icons, float dpi)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2 * dpi, 0));
    if (!visible) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const std::string label = (icons ? Utf8(visible ? 0xE890 : 0xED1A) : std::string(visible ? "o" : "-")) + id;
    const bool pressed = ui::SmallButton(label.c_str());
    if (!visible) ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (tip && ui::Hovered()) ui::SetTooltip("%s", tip);
    return pressed;
}

void ModelOutliner::Draw()
{
    ModelVisibility& vis = view_.Visibility();
    if (!s_.hasModel)
    {
        ui::TextWrapped("No model. File > Open (Ctrl+O) or drop a .p3d onto the window.");
        return;
    }

    ui::TextWrapped("%s", PathToUtf8(s_.model.path).c_str());
    if (s_.modelCfg.empty())
        ui::TextColored(kWarnColor, "model.cfg: not found");
    else
        ui::TextWrapped("model.cfg: %s", PathToUtf8(s_.modelCfg).c_str());

    ui::SeparatorText("LODs");
    if (ImGui::BeginTable("lods", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
    {
        ui::TableSetupColumn("##eye", ImGuiTableColumnFlags_WidthFixed);
        ui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
        ui::TableSetupColumn("LOD");
        ui::TableSetupColumn("Points", ImGuiTableColumnFlags_WidthFixed);
        ui::TableSetupColumn("Faces", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(s_.model.lods.size()); ++i)
        {
            const P3DLod& l = s_.model.lods[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            ImGui::TableNextColumn();
            if (ui::SelectableRaw(std::to_string(i).c_str(), s_.lod == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            {
                inspector_.what = InspectorPanel::Kind::None;
                if (s_.lod != i)
                {
                    s_.lod = i;
                    host_.LodChanged();
                }
            }
            ImGui::TableSetColumnIndex(0); // eye over the row's selectable
            if (i == s_.lod)
            {
                LodVisibility& v = vis.Lod(l.resolution);
                if (EyeButton("##loveye", !v.meshHidden, "Hide / show this LOD's mesh (proxies, points, skeleton stay)", icons_, host_.Dpi()))
                {
                    v.meshHidden = !v.meshHidden;
                    view_.VisibilityChanged();
                }
            }
            else
            {
                const std::string key = ModelVisibility::Key(l.resolution);
                const bool on = vis.overlays.count(key) > 0;
                if (EyeButton("##loveye", on, "Draw this LOD over the current one as wireframe (rest pose)", icons_, host_.Dpi()))
                {
                    if (on) vis.overlays.erase(key);
                    else vis.overlays.insert(key);
                    vis.Save();
                    view_.RebuildLodOverlay();
                }
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::PopID();
            ImGui::TableNextColumn();
            ui::TextUnformatted(l.Name().c_str());
            ImGui::TableNextColumn();
            ui::Text("%zu", l.points.size());
            ImGui::TableNextColumn();
            ui::Text("%zu", l.faces.size());
        }
        ImGui::EndTable();
    }

    const P3DLod& l = s_.model.lods[s_.lod];
    if (ui::CollapsingHeader(TF("Selections ({})", l.selections.size()).c_str(), ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SetNextItemWidth(-FLT_MIN);
        char buf[128];
        strncpy_s(buf, filter_.c_str(), _TRUNCATE);
        if (ui::InputTextWithHint("##filter", "filter", buf, sizeof(buf))) filter_ = buf;
        ui::TextDisabled("Click a row to show its point weights (blue 0 → red 1).");

        DrawVisibility();
        const LodVisibility* lv = vis.Find(l.resolution);
        if (ImGui::BeginTable("sels", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0, std::max(150.0f, ImGui::GetContentRegionAvail().y))))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ui::TableSetupColumn("##eye", ImGuiTableColumnFlags_WidthFixed);
            ui::TableSetupColumn("Name");
            ui::TableSetupColumn("Points", ImGuiTableColumnFlags_WidthFixed);
            ui::TableSetupColumn("Partial", ImGuiTableColumnFlags_WidthFixed);
            ui::TableSetupColumn("Faces", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();
            std::vector<int> rows;
            rows.reserve(l.selections.size());
            for (int i = 0; i < int(l.selections.size()); ++i)
                if (ContainsNoCase(l.selections[i].name, filter_)) rows.push_back(i);
            ImGuiListClipper clip; // only the visible rows
            clip.Begin(int(rows.size()));
            while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r)
            {
                const int i = rows[r];
                const P3DSelection& s = l.selections[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                ImGui::TableNextColumn();
                const bool dim = s.pointCount == 0 || s.engineIgnored;
                if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                const bool active = view_.HighlightedSelection() == i;
                if (ui::SelectableRaw(s.name.c_str(), active, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    view_.HighlightSelection(active ? -1 : i);
                if (dim) ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(0); // eye over the row's selectable
                {
                    const bool hidden = lv && lv->hidden.count(ToLower(s.name));
                    const bool isolated = lv && !lv->isolate.empty();
                    const bool isoThis = isolated && IEquals(lv->isolate, s.name);
                    if (EyeButton("##eye", !hidden && (!isolated || isoThis),
                                  isoThis ? "Isolated (Shift+H again or Alt+H: back)" : "Hide / show (H). Right click: isolate (Shift+H)", icons_,
                                  host_.Dpi()))
                    {
                        LodVisibility& v = vis.Lod(l.resolution);
                        if (isolated && !isoThis) v.isolate.clear();
                        else if (hidden) v.hidden.erase(ToLower(s.name));
                        else if (isoThis) v.isolate.clear();
                        else v.hidden.insert(ToLower(s.name));
                        view_.VisibilityChanged();
                    }
                    if (ImGui::BeginPopupContextItem("##eyectx"))
                    {
                        if (ui::MenuItem("Isolate", "Shift+H"))
                        {
                            LodVisibility& v = vis.Lod(l.resolution);
                            v.isolate = isoThis ? std::string() : s.name;
                            v.isolateFaces.clear();
                            view_.VisibilityChanged();
                        }
                        if (ui::MenuItem("Show all", "Alt+H")) view_.ShowAllHidden();
                        ImGui::EndPopup();
                    }
                }
                ImGui::TableSetColumnIndex(1);
                if (s.engineIgnored && ui::Hovered()) ui::SetTooltip("Name starts with '-' or '.': the engine skips this selection (CWR ShapeSetup.cpp:928)");
                ImGui::PopID();
                ImGui::TableNextColumn();
                ui::Text("%zu", s.pointCount);
                ImGui::TableNextColumn();
                ui::Text("%zu", s.partialCount);
                ImGui::TableNextColumn();
                ui::Text("%zu", s.faceCount);
            }
            ImGui::EndTable();
        }
    }
}

void ModelOutliner::DrawVisibility()
{
    ModelVisibility& vis = view_.Visibility();
    std::vector<uint32_t>& picked = view_.PickedFaces();
    const P3DLod& l = s_.model.lods[s_.lod];
    if (vis.AnyHidden(l.resolution))
    {
        const LodVisibility* cv = vis.Find(l.resolution);
        int hid = 0;
        for (uint8_t h : view_.FaceHidden()) hid += h;
        ui::TextColored(kWarnColor, "%d of %zu faces hidden%s", hid, l.faces.size(), cv && !cv->isolate.empty() ? " (isolated)" : "");
        ImGui::SameLine();
        if (ui::SmallButton("Show all (Alt+H)")) view_.ShowAllHidden();
    }
    if (!picked.empty())
    {
        ui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1), "Picked faces: %zu", picked.size());
        ImGui::SameLine();
        if (ui::SmallButton("Hide (H)")) view_.HideSelected();
        ImGui::SameLine();
        if (ui::SmallButton("Isolate (Shift+H)")) view_.IsolateSelected();
        ImGui::SameLine();
        if (ui::SmallButton("Clear")) { picked.clear(); host_.RebuildGeometry(); }
        ImGui::SetNextItemWidth(160 * host_.Dpi());
        ui::InputTextWithHint("##setname", "set name", setName_, sizeof(setName_));
        ImGui::SameLine();
        if (ui::SmallButton("Save as temporary set") && setName_[0])
        {
            vis.Lod(l.resolution).sets[setName_] = picked;
            picked.clear();
            setName_[0] = 0;
            view_.VisibilityChanged();
        }
        if (ui::Hovered()) ui::SetTooltip("Viewer-only named face set for hiding / checks (settings.txt, never written to the P3D)");
    }
    else
        ui::TextDisabled("Ctrl+click faces to pick; H hide, Shift+H isolate, Alt+H show all");
    if (!picked.empty() && ui::TreeNode("Shading check of the picked faces"))
    {
        inspector_.DrawShadingCheck(picked);
        ImGui::TreePop();
    }
    LodVisibility& v = vis.Lod(l.resolution);
    if (v.sets.empty()) return;
    ui::TextDisabled("Temporary sets (viewer only):");
    std::string erase;
    for (auto& [name, faces] : v.sets)
    {
        ImGui::PushID(name.c_str());
        const bool hidden = v.hidden.count(ToLower(name)) > 0;
        const bool isoThis = v.isolate == name;
        if (EyeButton("##seteye", !hidden && (v.isolate.empty() || isoThis), "Hide / show", icons_, host_.Dpi()))
        {
            if (hidden) v.hidden.erase(ToLower(name));
            else v.hidden.insert(ToLower(name));
            view_.VisibilityChanged();
        }
        ImGui::SameLine();
        ui::Text("%s  (%zu faces)", name.c_str(), faces.size());
        ImGui::SameLine();
        if (ui::SmallButton(isoThis ? "un-isolate" : "isolate"))
        {
            v.isolate = isoThis ? std::string() : name;
            v.isolateFaces.clear();
            view_.VisibilityChanged();
        }
        ImGui::SameLine();
        if (ui::SmallButton("pick")) { picked = faces; host_.RebuildGeometry(); }
        ImGui::SameLine();
        if (ui::SmallButton("delete")) erase = name;
        ImGui::PopID();
    }
    if (!erase.empty())
    {
        v.hidden.erase(ToLower(erase));
        if (v.isolate == erase) v.isolate.clear();
        v.sets.erase(erase);
        view_.VisibilityChanged();
    }
}
