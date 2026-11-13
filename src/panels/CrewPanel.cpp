#include "CrewPanel.h"
#include "Lang.h"
#include "Log.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace fs = std::filesystem;

namespace {
const ImVec4 kOkColor{ 0.9f, 0.9f, 0.9f, 1 }, kWarnColor{ 1.0f, 0.8f, 0.3f, 1 }, kErrorColor{ 1.0f, 0.4f, 0.4f, 1 };
} // namespace

void CrewPanel::Draw(const std::string& windowName)
{
    if (!ui::Begin(windowName.c_str()))
    {
        ImGui::End();
        return;
    }
    auto& seats = session_.attach.Seats();
    bool rebuild = false, resolve = false;
    ui::Text("CfgVehicles::%s", session_.attach.VehicleClass().empty() ? "(no class with this model)" : session_.attach.VehicleClass().c_str());
    ImGui::SameLine();
    ui::TextDisabled(session_.game ? "| game configs loaded" : "| game configs: loading / not on P:");
    rebuild |= ui::Checkbox("Show crew", &session_.attach.showCrew);
    ImGui::SameLine();
    rebuild |= ui::Checkbox("Show proxies", &session_.attach.showProxies);
    ImGui::SameLine();
    rebuild |= ui::Checkbox("Show effects", &session_.attach.showEffects);
    if (ui::Hovered()) ui::SetTooltip("Muzzle flash / light volume proxies and gear-slot placeholders (weapon, launcher, headgear ...: the game fills them)");
    ImGui::SameLine();
    rebuild |= ui::Checkbox("Destroyed state", &session_.attach.showDestroyed);
    if (ui::Hovered()) ui::SetTooltip("Wreck proxies (*wreck*.p3d): the game shows them only when the vehicle is destroyed");
    ImGui::SameLine();
    if (ui::Checkbox("Hide vehicle", &hideVehicle)) host_.RebuildDrawSections();
    resolve |= ui::Checkbox("Turned out (*Action instead of *InAction)", &session_.attach.turnedOut);
    ImGui::SameLine();
    if (ui::Checkbox("IK hands / legs", &session_.attach.ik)) { session_.attach.RefreshIK(); host_.PoseChanged(); resolve = true; }
    if (ui::Hovered()) ui::SetTooltip("Two-bone IK: hands / feet follow the vehicle bones of driverLeftHandAnimName, driverRightHandAnimName,\ndriverLeftLegAnimName, driverRightLegAnimName (gunner* for turrets)");
    ImGui::SameLine();
    ui::Checkbox("Real get-in position", &session_.attach.realGetIn);
    if (ui::Hovered()) ui::SetTooltip("Get in / out states play at memoryPointsGetIn<Driver|Gunner|Cargo>, facing from ...Dir to it");
    if (ui::CollapsingHeader("Advanced##crew"))
    {
        resolve |= ui::Checkbox("Crew missing bones: follow parent", &session_.attach.followParent);
        if (ui::Hovered()) ui::SetTooltip("On (default): bones without RTM data follow their parent.\nOff: they stay in the bind pose (identity, as OFP / CWR does - not confirmed for Arma 3; stretching shows).");
        if (!modelEdit_[0]) strncpy_s(modelEdit_, session_.attach.crewModel.c_str(), _TRUNCATE);
        ImGui::SetNextItemWidth(-160);
        ui::InputText("##crewmodel", modelEdit_, sizeof(modelEdit_));
        ImGui::SameLine();
        if (ui::Button("Set crew model"))
        {
            session_.attach.crewModel = modelEdit_;
            resolve = true;
        }
        if (ui::Hovered()) ui::SetTooltip("MLOD character with model.cfg skeleton, loaded once for all seats");
    }

    // playback (all seats)
    ui::Checkbox("Play all", &session_.attach.playAll);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * host_.Dpi());
    ui::SliderFloat("##crewspeed", &session_.attach.speedAll, 0.0f, 3.0f, "speed x %.2f");
    ImGui::SameLine();
    ui::Checkbox("Loop##crew", &session_.attach.loopAll);
    ImGui::SameLine();
    if (ui::Button("Rewind")) for (auto& s : seats) { s.time = 0.0; s.chainTime = 0.0; if (s.rtm && !s.posed.empty()) session_.attach.PoseSeatAt(s, *s.rtm, 0.0f); host_.PoseChanged(); }
    if (ImGui::BeginTable("seats", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0, std::max(80.0f * host_.Dpi(), ImGui::GetContentRegionAvail().y))))
    {
        ui::TableSetupColumn("Seat");
        ui::TableSetupColumn("Proxy");
        ui::TableSetupColumn("Action");
        ui::TableSetupColumn("State");
        ui::TableSetupColumn("RTM");
        ui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(seats.size()); ++i)
        {
            CrewSeat& s = seats[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ui::SelectableRaw(s.title.c_str(), selected == i, ImGuiSelectableFlags_SpanAllColumns))
            {
                selected = i;
                host_.InspectSeat();
            }
            ImGui::TableNextColumn();
            ui::Text("%s.%03d%s", SeatRoleName(s.role), s.proxyIndex, s.slot < 0 ? " (none)" : "");
            ImGui::TableNextColumn();
            ui::TextUnformatted(s.action.empty() ? "-" : s.action.c_str());
            if (!s.actionRef.file.empty())
            {
                if (ui::Hovered()) ui::SetTooltip("%s", s.actionRef.Where().c_str());
                ImGui::SameLine();
                if (ui::SmallButton("open##a")) host_.OpenInEditor(s.actionRef.file, s.actionRef.line);
            }
            ImGui::TableNextColumn();
            ui::TextUnformatted(s.state.empty() ? "-" : s.state.c_str());
            if (!s.stateRef.file.empty())
            {
                if (ui::Hovered()) ui::SetTooltip("ManActions: %s\nState: %s", s.manActionRef.Where().c_str(), s.stateRef.Where().c_str());
                ImGui::SameLine();
                if (ui::SmallButton("open##s")) host_.OpenInEditor(s.stateRef.file, s.stateRef.line);
            }
            ImGui::TableNextColumn();
            std::string rtm = !s.overrideRtm.empty() ? "[manual] " + PathToUtf8(fs::path(Utf8ToWide(s.overrideRtm)).filename())
                            : s.rtmArma.empty() ? "-" : s.rtmArma;
            ui::TextUnformatted(rtm.c_str());
            ImGui::TableNextColumn();
            if (!s.problem.empty()) ui::TextColored(kErrorColor, "%s", s.problem.c_str());
            else if (s.binarized) ui::TextColored(kWarnColor, "binarized animation - not supported (frame)");
            else if (!s.posed.empty())
            {
                ui::TextColored(kOkColor, "%zu fr, %s", s.rtm ? s.rtm->Frames() : 0,
                                   s.duration > 0 ? std::format("{:.2f} s", s.duration).c_str() : "static");
                ImGui::SameLine();
                ui::Checkbox("play##seat", &s.playing);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    Apply(resolve, rebuild);
    ImGui::End();
}

void CrewPanel::DrawSeatInspector()
{
    auto& seats = session_.attach.Seats();
    bool rebuild = false, resolve = false;
    if (selected >= 0 && selected < int(seats.size()))
    {
        CrewSeat& s = seats[selected];
        ui::Text("%s: manual RTM (empty = from config)", s.title.c_str());
        if (editFor_ != selected)
        {
            strncpy_s(rtmEdit_, s.overrideRtm.c_str(), _TRUNCATE);
            editFor_ = selected;
        }
        ImGui::SetNextItemWidth(-220);
        ui::InputTextWithHint("##rtmov", "P:\\...\\file.rtm", rtmEdit_, sizeof(rtmEdit_));
        ImGui::SameLine();
        if (ui::Button("Apply RTM")) { s.overrideRtm = rtmEdit_; resolve = true; }
        ImGui::SameLine();
        if (ui::Button("From config")) { s.overrideRtm.clear(); rtmEdit_[0] = 0; resolve = true; }
        if (!s.rtmFile.empty()) ui::TextDisabled("%s", PathToUtf8(s.rtmFile).c_str());
        ImGui::SetNextItemWidth(120 * host_.Dpi());
        ui::SliderFloat("seat speed", &s.speed, 0.0f, 3.0f, "x %.2f");
        // transitions: related states -> chain
        if (!s.state.empty())
        {
            ui::TextDisabled("related to %s:", s.state.c_str());
            for (size_t k = 0; k < s.related.size(); ++k)
            {
                ImGui::SameLine();
                if (ui::SmallButton(TF("{}: {}##rel{}", s.related[k].first, s.related[k].second, k).c_str()))
                {
                    // get in: get-in state, then the seat; otherwise: seat, then the clicked state
                    const std::string lk = ToLower(s.related[k].first);
                    const bool getIn = lk.find("getin") != std::string::npos, getOut = lk.find("getout") != std::string::npos;
                    if (getIn)
                    {
                        s.chain.clear();
                        session_.attach.AddToChain(s, s.related[k].second, session_.CrewConfig(), session_.projectRoot, true);
                        session_.attach.AddToChain(s, s.state, session_.CrewConfig(), session_.projectRoot);
                    }
                    else
                    {
                        if (s.chain.empty()) session_.attach.AddToChain(s, s.state, session_.CrewConfig(), session_.projectRoot);
                        session_.attach.AddToChain(s, s.related[k].second, session_.CrewConfig(), session_.projectRoot, getOut);
                    }
                    session_.attach.PlayChain(s);
                    host_.PoseChanged();
                }
                if (ui::Hovered()) ui::SetTooltip("Play as a chain: get in -> seat, or seat -> this state (blend 1 / interpolationSpeed)");
            }
            if (!s.chain.empty())
            {
                ui::TextDisabled("chain:");
                for (size_t k = 0; k < s.chain.size(); ++k)
                {
                    ImGui::SameLine();
                    bool cur = int(k) == s.chainIndex;
                    ui::TextColored(cur ? ImVec4(1, 0.4f, 1, 1) : ImVec4(0.8f, 0.8f, 0.85f, 1), "%s%s (%.2f s, interp %.0f)", k ? "-> " : "",
                                       s.chain[k].state.c_str(), s.chain[k].duration, s.chain[k].interpolationSpeed);
                }
                if (ui::Button("Play chain")) session_.attach.PlayChain(s);
                ImGui::SameLine();
                if (ui::Button("Clear chain")) { s.chain.clear(); s.chainIndex = -1; s.poseA = s.poseB = nullptr; if (s.rtm) session_.attach.PoseSeatAt(s, *s.rtm, 0.0f); host_.PoseChanged(); }
            }
        }
    }
    Apply(resolve, rebuild);
}

void CrewPanel::Apply(bool resolve, bool rebuild)
{
    if (resolve && session_.hasModel)
    {
        session_.attach.ResolveCrew(session_.CrewConfig(), session_.projectRoot);
        rebuild = true;
        host_.WatchedFilesChanged();
    }
    if (rebuild && session_.hasModel) host_.RebuildGeometry();
}
