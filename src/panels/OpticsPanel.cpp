#include "OpticsPanel.h"
#include "Lang.h"
#include "Text.h"

#include <imgui.h>

#include <glm/gtx/euler_angles.hpp>

#include <format>

namespace {
constexpr float kDeg = 57.2957795f;
} // namespace

void OpticsPanel::Draw(const std::string& windowName)
{
    if (!ui::Begin(windowName.c_str()))
    {
        ImGui::End();
        return;
    }
    ui::HelpMarker("Optics: attachments on the weapon's slots and the sight picture.\n"
                   "1. Open the weapon P3D - slots come from WeaponSlotsInfo (CowsSlot, PointerSlot, MuzzleSlot, UnderBarrelSlot).\n"
                   "2. Pick an item per slot: its model sits on the slot's proxy.\n"
                   "3. View through the sight / Camera at the eye: the camera goes to memoryPointCamera with opticsZoom.\n"
                   "4. Character aiming poses the character with the weapon.\n"
                   "5. compatibleItems of the slot can be edited (with .bak) - then Validation.");
    ImGui::SameLine();
    if (!s_.hasModel || !s_.weapon.found)
    {
        ui::EmptyState(s_.hasModel ? "No CfgWeapons class with this model - check the config.cpp of the addon."
                                   : "Open a weapon P3D (MLOD) with Ctrl+O: its slots and optics are read from CfgWeapons.");
        ImGui::End();
        return;
    }
    ui::Text("%s", s_.weapon.cls.c_str());
    ui::TextDisabled("%s", s_.weapon.ref.Where().c_str());
    DrawSlots();
    DrawSight();
    DrawFit();
    ImGui::End();
}

