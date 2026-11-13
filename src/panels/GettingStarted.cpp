#include "GettingStarted.h"
#include "Lang.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <vector>

// ============================================================================================ Getting started

namespace {

struct GuideStep
{
    const char* text;   // what to do (string table)
    const char* target; // control to highlight: its label / id as the ui:: wrappers see it ("" = none)
};
struct Guide
{
    const char* title;
    const char* about;
    int mode;           // App::UiMode
    const char* panel;  // panel to bring to front ("" = none)
    std::vector<GuideStep> steps;
};

const std::vector<Guide>& Guides()
{
    static const std::vector<Guide> g = {
        { "Check a model", "LODs, selections, textures and the rig of a P3D (MLOD) - without the game.", 0, "Outliner",
          { { "Open a model: the folder button of the toolbar or Ctrl+O (or drop a .p3d onto the window).", "Open model / RTM (Ctrl+O)" },
            { "Pick a LOD in the list (PgUp / PgDn). The Outliner lists its selections: click one to see its points.", "lod" },
            { "Turn on the skeleton (B) and the wireframe (W) to check the rig.", "Show skeleton (B)" },
            { "Open Validation (Ctrl+5): rig, selections, model.cfg animations and textures - every problem with its place.", "Validation" } } },
        { "Check an RTM animation", "An .rtm on the model's skeleton: bones, loop, speed.", 1, "Timeline",
          { { "Open the character model (Ctrl+O): the skeleton comes from its model.cfg.", "Open model / RTM (Ctrl+O)" },
            { "Timeline: Open RTM... adds an animation layer (several layers blend).", "Open RTM..." },
            { "Play / pause (Space), step frames with Left / Right.", "Play / pause (Space)" },
            { "Validation lists RTM bones the skeleton does not have and the loop seam.", "Validation" } } },
        { "Check CfgMoves (control)", "The character's move config as in the game: start state, keys, transitions.", 1, "Moves",
          { { "Open the character model (Ctrl+O): its CfgMoves is found by the skeleton.", "Open model / RTM (Ctrl+O)" },
            { "Moves panel: check the start state (auto = as the engine: the Default action of the actions class).", "Start state" },
            { "Tick Control mode: W A S D walk, Shift run, C crouch / stand, Z prone / stand, X stand up, Q / E turn.", "Control mode" },
            { "Reset: back to the start state at the origin.", "Reset##moves" },
            { "The transition log shows every step with its path; click a line to replay it.", "Clear log" } } },
        { "Weapon reload", "The reload gesture against the weapon's model.cfg (magazine, bolt) in weapon space.", 5, "Reload sync",
          { { "Open the weapon P3D (Ctrl+O): its CfgWeapons class, reloadAction and handAnim are found.", "Open model / RTM (Ctrl+O)" },
            { "Play the reload, or scrub frame by frame.", "Play##reload" },
            { "Pick the hand bone and the weapon part: Distance over time shows where they meet.", "hand bone" },
            { "Suggest window proposes the model.cfg window of the animation; Diff... shows the change before saving.", "Suggest window" } } },
        { "Optic fitting", "Attachments on the slots and the picture through the sight.", 6, "Optics",
          { { "Open the weapon P3D (Ctrl+O): slots come from WeaponSlotsInfo.", "Open model / RTM (Ctrl+O)" },
            { "Pick an item for a slot: it sits on the slot's proxy.", "item" },
            { "View through the sight: the camera at memoryPointCamera with opticsZoom.", "View through the sight" },
            { "Character aiming: the character holds the weapon - check the eye against the sight.", "Character aiming" } } },
        { "Vehicle and crew", "Crew seats, proxies, get-in points and turned-out poses of a vehicle.", 2, "Crew",
          { { "Open the vehicle P3D (Ctrl+O).", "Open model / RTM (Ctrl+O)" },
            { "Show crew: the crew model sits on every seat proxy with its CfgMoves pose.", "Show crew" },
            { "Play all seats, or pick a seat in the list for its details in the Inspector.", "Play all" },
            { "Set crew model: your own character instead of BI's sample man.", "Set crew model" } } },
        { "Retarget", "An RTM of another skeleton on this model, exported to a new RTM_0101.", 1, "Retarget",
          { { "Open the target character model (Ctrl+O).", "Open model / RTM (Ctrl+O)" },
            { "Load the source rig (a character of the source skeleton; BI's sample man by default).", "Load source rig" },
            { "Type an .rtm or a CfgMovesMaleSdr state name, then Retarget + preview.", "Retarget + preview" },
            { "Export RTM_0101 + verify writes a NEW file and compares it with the preview.", "Export RTM_0101 + verify" } } },
    };
    return g;
}

} // namespace

