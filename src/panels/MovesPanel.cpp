#include "MovesPanel.h"
#include "Lang.h"
#include "Log.h"
#include "Text.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <format>
#include <fstream>

namespace fs = std::filesystem;

namespace {
const ImVec4 kErr = { 1.0f, 0.4f, 0.4f, 1 };
const ImVec4 kWarn = { 1.0f, 0.8f, 0.3f, 1 };
const ImVec4 kGood = { 0.45f, 0.85f, 0.45f, 1 };

bool ContainsNoCase(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                       [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); }) != hay.end();
}
} // namespace

void MovesPanel::Draw(const std::string& windowName, const std::string& dockWith)
{
    ld_.Ensure(false);
    // first use: dock next to the Timeline (layouts built before this panel existed)
    if (ImGuiWindow* tl = ImGui::FindWindowByName(dockWith.c_str()); tl && tl->DockId) ImGui::SetNextWindowDockID(tl->DockId, ImGuiCond_FirstUseEver);
    if (!ui::Begin(windowName.c_str()))
    {
        ImGui::End();
        return;
    }
    ld_.PollValidation();
    if (DrawHeader())
    {
        DrawStartState();
        DrawCurrentState();
        DrawActions();
        DrawLog();
        // details (collapsed)
        DrawGestures();
        DrawControl();
        DrawValidation();
    }
    ImGui::End();
}

