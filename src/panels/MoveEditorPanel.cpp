#include "MoveEditorPanel.h"
#include "Lang.h"
#include "Log.h"
#include "Text.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cfloat>
#include <format>
#include <set>

namespace fs = std::filesystem;

namespace {
const ImVec4 kErr = { 1.0f, 0.4f, 0.4f, 1 };
const ImVec4 kWarn = { 1.0f, 0.8f, 0.3f, 1 };
const ImVec4 kGood = { 0.45f, 0.85f, 0.45f, 1 };
} // namespace

void MoveEditorPanel::Draw(const std::string& windowName, const std::string& dockWith)
{
    if (ImGuiWindow* tl = ImGui::FindWindowByName(dockWith.c_str()); tl && tl->DockId) ImGui::SetNextWindowDockID(tl->DockId, ImGuiCond_FirstUseEver);
    if (!ui::Begin(windowName.c_str()))
    {
        ImGui::End();
        DrawDiff();
        return;
    }
    const MovesGraph& moves = s_.moves;
    if (moves.Empty())
    {
        ui::TextDisabled("No CfgMoves for this model.");
        ImGui::End();
        return;
    }
    ed_.Open(s_.CrewConfig());
    if (ed_.pendingCheck && !ld_.Validating()) // saved: the hot reload rebuilt the graph - check it
    {
        ed_.pendingCheck = false;
        ld_.StartValidation(false);
        ed_.traps = FindMoveTraps(moves, false, nullptr, true);
    }
    DrawFileLine();
    const int sel = states_.graph.Selected() >= 0 ? states_.graph.Selected() : sim_.player.Current();
    if (sel >= 0 && sel < int(moves.states.size())) DrawState(sel);
    // new state
    if (s_.movesDoc.Loaded())
    {
        const float dpi = host_.Dpi();
        ui::SeparatorText("New state");
        ImGui::SetNextItemWidth(200 * dpi);
        ui::InputTextWithHint("##newname", "name", newName_, sizeof(newName_));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200 * dpi);
        ui::InputTextWithHint("##newbase", "base (your state, optional)", newBase_, sizeof(newBase_));
        ImGui::SameLine();
        if (ui::Button("Create") && newName_[0]) ed_.AddState(newName_, newBase_);
        DrawActions();
    }
    ImGui::End();
    DrawDiff();
}

// the own file: unsaved?, undo / redo, diff & save, revert
void MoveEditorPanel::DrawFileLine()
{
    ConfigDoc& doc = s_.movesDoc;
    if (doc.Loaded())
    {
        ui::TextColored(doc.Dirty() ? kWarn : kGood, "%s%s", PathToUtf8(doc.File()).c_str(), doc.Dirty() ? "  (unsaved)" : "");
        ImGui::SameLine();
        ImGui::BeginDisabled(!s_.commands.CanUndo());
        if (ui::SmallButton("Undo")) s_.commands.Undo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!s_.commands.CanRedo());
        if (ui::SmallButton("Redo")) s_.commands.Redo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!doc.Dirty());
        if (ui::SmallButton("Diff & Save...")) showDiff = true;
        ImGui::SameLine();
        if (ui::SmallButton("Revert##moves")) doc.Revert();
        ImGui::EndDisabled();
    }
    else
        ui::TextDisabled("BI move config: read only (copy states into your own config)");
    if (!ed_.msg.empty()) ui::TextWrapped("%s", ed_.msg.c_str());
}

