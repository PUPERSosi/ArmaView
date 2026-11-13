#include "UnitPanel.h"
#include "Lang.h"
#include "RvMaterial.h"
#include "Text.h"

#include <imgui.h>

#include <cstring>
#include <format>

namespace {
const ImVec4 kWarnColor{ 1.0f, 0.8f, 0.3f, 1 }, kErrorColor{ 1.0f, 0.4f, 0.4f, 1 };

bool Matches(const std::string& s, const char* filter)
{
    if (!filter[0]) return true;
    return ToLower(s).find(ToLower(filter)) != std::string::npos;
}
} // namespace

void UnitPanel::Draw(const std::string& windowName)
{
    if (!ui::Begin(windowName.c_str()))
    {
        ImGui::End();
        return;
    }
    ui::HelpMarker("A soldier class dressed like the game does it: the class's model is the uniform, the head comes from its face "
                   "(CfgFaces / CfgHeads), headgear, vest and NVG from linkedItems[], the backpack from backpack=. Every part is "
                   "skinned with the open model's skeleton and follows its animation.");
    ImGui::SameLine();
    const Unit& u = session_.unit;
    if (u.Active())
    {
        if (u.Outfit())
        {
            ui::Text("Outfit on %s", PathToUtf8(u.OutfitBody().filename()).c_str());
            ImGui::SameLine();
            ui::TextDisabled("(%s)", PathToUtf8(u.OutfitBody().parent_path()).c_str());
        }
        else
        {
            ui::Text("CfgVehicles::%s", u.Class().c_str());
            ImGui::SameLine();
            ui::TextDisabled("(%s)", u.ModelPath().c_str());
        }
        ImGui::SameLine();
        if (ui::SmallButton("Put away##unit")) host_.AssembleUnit("");
        if (ui::Hovered()) ui::SetTooltip("Remove the parts; the model stays open");
    }
    else
        ui::TextDisabled("No unit assembled");
    if (ImGui::BeginTabBar("##unittabs"))
    {
        if (ui::BeginTabItem("Class##unittab", nullptr, selectTab_ == 0 ? ImGuiTabItemFlags_SetSelected : 0))
        {
            DrawClassPicker();
            ImGui::EndTabItem();
        }
        if (ui::BeginTabItem("Outfit##unittab", nullptr, selectTab_ == 1 ? ImGuiTabItemFlags_SetSelected : 0))
        {
            DrawOutfit();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        selectTab_ = -1;
    }
    if (u.Active()) DrawParts();
    ImGui::End();
}

// Outfit (try-on): the body (the Samples' ArmaMan, a soldier class, a P3D), parts from P3D files, presets, WASD
void UnitPanel::DrawOutfit()
{
    ui::TextUnformatted("Body:");
    ImGui::SameLine();
    const auto armaMan = host_.SamplesArmaMan();
    std::error_code ec;
    ImGui::BeginDisabled(armaMan.empty() || !std::filesystem::exists(armaMan, ec));
    if (ui::Button("ArmaMan (Samples)")) host_.OutfitBody(armaMan);
    if (ui::Hovered()) ui::SetTooltip("A3_character_example.p3d of Arma 3 Samples (Setup and paths: Samples) as the body");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::Button("P3D file...##body"))
        if (auto f = host_.PickP3d(); !f.empty()) host_.OutfitBody(f);
    if (ui::Hovered()) ui::SetTooltip("Any character P3D with a model.cfg skeleton as the body");
    ImGui::SameLine();
    if (ui::Button("Open model##body") && session_.hasModel) host_.OutfitBody(session_.model.path);
    if (ui::Hovered()) ui::SetTooltip("The open model as the body (its parts are kept)");
    ui::TextDisabled("A soldier class as the body: the Class tab. The parts stay when the body changes.");
    // parts from files
    ui::TextUnformatted("Part from a file:");
    ImGui::SameLine();
    static const char* slots[] = { "head", "headgear", "goggles", "vest", "nvg", "backpack", "other" };
    ImGui::SetNextItemWidth(110 * ImGui::GetIO().FontGlobalScale);
    ui::Combo("##fileslot", &fileSlot_, slots, int(UnitSlot::Count));
    if (ui::Hovered()) ui::SetTooltip("Slot of the part: it replaces the part of that slot (other: added)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!session_.hasModel);
    if (ui::Button("Add P3D..."))
        if (auto f = host_.PickP3d(); !f.empty()) host_.AddUnitFile(UnitSlot(fileSlot_), f);
    if (ui::Hovered()) ui::SetTooltip("A helmet, vest, backpack ... P3D: skinned with the body's skeleton by bone (selection) names");
    ImGui::EndDisabled();
    // control
    bool ctl = host_.ControlOn();
    ImGui::BeginDisabled(!session_.unit.Active());
    if (ui::Checkbox("Control (WASD)##outfit", &ctl)) host_.SetControl(ctl);
    if (ui::Hovered()) ui::SetTooltip("CfgMoves control mode with this outfit: WASD walk, Shift run, Q / E turn, C crouch, Z prone");
    ImGui::EndDisabled();
    // presets
    if (!presetsRead_) { presets_ = host_.OutfitPresets(); presetsRead_ = true; }
    ImGui::SetNextItemWidth(160 * ImGui::GetIO().FontGlobalScale);
    ui::InputTextWithHint("##presetname", "preset name", presetName_, sizeof(presetName_));
    ImGui::SameLine();
    ImGui::BeginDisabled(!session_.unit.Active() || !presetName_[0]);
    if (ui::Button("Save outfit preset") && host_.SaveOutfitPreset(presetName_)) presetsRead_ = false;
    if (ui::Hovered()) ui::SetTooltip("Saves the body and the parts (classes and files) as a preset in the settings folder (outfits)");
    ImGui::EndDisabled();
    for (const auto& n : presets_)
    {
        ImGui::PushID(n.c_str());
        if (ui::SmallButton("Load")) host_.LoadOutfitPreset(n);
        ImGui::SameLine();
        if (ui::SmallButton("Delete")) { host_.DeleteOutfitPreset(n); presetsRead_ = false; }
        if (ui::Hovered()) ui::SetTooltip("Moves the preset file to the recycle bin");
        ImGui::SameLine();
        ImGui::TextUnformatted(n.c_str());
        ImGui::PopID();
    }
    if (presets_.empty()) ui::TextDisabled("No saved presets");
}

bool UnitPanel::PickFromList(const char* id, const std::vector<std::string>& items, char* filter, size_t filterSize, std::string& picked)
{
    bool done = false;
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(-1);
    ui::InputTextWithHint("##filter", "filter", filter, filterSize);
    if (ImGui::BeginChild("##list", ImVec2(0, 160 * ImGui::GetIO().FontGlobalScale), ImGuiChildFlags_Borders))
    {
        int shown = 0;
        for (const auto& s : items)
        {
            if (!Matches(s, filter)) continue;
            if (++shown > 400) { ui::TextDisabled("... (filter to see more)"); break; }
            if (ui::SelectableRaw(s.c_str())) { picked = s; done = true; }
        }
        if (!shown) ui::TextDisabled("(none)");
    }
    ImGui::EndChild();
    ImGui::PopID();
    return done;
}

void UnitPanel::DrawClassPicker()
{
    const ArmaConfig* cfg = host_.UnitConfig();
    if (!cfg)
    {
        ui::TextDisabled("No config loaded (open a model of a mod, or wait for the game configs)");
        return;
    }
    if (cfg != listedFor_)
    {
        listedFor_ = cfg;
        soldiers_ = Unit::SoldierClasses(*cfg);
        for (auto& r : candidatesRead_) r = false;
    }
    // classes that use the open model
    if (session_.hasModel)
        if (const CfgClass* c = FindVehicleClass(*cfg, ArmaPathOf(session_.model.path, session_.projectRoot)))
            if (!session_.unit.Active() || !IEquals(session_.unit.Class(), c->name))
            {
                if (ImGui::Button(std::format("{} {}##own", T("Assemble"), c->name).c_str())) host_.AssembleUnit(c->name);
                ui::TrackId("##own");
                if (ui::Hovered()) ui::SetTooltip("The CfgVehicles class whose model= is the open model");
            }
    if (ImGui::CollapsingHeader(std::format("{} ({})###unitclasses", T("Soldier classes"), soldiers_.size()).c_str()))
    {
        std::string picked;
        if (PickFromList("classes", soldiers_, classFilter_, sizeof(classFilter_), picked)) host_.AssembleUnit(picked);
        ui::TextDisabled("Click a class: its model is opened (if another one) and dressed");
    }
}

void UnitPanel::DrawParts()
{
    Unit& u = session_.unit;
    const ArmaConfig* cfg = host_.UnitConfig();
    bool changed = false;
    int removeIndex = -1;
    if (ImGui::BeginTable("##unitparts", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp))
    {
        ui::TableSetupColumn("Show", ImGuiTableColumnFlags_WidthFixed);
        ui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed);
        ui::TableSetupColumn("Class");
        ui::TableSetupColumn("State");
        ui::TableSetupColumn("Checks");
        ui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        int i = 0;
        for (auto& p : u.Parts())
        {
            ImGui::PushID(i++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (p.problem.empty())
            {
                changed |= ImGui::Checkbox("##show", &p.enabled);
                ui::TrackId("##show");
                if (ImGui::IsItemHovered()) ui::SetTooltip("Shows or hides this part.");
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(UnitSlotName(p.slot));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.cls.empty() ? "-" : p.cls.c_str());
            if (ImGui::IsItemHovered()) ui::SetTooltip("%s\n%s", p.from.c_str(), p.file.empty() ? p.armaModel.c_str() : PathToUtf8(p.file).c_str());
            ImGui::TableNextColumn();
            if (!p.problem.empty()) ImGui::TextColored(kErrorColor, "%s", p.problem.c_str());
            else if (p.unboundPoints) ImGui::TextColored(kWarnColor, "%d point(s) on no bone of the skeleton", p.unboundPoints);
            else ImGui::TextUnformatted(p.note.empty() ? "ok" : p.note.c_str());
            if (p.problem.empty() && p.unboundPoints && ImGui::IsItemHovered())
            {
                std::string s;
                for (const auto& n : p.unboundSelections) s += n + "\n";
                ui::SetTooltip("They stay in the bind pose. Selections:\n%s", s.c_str());
            }
            ImGui::TableNextColumn();
            if (p.problem.empty())
            {
                const std::string c = p.Checks();
                if (c.empty()) ImGui::TextUnformatted("ok");
                else ImGui::TextColored(kWarnColor, "%s", c.c_str());
                if (ImGui::IsItemHovered())
                    ui::SetTooltip("Against the body's skeleton: bones of the part (its model.cfg) the body lacks, points on more than 4 "
                                   "bones (the engine keeps 4), bone weights not summing to 1.");
            }
            ImGui::TableNextColumn();
            if (p.fromFile) { if (ui::SmallButton("Remove")) removeIndex = i - 1; }
            else if (ui::SmallButton("Replace")) replaceSlot_ = int(p.slot);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeIndex >= 0) { host_.RemoveUnitPart(size_t(removeIndex)); return; }
    if (changed) host_.UnitPartsChanged();
    // add / replace a part
    static const char* slots[] = { "head", "headgear", "goggles", "vest", "nvg", "backpack", "other" };
    ImGui::SetNextItemWidth(140 * ImGui::GetIO().FontGlobalScale);
    int slot = replaceSlot_ < 0 ? 1 : replaceSlot_;
    if (ui::Combo("Part slot", &slot, slots, int(UnitSlot::Count) - 1)) replaceSlot_ = slot; // other: files only (Outfit tab)
    if (replaceSlot_ >= 0 && cfg)
    {
        if (!candidatesRead_[replaceSlot_])
        {
            candidates_[replaceSlot_] = Unit::Candidates(*cfg, UnitSlot(replaceSlot_));
            candidatesRead_[replaceSlot_] = true;
        }
        ImGui::SameLine();
        if (ui::SmallButton("Remove part")) { host_.ReplaceUnitPart(UnitSlot(replaceSlot_), ""); replaceSlot_ = -1; return; }
        ImGui::SameLine();
        if (ui::SmallButton("Close##replace")) { replaceSlot_ = -1; return; }
        std::string picked;
        if (PickFromList("parts", candidates_[replaceSlot_], partFilter_, sizeof(partFilter_), picked))
        {
            host_.ReplaceUnitPart(UnitSlot(replaceSlot_), picked);
            replaceSlot_ = -1;
        }
    }
}