// which move config, its skeleton against the model's, the States graph
bool MovesPanel::DrawHeader()
{
    const MovesGraph& moves = s_.moves;
    if (!s_.hasModel)
    {
        ui::EmptyState("Open a character model (Ctrl+O): the Moves panel plays its CfgMoves (the move config of its skeleton).");
        return false;
    }
    if (!s_.CrewConfig())
    {
        ui::EmptyState(view_.GameConfigsLoading() ? "Game configs are loading..."
                                                  : "No configs: put a config.cpp with CfgMoves next to the model, or the game data on P:\\a3.");
        return false;
    }
    ImGui::SetNextItemWidth(220 * host_.Dpi());
    if (ui::BeginCombo("CfgMoves", ld_.chosen.empty() ? TF("auto: {}", moves.cls).c_str() : ld_.chosen.c_str()))
    {
        if (ui::SelectableRaw(T("auto (by the model's skeleton)"), ld_.chosen.empty()))
        {
            ld_.chosen.clear();
            ld_.Rebuild();
        }
        for (const auto& c : ld_.classes)
            if (ui::SelectableRaw(c.c_str(), IEquals(c, ld_.chosen)))
            {
                ld_.chosen = c;
                ld_.Rebuild();
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ui::HelpMarker("CfgMoves in the viewer, as in the game:\n"
                   "1. Start state: where the character begins (auto = the Default action of its actions class, as the engine).\n"
                   "2. Game mode / Control mode: CfgMoves drives the animation; in control mode W A S D, Shift, C, Z, X and Q / E move the character.\n"
                   "3. Actions: buttons of the current state's actions class - each walks the cheapest ConnectTo / InterpolateTo path.\n"
                   "4. Transition log: every step with its path; click a line to replay it. Reset: start state at the origin.\n"
                   "Death / unconscious states are entered only by an explicit click, never by a key or at the start.");
    const Skeleton& sk = s_.skeleton;
    if (moves.Empty())
    {
        ui::EmptyState(sk.Empty() ? "This model has no skeleton (CfgModels skeletonName in its model.cfg): CfgMoves needs one."
                                  : "No CfgMoves for this skeleton. Add a move config (class CfgMovesX { skeletonName = \"...\"; class States {...}; }) "
                                    "to the config.cpp next to the model, or pick one in the list above.");
        return false;
    }
    ImGui::SameLine();
    const bool skelOk = !sk.Empty() && IEquals(sk.name, moves.skeletonName);
    ui::TextColored(skelOk ? kGood : kWarn, "skeleton %s", moves.skeletonName.c_str());
    ImGui::SameLine();
    ui::TextDisabled("| %zu states", moves.states.size());
    ImGui::SameLine();
    if (ui::SmallButton("States graph (Ctrl+M)")) view_.ToggleStatesGraph();
    if (!skelOk)
        ui::TextColored(kWarn, "The model's skeleton is %s, this CfgMoves is for %s: the RTMs will not fit. Pick the move config of %s in the list (or add one).",
                        sk.Empty() ? "-" : sk.name.c_str(), moves.skeletonName.c_str(), sk.Empty() ? "-" : sk.name.c_str());
    return true;
}

void MovesPanel::DrawStartState()
{
    const MovesGraph& moves = s_.moves;
    ui::SeparatorText("1. Start state");
    const std::string key = ToLower(PathToUtf8(s_.model.path));
    std::string& pick = sim_.startPick[key];
    std::string why;
    const int autoStart = moves.StartState(why);
    const std::string autoName = autoStart >= 0 ? moves.states[size_t(autoStart)].name : T("none");
    ImGui::SetNextItemWidth(300 * host_.Dpi());
    if (ui::BeginCombo("Start state", pick.empty() ? TF("auto: {}", autoName).c_str() : pick.c_str()))
    {
        if (ui::SelectableRaw(TF("auto: {}", autoName).c_str(), pick.empty()))
        {
            pick.clear();
            ImGui::MarkIniSettingsDirty();
        }
        for (int i = 0; i < int(moves.states.size()); ++i)
        {
            const MoveState& s = moves.states[size_t(i)];
            if (s.abstract || !s.fileExists || moves.IsDeathLike(i)) continue; // death: never a start state
            if (ui::SelectableRaw(s.name.c_str(), IEquals(pick, s.name)))
            {
                pick = s.name;
                ImGui::MarkIniSettingsDirty();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ui::Button("Reset##moves")) view_.MovesReset();
    const int start = sim_.StartState(&sim_.startWhy);
    if (start < 0)
        ui::TextColored(kWarn, "No start state: %s. Check the file= paths of the idle state (the viewer looks on P:\\ and in the mod folder under its prefix).",
                        sim_.startWhy.c_str());
    else if (!moves.states[size_t(start)].fileExists)
        ui::TextColored(kWarn, "The start state %s has no RTM on disk (%s).", moves.states[size_t(start)].name.c_str(), moves.states[size_t(start)].file.c_str());
    else
    {
        ImGui::PushTextWrapPos(0.0f);
        ui::TextDisabled("why: %s", sim_.startWhy.c_str());
        ImGui::PopTextWrapPos();
    }
}

void MovesPanel::DrawCurrentState()
{
    const MovesGraph& moves = s_.moves;
    const MovesPlayer& p = sim_.player;
    ui::SeparatorText("2. Current state");
    bool drive = sim_.drive;
    if (ui::Checkbox("Game mode: CfgMoves drives the animation", &drive)) view_.SetMovesDrive(drive);
    ImGui::SameLine();
    bool control = ctl_.on;
    if (ui::Checkbox("Control mode", &control)) view_.SetControlMode(control);
    const int cur = p.Current();
    if (cur >= 0)
    {
        const MoveState& s = moves.states[size_t(cur)];
        ui::TextColored(moves.IsDeathLike(cur) ? kErr : ImVec4(1, 0.45f, 1, 1), "%s", s.name.c_str());
        ImGui::SameLine();
        ui::TextDisabled("phase %.2f, %.2f s%s, actions %s", p.PhaseOf(moves, p.Cur(), pb_.time), s.duration, s.looped ? T(" looped") : "", s.actions.c_str());
        if (p.Previous() >= 0)
        {
            ImGui::SameLine();
            ui::TextDisabled("| blending from %s (%.0f%%)", moves.states[size_t(p.Previous())].name.c_str(), 100.0f * p.BlendWeight(moves, pb_.time));
        }
        if (!p.Queue().empty())
        {
            std::string q;
            for (int x : p.Queue()) q += " > " + moves.states[size_t(x)].name;
            ui::TextColored(kWarn, "queued%s", q.c_str());
        }
    }
    else
        ui::EmptyState("Not playing: tick Game mode or Control mode, or press Reset - the character goes to the start state.");
    if (ctl_.on)
        ui::TextDisabled("W A S D walk, Shift+W run, C crouch / stand, Z prone / stand, X stand up, Q / E turn | position %.2f %.2f, heading %.0f deg, key action %s",
                         ctl_.rootPos.x, ctl_.rootPos.z, ctl_.rootYaw, ctl_.action.empty() ? "-" : ctl_.action.c_str());
}

// playMove / switchMove by name; buttons of the current state's actions class
void MovesPanel::DrawActions()
{
    const MovesGraph& moves = s_.moves;
    ui::SeparatorText("3. Actions");
    ImGui::SetNextItemWidth(260 * host_.Dpi());
    const bool enter = ui::InputTextWithHint("##movecmd", "state or action (e.g. WalkF, Down, AmovPpneMstpSrasWrflDnon)", cmd_, sizeof(cmd_),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ui::Button("playMove") || enter) && cmd_[0]) view_.MovesPlay(cmd_, false, true);
    ImGui::SameLine();
    if (ui::Button("switchMove") && cmd_[0]) view_.MovesPlay(cmd_, true, true);
    if (!sim_.msg.empty()) ui::TextWrapped("%s", sim_.msg.c_str());
    const int cur = sim_.player.Current();
    if (cur < 0) return;
    static const char* const kMain[] = { "Stop", "StopRelaxed", "Default", "Up", "Down", "PlayerStand", "PlayerCrouch", "PlayerProne",
                                         "Stand", "Crouch", "Lying", "WalkF", "WalkB", "WalkL", "WalkR", "WalkLF", "WalkRF", "WalkLB", "WalkRB",
                                         "SlowF", "SlowB", "FastF", "FastB", "TactF", "EvasiveForward", "EvasiveLeft", "EvasiveRight", "TurnL",
                                         "TurnR", "Combat", "Civil", "PrimaryWeapon", "SecondaryWeapon", "HandGunOn", "Binoculars", "Salute",
                                         "SitDown", "Surrender", "ReloadMagazine", "LimpF", "Diary" };
    std::vector<std::pair<std::string, int>> main, other;
    for (const auto& a : moves.ActionsOf(cur))
        (std::any_of(std::begin(kMain), std::end(kMain), [&](const char* k) { return IEquals(a.first, k); }) ? main : other).push_back(a);
    std::string play;
    auto buttons = [&](const std::vector<std::pair<std::string, int>>& acts, int idBase) {
        float x = 0;
        const float maxX = ImGui::GetContentRegionAvail().x;
        for (size_t i = 0; i < acts.size(); ++i)
        {
            const auto& [a, st] = acts[i];
            const float w = ImGui::CalcTextSize(a.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
            if (i && x + w < maxX) ImGui::SameLine();
            else x = 0;
            x += w + ImGui::GetStyle().ItemSpacing.x;
            ImGui::PushID(idBase + int(i));
            const bool self = st == cur, death = moves.IsDeathLike(st);
            if (self) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            else if (death) ImGui::PushStyleColor(ImGuiCol_Text, kErr);
            if (ui::SmallButtonRaw(a.c_str())) play = moves.states[size_t(st)].name; // data: the action name
            if (self || death) ImGui::PopStyleColor();
            if (ui::Hovered())
                ui::SetTooltip(death ? "%s -> %s (death / unconscious: only by this click)" : "%s -> %s (playMove: the cheapest path from the current state)",
                               a.c_str(), moves.states[size_t(st)].name.c_str());
            ImGui::PopID();
        }
    };
    ui::TextDisabled("actions of %s (%zu):", moves.states[size_t(cur)].actions.c_str(), main.size() + other.size());
    buttons(main, 0);
    if (!other.empty() && ImGui::TreeNode(TF("Other actions ({})##otheracts", other.size()).c_str()))
    {
        buttons(other, 10000);
        ImGui::TreePop();
    }
    if (!play.empty()) view_.MovesPlay(play, false, true); // after drawing: the buttons belong to the state they were drawn for
}

void MovesPanel::DrawLog()
{
    auto& log = sim_.log;
    ui::SeparatorText(TF("4. Transition log ({})", log.size()).c_str());
    if (ui::SmallButton("Clear log")) log.clear();
    if (log.empty())
    {
        ImGui::SameLine();
        ui::TextDisabled("empty - every playMove / switchMove / key action lands here with its path");
        return;
    }
    std::string from, action;
    if (ImGui::BeginChild("##movelog", ImVec2(0, 120 * host_.Dpi()), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY))
        for (int i = int(log.size()) - 1; i >= 0; --i)
        {
            const auto& e = log[size_t(i)];
            ImGui::PushID(i);
            if (ui::SelectableRaw(std::format("{:7.2f} s  {:<14} {}  [{}]  blend {:.2f} s", e.time, e.action, e.path, e.kinds, e.blend).c_str()) &&
                s_.moves.Find(e.from) >= 0)
            {
                from = e.from;
                action = e.action;
            }
            if (ui::Hovered()) ui::SetTooltip("Click: switchMove to %s and run %s again", e.from.c_str(), e.action.c_str());
            ImGui::PopID();
        }
    ImGui::EndChild();
    if (!from.empty()) // replay from where it started (after drawing: it appends to the log)
    {
        view_.MovesPlay(from, true, true);
        view_.MovesPlay(action, false, true);
    }
}

// gestures over the moves, the layer stack with masks, the aiming approximation, IK curves
void MovesPanel::DrawGestures()
{
    const MovesGraph& moves = s_.moves;
    const MovesGraph& gestures = s_.gestures;
    if (!ui::CollapsingHeader(TF("Gestures and layers ({}: {} gestures, {} masks)###gestures", moves.gestures.empty() ? "-" : moves.gestures,
                                 gestures.states.size(), s_.masks.size()).c_str()))
        return;
    const float dpi = host_.Dpi();
    ui::TextUnformatted("Gesture / handAnim blending:");
    ImGui::SameLine();
    ui::RadioButton("Model space##bl", &s_.blendMode, 0);
    ImGui::SameLine();
    ui::RadioButton("Parent-relative##bl", &s_.blendMode, 1);
    ImGui::SameLine();
    ui::TextDisabled("(fade: 1 / interpolationSpeed - approximation)");
    ImGui::SetNextItemWidth(240 * dpi);
    const bool enter = ui::InputTextWithHint("##gesture", "gesture (e.g. GestureNod, GestureFreeze)", gestureCmd_, sizeof(gestureCmd_),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
    if (gestureCmd_[0] && ImGui::IsItemActive())
    {
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y));
        ImGui::BeginTooltip();
        int n = 0;
        for (const auto& g : gestures.states)
            if (!g.abstract && n < 15 && ContainsNoCase(g.name, gestureCmd_))
            {
                ui::TextUnformatted(g.name.c_str());
                ++n;
            }
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    if ((ui::Button("playGesture") || enter) && gestureCmd_[0]) view_.PlayGesture(gestureCmd_);
    ImGui::SameLine();
    if (ui::Button("Stop gesture")) gp_.Stop();
    ImGui::SameLine();
    bool heat = view_.MaskHeat();
    if (ui::Checkbox("Mask heat map", &heat)) view_.SetMaskHeat(heat, view_.MaskView());
    if (ui::Hovered()) ui::SetTooltip("Mesh coloured by the mask weight of its bones (blue 0 -> red 1)");
    if (heat)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200 * dpi);
        const std::string& mv = view_.MaskView();
        if (ui::BeginCombo("##maskview", mv.empty() ? "mask" : mv.c_str()))
        {
            std::string pick;
            for (const auto& [k, m] : s_.masks)
                if (ui::SelectableRaw(k.c_str(), k == ToLower(mv))) pick = k;
            ImGui::EndCombo();
            if (!pick.empty()) view_.SetMaskHeat(true, pick);
        }
    }
    // the stack: base state(s) + gesture, weights and masks
    if (ImGui::BeginTable("##stack", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
    {
        ui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthFixed);
        ui::TableSetupColumn("State / RTM");
        ui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed);
        ui::TableSetupColumn("Weight", ImGuiTableColumnFlags_WidthFixed);
        ui::TableSetupColumn("Mask (bones > 0)");
        ImGui::TableHeadersRow();
        int idx = 0;
        for (const auto& l : s_.anim.layers)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ui::Text("%d%s", idx++, l.tag == 1 ? " gesture" : "");
            ImGui::TableNextColumn();
            ui::TextUnformatted(l.anim ? l.anim->Name().c_str() : "-");
            ImGui::TableNextColumn();
            ui::TextUnformatted(l.mode == LayerMode::Override ? "override" : "blend");
            ImGui::TableNextColumn();
            ui::Text("%.2f", l.weight);
            ImGui::TableNextColumn();
            if (l.tag == 1 && gp_.current >= 0)
                ui::Text("%s (%d)", gestures.states[size_t(gp_.current)].mask.c_str(),
                         int(std::count_if(l.boneWeight.begin(), l.boneWeight.end(), [](float w) { return w > 0; })));
            else
                ui::TextDisabled("all bones");
        }
        ImGui::EndTable();
    }
    const AnimLayer* gl = gp_.Layer();
    if (gp_.current >= 0 && ui::TreeNode("Mask weights per bone"))
    {
        if (gl)
            for (size_t b = 0; b < gl->boneWeight.size() && b < s_.skeleton.bones.size(); ++b)
                if (gl->boneWeight[b] > 0) ui::BulletText("%s  %.2f", s_.skeleton.bones[b].name.c_str(), gl->boneWeight[b]);
        ImGui::TreePop();
    }
    // approximations, marked as such
    ui::TextColored(kWarn, "Approximation:");
    ImGui::SameLine();
    ui::Checkbox("aiming / aimingBody", &gp_.aimApprox);
    if (ui::Hovered())
        ui::SetTooltip("Bones of the state's aiming mask pitch, of aimingBody yaw, around the pelvis by their mask weight.\n"
                       "The engine aims with its own weapon / head controllers - this only shows which bones follow and how much.");
    if (gp_.aimApprox)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * dpi);
        if (ui::SliderFloat("yaw", &gp_.aimYaw, -90, 90, "%.0f deg")) host_.PoseChanged();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * dpi);
        if (ui::SliderFloat("pitch", &gp_.aimPitch, -60, 60, "%.0f deg")) host_.PoseChanged();
    }
    auto curves = [&](const MovesGraph& g, int st, float phase, const char* what) {
        if (st < 0) return;
        const MoveState& s = g.states[size_t(st)];
        if (!s.aiming.empty() || !s.aimingBody.empty()) ui::TextDisabled("%s %s: aiming %s, aimingBody %s", what, s.name.c_str(), s.aiming.c_str(), s.aimingBody.c_str());
        for (int hand = 0; hand < 2; ++hand)
        {
            const auto& c = hand ? s.rightHandIKCurve : s.leftHandIKCurve;
            if (c.size() < 2) continue;
            std::vector<float> plot(64);
            for (int i = 0; i < 64; ++i) plot[size_t(i)] = CurveAt(c, i / 63.0f);
            ImGui::PlotLines(std::format("{} {}HandIKCurve, now {:.2f}##{}{}", what, hand ? "right" : "left", CurveAt(c, phase), what, hand).c_str(), plot.data(), 64, 0,
                             nullptr, 0.0f, 1.0f, ImVec2(160 * dpi, 36 * dpi));
        }
    };
    const MovesPlayer& p = sim_.player;
    if (p.Current() >= 0) curves(moves, p.Current(), p.PhaseOf(moves, p.Cur(), pb_.time), "state");
    if (gp_.current >= 0) curves(gestures, gp_.current, gl ? gl->offset : 0.0f, "gesture");
    ui::TextDisabled("IK curves are shown, not applied: there is no weapon to reach for in the viewer.");
}