void MoveEditorPanel::DrawState(int sel)
{
    const float dpi = host_.Dpi();
    const MoveState& s = s_.moves.states[size_t(sel)];
    const bool own = ed_.IsOwn(sel);
    ui::SeparatorText(TF("{} ({}{})", s.name, T(own ? "your config" : "BI, read only"), s.base.empty() ? std::string() : T(", base ") + s.base).c_str());
    auto field = [&](const char* label, const char* entry, std::string value, bool quote) {
        char buf[300];
        strncpy_s(buf, value.c_str(), _TRUNCATE);
        ImGui::SetNextItemWidth(-240 * dpi);
        ImGui::BeginDisabled(!own);
        if (ui::InputText(label, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue) && own) ed_.SetValue(sel, entry, buf, quote);
        ImGui::EndDisabled();
    };
    field("file (Enter)", "file", s.file, true);
    if (own) DrawRtmPicker(sel);
    field("speed (Enter)", "speed", std::format("{:g}", s.speed), false);
    ImGui::SameLine();
    ui::TextDisabled("= %.2f s per cycle", s.duration);
    field("interpolationSpeed", "interpolationSpeed", std::format("{:g}", s.interpolationSpeed), false);
    field("interpolationRestart", "interpolationRestart", std::to_string(s.interpolationRestart), false);
    field("looped (0/1)", "looped", s.looped ? "1" : "0", false);
    field("actions", "actions", s.actions, true);
    field("equivalentTo", "equivalentTo", s.equivalentTo, true);
    field("aiming", "aiming", s.aiming, true);
    field("aimingBody", "aimingBody", s.aimingBody, true);
    DrawLinks(sel, own);
    ui::TextDisabled("New link: Alt+drag from this state to another in the States graph (Ctrl+M); with Shift: connectTo");
    if (own)
    {
        ImGui::SetNextItemWidth(200 * dpi);
        ui::InputTextWithHint("##rename", "new name", rename_, sizeof(rename_));
        ImGui::SameLine();
        if (ui::Button("Rename") && rename_[0]) ed_.Rename(sel, rename_);
        ImGui::SameLine();
        if (ui::Button("Delete state")) ed_.Remove(sel);
    }
    else if (s_.movesDoc.Loaded())
    {
        ImGui::SetNextItemWidth(200 * dpi);
        ui::InputTextWithHint("##copyname", "name in your config", rename_, sizeof(rename_));
        ImGui::SameLine();
        if (ui::Button("Copy into my config")) ed_.CopyIntoOwn(sel, rename_);
        if (ui::Hovered())
            ui::SetTooltip("The BI class stays untouched: a new class with its file, speed, looped, interpolationSpeed, actions;\nlinks empty (add them in the graph)");
    }
}

// RTMs of the mod folder (next to / below the config): set as the state's file, or preview as the only layer
void MoveEditorPanel::DrawRtmPicker(int sel)
{
    ImGui::SameLine();
    if (ui::SmallButton("Pick RTM")) ImGui::OpenPopup("##pickrtm");
    if (!ImGui::BeginPopup("##pickrtm")) return;
    const fs::path dir = s_.movesDoc.File().parent_path();
    if (rtmChoices_.empty())
    {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
            if (it->is_regular_file(ec) && IEquals(PathToUtf8(it->path().extension()), ".rtm")) rtmChoices_.push_back(it->path());
    }
    for (const auto& p : rtmChoices_)
    {
        ImGui::PushID(PathToUtf8(p).c_str());
        if (ui::SelectableRaw(PathToUtf8(p.filename()).c_str())) ed_.SetFile(sel, "\\" + ArmaPathOf(p, s_.projectRoot));
        if (ui::Hovered()) ui::SetTooltip("%s", PathToUtf8(p).c_str());
        ImGui::SameLine();
        if (ui::SmallButton("preview"))
        {
            view_.SetMovesDrive(false);
            s_.anim.layers.clear();
            pb_.AddLayer(p);
        }
        ImGui::PopID();
    }
    if (rtmChoices_.empty()) ui::TextDisabled("no .rtm below %s", PathToUtf8(dir).c_str());
    ImGui::EndPopup();
}