void GettingStarted::Go(int scenario, int step)
{
    const auto& gs = Guides();
    if (scenario < 0 || scenario >= int(gs.size())) { guide_ = -1; ui::SetHighlight(""); return; }
    const Guide& g = gs[size_t(scenario)];
    guide_ = scenario;
    step_ = std::clamp(step, 0, int(g.steps.size()) - 1);
    host_.ShowMode(g.mode, g.panel);
    ui::SetHighlight(g.steps[size_t(step_)].target);
    open = true;
}

void GettingStarted::Draw(float dpi)
{
    if (!open) { if (guide_ >= 0) { guide_ = -1; ui::SetHighlight(""); } return; }
    // never over the highlighted control: when it is under this window, the window moves to the other side of the screen
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(700 * dpi, 330 * dpi), ImGuiCond_FirstUseEver);
    if (ImGuiWindow* self = ImGui::FindWindowByName("###Getting started"); self && !ui::Highlight().empty())
    {
        float tx = 0, ty = 0;
        const ImRect r(ImVec2(self->Pos.x - 8, self->Pos.y - 8), ImVec2(self->Pos.x + self->Size.x + 8, self->Pos.y + self->Size.y + 8));
        if (ui::ControlCenter(ui::Highlight(), tx, ty) && r.Contains(ImVec2(tx, ty)))
        {
            const bool left = tx > vp->WorkPos.x + vp->WorkSize.x * 0.5f;
            const bool top = ty > vp->WorkPos.y + vp->WorkSize.y * 0.5f;
            ImGui::SetNextWindowPos(ImVec2(left ? vp->WorkPos.x + 20 : vp->WorkPos.x + vp->WorkSize.x - 20, top ? vp->WorkPos.y + 90 * dpi : vp->WorkPos.y + vp->WorkSize.y - 30),
                                    ImGuiCond_Always, ImVec2(left ? 0.0f : 1.0f, top ? 0.0f : 1.0f));
        }
    }
    if (!ui::Begin("Getting started", &open))
    {
        ImGui::End();
        return;
    }
    const auto& gs = Guides();
    ui::TextDisabled("Pick a scenario: the viewer opens its mode and highlights the control of the next step (yellow frame).");
    if (ImGui::BeginTable("##guides", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
    {
        ui::TableSetupColumn("##list", ImGuiTableColumnFlags_WidthFixed, 230 * dpi);
        ui::TableSetupColumn("##steps");
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        for (int i = 0; i < int(gs.size()); ++i)
        {
            ImGui::PushID(i);
            const bool active = i == guide_;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ui::Button(gs[size_t(i)].title, ImVec2(-FLT_MIN, 0))) Go(i, 0);
            if (active) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::TableNextColumn();
        if (guide_ < 0)
            ui::EmptyState("Choose a scenario on the left. Each one switches to its mode, brings the right panel to the front and walks you "
                           "through it step by step. Help > Getting started opens this window again; F1 lists every key.");
        else
        {
            const Guide& g = gs[size_t(guide_)];
            ImGui::TextUnformatted(T(g.title));
            ui::TextDisabled(g.about);
            ImGui::Separator();
            for (int s = 0; s < int(g.steps.size()); ++s)
            {
                const bool cur = s == step_, done = s < step_;
                ImGui::PushID(s);
                const ImVec4 c = cur ? ImVec4(1.0f, 0.8f, 0.2f, 1) : done ? ImVec4(0.45f, 0.85f, 0.45f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                ImGui::TextColored(c, "%s %d.", done ? "v" : cur ? ">" : " ", s + 1);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, c);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(T(g.steps[size_t(s)].text));
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                if (ImGui::IsItemClicked()) Go(guide_, s);
                ImGui::PopID();
            }
            ImGui::Separator();
            ImGui::BeginDisabled(step_ == 0);
            if (ui::Button("< Back")) Go(guide_, step_ - 1);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (step_ + 1 < int(g.steps.size()))
            {
                if (ui::Button("Next step >")) Go(guide_, step_ + 1);
            }
            else if (ui::Button("Done")) { guide_ = -1; ui::SetHighlight(""); }
            ImGui::SameLine();
            if (ui::Button("Show me")) Go(guide_, step_);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