// key map of the control mode, test scenarios of the own config, traps
void MovesPanel::DrawControl()
{
    if (!ui::CollapsingHeader(T("Control keys, tests, traps###controlsec"))) return;
    const float dpi = host_.Dpi();
    const MovesGraph& moves = s_.moves;
    ui::TextDisabled("keys -> actions of the current state; Q / E turn; the character walks with the RTM step, camera follows");
    if (ImGui::TreeNode(T("Key map")))
    {
        auto& keymap = ctl_.keymap;
        for (size_t i = 0; i < keymap.size(); ++i)
        {
            ImGui::PushID(int(i));
            char k[32], a[64];
            strncpy_s(k, keymap[i].keys.c_str(), _TRUNCATE);
            strncpy_s(a, keymap[i].action.c_str(), _TRUNCATE);
            ImGui::SetNextItemWidth(90 * dpi);
            if (ui::InputText("##keys", k, sizeof(k))) keymap[i].keys = k;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160 * dpi);
            if (ui::InputText("##act", a, sizeof(a))) keymap[i].action = a;
            ImGui::PopID();
        }
        if (ui::SmallButton("+ binding")) keymap.push_back({ "V", "Stop" });
        ImGui::SameLine();
        if (ui::SmallButton("Defaults")) ctl_.DefaultKeymap();
        ImGui::TreePop();
    }
    // test scenarios
    fs::path own;
    if (ed_.OwnFile(s_.CrewConfig(), own))
    {
        const fs::path tests = own.parent_path() / "armaview_moves_tests.txt";
        ui::SeparatorText("Test scenarios");
        ui::TextDisabled("%s", PathToUtf8(tests).c_str());
        if (ui::SmallButton("Run tests"))
        {
            ed_.scenarios.clear();
            for (const auto& sc : LoadScenarios(tests)) ed_.scenarios.push_back(RunScenario(moves, sc));
            if (ed_.scenarios.empty())
                ed_.msg = "no tests in " + PathToUtf8(tests.filename()) + " (\"test name\", \"start State\", \"do Action [s]\", \"expect State\")";
        }
        ImGui::SameLine();
        if (ui::SmallButton("Open file"))
        {
            std::error_code ec;
            if (!fs::exists(tests, ec))
                std::ofstream(tests) << "# ArmaView move tests: test <name> / start <state> / do <action or state> [seconds] / expect <state>\n";
            host_.OpenInEditor(tests, 1);
        }
        for (const auto& r : ed_.scenarios)
        {
            ui::TextColored(r.ok ? kGood : kErr, "%s %s -> %s", r.ok ? "PASS" : "FAIL", r.name.c_str(), r.reached.c_str());
            if (ui::Hovered()) ui::SetTooltip("%s", r.log.c_str());
        }
    }
    // traps
    ui::SeparatorText(TF("Traps ({})", ed_.traps.size()).c_str());
    ui::Checkbox("your states only", &ed_.trapsOwnOnly);
    ImGui::SameLine();
    ui::Checkbox("pose jumps on connectTo (reads RTMs)", &ed_.trapsPose);
    ImGui::SameLine();
    if (ui::SmallButton("Find traps"))
    {
        const Skeleton& sk = s_.skeleton;
        const Skeleton* skel = !sk.Empty() && IEquals(sk.name, moves.skeletonName) ? &sk : nullptr;
        ed_.traps = FindMoveTraps(moves, ed_.trapsPose, skel, ed_.trapsOwnOnly);
        Log::Get().Info("CfgMoves traps: {}", ed_.traps.size());
    }
    for (size_t i = 0; i < ed_.traps.size() && i < 300; ++i)
    {
        const MoveTrap& t = ed_.traps[i];
        ImGui::PushID(int(i));
        ui::TextColored(t.error ? kErr : kWarn, "%s", t.text.c_str());
        if (t.state >= 0)
        {
            ImGui::SameLine();
            if (ui::SmallButton("show")) view_.ShowState(t.state);
        }
        ImGui::PopID();
    }
}