void MoveEditorPanel::DrawLinks(int sel, bool own)
{
    const MoveState& s = s_.moves.states[size_t(sel)];
    if (!ImGui::BeginTable("##links", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) return;
    ui::TableSetupColumn("link to");
    ui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed);
    ui::TableSetupColumn("cost", ImGuiTableColumnFlags_WidthFixed, 90 * host_.Dpi());
    ui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (int interp = 0; interp < 2; ++interp)
    {
        const auto& list = interp ? s.interpolateRaw : s.connectRaw;
        for (size_t k = 0; k < list.size(); ++k)
        {
            ImGui::PushID(interp * 1000 + int(k));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const int t = s_.moves.Find(list[k].first);
            ui::TextColored(t < 0 ? kErr : ImVec4(1, 1, 1, 1), "%s", list[k].first.c_str());
            ImGui::TableNextColumn();
            ui::TextUnformatted(interp ? "interpolateTo" : "connectTo");
            ImGui::TableNextColumn();
            float c = list[k].second;
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::BeginDisabled(!own);
            bool changed = false;
            auto l2 = list;
            if (ui::InputFloat("##cost", &c, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue))
            {
                l2[k].second = c;
                changed = true;
            }
            ImGui::TableNextColumn();
            if (ui::SmallButton("x"))
            {
                l2.erase(l2.begin() + std::ptrdiff_t(k));
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            if (changed)
            {
                ed_.SetLinks(sel, interp, l2); // rebuilds the lists drawn here
                ImGui::EndTable();
                return;
            }
        }
    }
    ImGui::EndTable();
}

// the actions classes of the own states: drag a state onto an action to set it
void MoveEditorPanel::DrawActions()
{
    if (!ui::CollapsingHeader("Actions (drag a state onto an action)")) return;
    const MovesGraph& moves = s_.moves;
    ConfigDoc& doc = s_.movesDoc;
    ui::TextDisabled("Your states:");
    for (int i = 0; i < int(moves.states.size()); ++i)
    {
        if (!ed_.IsOwn(i) || moves.states[size_t(i)].abstract) continue;
        ImGui::SameLine();
        ui::SmallButtonRaw(moves.states[size_t(i)].name.c_str());
        if (ui::Hovered()) ui::SetTooltip("Drag %s onto an action below to set it", moves.states[size_t(i)].name.c_str());
        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload("MOVESTATE", &i, sizeof(int));
            ui::TextUnformatted(moves.states[size_t(i)].name.c_str());
            ImGui::EndDragDropSource();
        }
    }
    std::set<std::string> classes;
    for (int i = 0; i < int(moves.states.size()); ++i)
        if (ed_.IsOwn(i) && !moves.states[size_t(i)].actions.empty()) classes.insert(moves.states[size_t(i)].actions);
    std::string err;
    for (const auto& cls : classes)
    {
        const std::string path = moves.cls + "/Actions/" + cls;
        const bool own = doc.HasClass(path);
        if (!ImGui::TreeNode(TF("{}{}", cls, own ? "" : " (not in your file: read only)").c_str())) continue;
        static const char* const kKeys[] = { "Stop", "StopRelaxed", "Default", "WalkF", "WalkB", "WalkL", "WalkR", "WalkLF", "WalkRF", "SlowF", "FastF", "TurnL", "TurnR",
                                             "Up", "Down", "PlayerCrouch", "PlayerProne", "PlayerStand", "Crouch", "Lying", "Stand", "LimpF", "EvasiveForward" };
        const auto it = moves.actions.find(ToLower(cls));
        if (ImGui::BeginTable("##acts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
        {
            for (const char* k : kKeys)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ui::TextUnformatted(k);
                ImGui::TableNextColumn();
                std::string cur = "-";
                if (it != moves.actions.end())
                    if (auto jt = it->second.find(ToLower(k)); jt != it->second.end()) cur = jt->second;
                ImGui::PushID(k);
                ui::SelectableRaw(cur.c_str());
                if (own && ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("MOVESTATE"))
                    {
                        const int st = *static_cast<const int*>(p->Data);
                        if (doc.SetEntry(path, k, MovesEditor::Quote(moves.states[size_t(st)].name), false, err))
                            ed_.msg = std::format("{} >> {} = {} (pending)", cls, k, moves.states[size_t(st)].name);
                        else
                            ed_.msg = err;
                    }
                    ImGui::EndDragDropTarget();
                }
                if (own && ImGui::BeginPopupContextItem("##actctx"))
                {
                    if (ui::MenuItem("Remove from the class") && !doc.RemoveEntry(path, k, err)) ed_.msg = err;
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}

// the changes of the own file before saving (a backup is kept); after saving the hot reload rebuilds and checks
void MoveEditorPanel::DrawDiff()
{
    if (!showDiff) return;
    ConfigDoc& doc = s_.movesDoc;
    const float dpi = host_.Dpi();
    ImGui::SetNextWindowSize(ImVec2(900 * dpi, 600 * dpi), ImGuiCond_FirstUseEver);
    if (!ui::Begin("Move config: changes before saving", &showDiff))
    {
        ImGui::End();
        return;
    }
    ui::TextUnformatted(PathToUtf8(doc.File()).c_str());
    ImGui::SameLine();
    if (ui::Button("Save (backup kept)"))
    {
        std::string err;
        if (doc.Save(err))
        {
            ed_.msg = std::format("saved {} (backup {}); hot reload + validation", PathToUtf8(doc.File().filename()), PathToUtf8(doc.LastBackup().filename()));
            Log::Get().Info("Move editor: {}", ed_.msg);
            ed_.pendingCheck = true;
            showDiff = false;
        }
        else
            ed_.msg = err;
    }
    ImGui::SameLine();
    if (ui::Button("Cancel")) showDiff = false;
    if (ImGui::BeginChild("##diff", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
        for (const auto& d : doc.Diff())
        {
            const ImVec4 c = d.kind == '+' ? kGood : d.kind == '-' ? kErr : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            ui::TextColored(c, "%c %5s  %s", d.kind == '~' ? ' ' : d.kind, d.kind == '+' ? std::to_string(d.newLine).c_str() : d.kind == '~' ? "" : std::to_string(d.oldLine).c_str(),
                            d.text.c_str());
        }
    ImGui::EndChild();
    ImGui::End();
}