// slot -> proxy (in this LOD?) -> item, the slot whose item is the sight
void OpticsPanel::DrawSlots()
{
    const float dpi = host_.Dpi();
    const auto& slots = s_.weapon.slots;
    bool changed = false;
    if (ImGui::BeginTable("##slots", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ui::TableSetupColumn("slot", ImGuiTableColumnFlags_WidthFixed, 120 * dpi);
        ui::TableSetupColumn("proxy");
        ui::TableSetupColumn("item", ImGuiTableColumnFlags_WidthFixed, 260 * dpi);
        ui::TableSetupColumn("sight", ImGuiTableColumnFlags_WidthFixed, 50 * dpi);
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(slots.size()); ++i)
        {
            const WeaponSlotCfg& sc = slots[size_t(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ui::TextUnformatted(sc.name.c_str());
            ImGui::TableNextColumn();
            bool inModel = false;
            for (const auto& s : s_.attach.Slots())
                if (NormalizeModelPath(s.armaPath) == NormalizeModelPath(sc.linkProxy)) inModel = true;
            ui::TextColored(inModel ? ImVec4(0.7f, 0.9f, 0.7f, 1) : ImVec4(1, 0.5f, 0.4f, 1), "%s%s", sc.linkProxy.c_str(), inModel ? "" : " (not in this LOD)");
            ImGui::TableNextColumn();
            std::string& cur = fit_.slotItem_[sc.name];
            ImGui::SetNextItemWidth(-1);
            if (ui::BeginCombo("##item", cur.empty() ? "(none)" : cur.c_str()))
            {
                if (ui::SelectableRaw("(none)", cur.empty()))
                {
                    cur.clear();
                    changed = true;
                }
                for (const auto& it : sc.items)
                    if (ui::SelectableRaw(it.c_str(), IEquals(it, cur)))
                    {
                        cur = it;
                        changed = true;
                        if (fit_.opticsSlot_ < 0) fit_.opticsSlot_ = i;
                    }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            if (ui::RadioButton("##sight", fit_.opticsSlot_ == i))
            {
                fit_.opticsSlot_ = fit_.opticsSlot_ == i ? -1 : i;
                fit_.opticsMode_ = 0;
                fit_.zoom = -1;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (changed) view_.SlotItemsChanged();
    // the chosen item: model status (a binarized model: why it is a frame)
    if (fit_.opticsSlot_ >= 0 && fit_.opticsSlot_ < int(slots.size()))
        for (const auto& s : s_.attach.Slots())
            if (s.item && NormalizeModelPath(s.armaPath) == NormalizeModelPath(slots[size_t(fit_.opticsSlot_)].linkProxy))
                ui::TextDisabled("%s: %s", PathToUtf8(s.item->file.filename()).c_str(),
                                 s.item->status == AttachedModel::Status::Ok ? "loaded" : s.item->message.c_str());
}

// sight modes, zoom, the view through it, its numbers against the bore line
void OpticsPanel::DrawSight()
{
    const auto& sight = fit_.sight_;
    ui::SeparatorText(("Sight: " + sight.from).c_str());
    for (int m = 0; m < int(sight.modes.size()); ++m)
    {
        if (m) ImGui::SameLine();
        const bool picked = ImGui::RadioButton(sight.modes[size_t(m)].name.c_str(), fit_.opticsMode_ == m); // a config class name: not translated
        ui::TrackId(sight.modes[size_t(m)].name.c_str());
        if (ImGui::IsItemHovered()) ui::SetTooltip("Optics mode of the sight (a class of OpticsModes): its zoom range and reticle.");
        if (picked)
        {
            fit_.opticsMode_ = m;
            fit_.zoom = -1;
        }
    }
    if (sight.zoomMax > sight.zoomMin)
    {
        float z = fit_.zoom > 0 ? fit_.zoom : sight.zoomInit;
        ImGui::SetNextItemWidth(200 * host_.Dpi());
        if (ui::SliderFloat("zoom (opticsZoom)", &z, sight.zoomMin, sight.zoomMax, "%.4f")) fit_.zoom = z;
    }
    ui::Checkbox("View through the sight", &fit_.throughSight);
    ImGui::SameLine();
    if (ui::Checkbox("Character aiming", &fit_.aimPose) && !fit_.aimPose) fit_.eyeView = false;
    if (fit_.aimPose)
    {
        ImGui::SameLine();
        ui::Checkbox("Camera at the eye", &fit_.eyeView);
    }
    if (fit_.eyeView) fit_.throughSight = false;
    if (!sight.problem.empty()) ui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", sight.problem.c_str());
    if (!sight.ok) return;
    ui::Text("sight point vs bore line (barrel frame: side / up / along from '%s'):", s_.weapon.muzzleEnd.c_str());
    ui::Text("  side %+.1f mm   height over the bore %+.1f mm   along %.1f mm", sight.offsetMm.x, sight.offsetMm.y, sight.offsetMm.z);
    ui::Text("  angle %.3f deg (elevation %.3f, windage %.3f)%s", sight.angleDeg, sight.elevDeg, sight.windDeg,
             sight.zeroM > 0 ? std::format("   lines cross at {:.0f} m", sight.zeroM).c_str() : "");
    if (sight.eyeOk) ui::Text("  eye (character 'pilot') to the sight point %.0f mm", sight.eyeDistMm);
    else if (fit_.aimPose) ui::TextDisabled("  eye distance: character not shown / no 'pilot' memory point");
    ui::TextDisabled("  FOV: vertical = 2 atan(opticsZoom) - model of the game's FOV value, centre alignment does not depend on it");
    if (!sight.note.empty()) ui::TextDisabled("  %s", sight.note.c_str());
}

// temporary fit of the sight item (viewer only) and the correction to make in the weapon or the optic P3D
void OpticsPanel::DrawFit()
{
    const auto& slots = s_.weapon.slots;
    if (fit_.opticsSlot_ < 0 || fit_.opticsSlot_ >= int(slots.size())) return;
    const WeaponSlotCfg& slot = slots[size_t(fit_.opticsSlot_)];
    if (fit_.slotItem_[slot.name].empty()) return;
    const float dpi = host_.Dpi();
    const std::string& sn = slot.name;
    ui::SeparatorText("Fit (viewer only, not written to the P3D)");
    ImGui::SetNextItemWidth(260 * dpi);
    ui::DragFloat3("move mm (x y z)", &fit_.fitPos_[sn].x, 0.05f, -200, 200, "%.2f");
    ImGui::SetNextItemWidth(260 * dpi);
    ui::DragFloat3("rotate deg (x y z)", &fit_.fitRot_[sn].x, 0.01f, -30, 30, "%.3f");
    ImGui::SameLine();
    if (ui::Button("Reset##fit"))
    {
        fit_.fitPos_[sn] = glm::vec3(0);
        fit_.fitRot_[sn] = glm::vec3(0);
    }
    const glm::vec3 p = fit_.fitPos_[sn], r = fit_.fitRot_[sn];
    if (p == glm::vec3(0) && r == glm::vec3(0)) return;
    ui::SeparatorText("Correction");
    ui::Text("Proxy %s in the weapon P3D: move x %+.2f  y %+.2f  z %+.2f mm, rotate x %+.3f  y %+.3f  z %+.3f deg about its corner",
             slot.linkProxy.c_str(), p.x, p.y, p.z, r.x, r.y, r.z);
    ui::TextDisabled("  Blender (a3ob: P3D x y z = Blender x z y): location X %+.2f  Y %+.2f  Z %+.2f mm", p.x, p.z, p.y);
    for (const auto& s : s_.attach.Slots())
        if (s.item && NormalizeModelPath(s.armaPath) == NormalizeModelPath(slot.linkProxy))
        {
            const glm::mat3 rx = glm::mat3(s.xf);
            const glm::vec3 local = glm::transpose(rx) * p;
            const glm::mat3 rw = glm::mat3(glm::eulerAngleYXZ(r.y / kDeg, r.x / kDeg, r.z / kDeg));
            const glm::mat3 rl = glm::transpose(rx) * rw * rx;
            float ly = 0, lx = 0, lz = 0;
            glm::extractEulerAngleYXZ(glm::mat4(rl), ly, lx, lz);
            ui::Text("or the optic P3D (%s): move all LODs x %+.2f  y %+.2f  z %+.2f mm, rotate x %+.3f  y %+.3f  z %+.3f deg about its origin",
                     PathToUtf8(s.item->file.filename()).c_str(), local.x, local.y, local.z, lx * kDeg, ly * kDeg, lz * kDeg);
            ui::TextDisabled("  Blender: location X %+.2f  Y %+.2f  Z %+.2f mm - after the export hot reload resets the fit", local.x, local.z, local.y);
            break;
        }
}