void MovesPanel::DrawValidation()
{
    const MovesGraph& moves = s_.moves;
    if (!ui::CollapsingHeader(TF("Validation of {}", moves.cls).c_str())) return;
    const float dpi = host_.Dpi();
    ld_.PollValidation();
    ImGui::BeginDisabled(ld_.Validating());
    if (ui::Button(ld_.Validating() ? "Checking..." : "Check states")) ld_.StartValidation(false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ui::Checkbox("loop seams (reads every looped RTM)", &ld_.seams);
    const MovesValidation& v = ld_.validation;
    if (!v.done)
    {
        ui::TextDisabled("unreachable states, missing links / RTMs, RTM vs skeleton, loop seams, actions");
        return;
    }
    const bool clean = v.badLinks + v.missingRtm + v.wrongSkeleton == 0;
    ui::TextColored(clean ? kGood : kErr, "%s", v.summary.c_str());
    ImGui::SetNextItemWidth(200 * dpi);
    ui::InputTextWithHint("##valfilter", "filter", valFilter_, sizeof(valFilter_));
    if (ImGui::BeginChild("##movesval", ImVec2(0, 220 * dpi), ImGuiChildFlags_Borders))
    {
        int shown = 0;
        for (const auto& it : v.items)
        {
            if (!ContainsNoCase(it.text, valFilter_)) continue;
            if (++shown > 2000)
            {
                ui::TextDisabled("...");
                break;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, it.error ? kErr : it.info ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : kWarn);
            ImGui::PushID(shown);
            if (ui::SelectableRaw(it.text.c_str()) && it.state >= 0) view_.ShowState(it.state);
            ImGui::PopID();
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
}
