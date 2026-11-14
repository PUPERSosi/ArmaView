#include "StatesPanel.h"
#include "Lang.h"
#include "Perf.h"

#include <imgui.h>

void StatesPanel::SetGraph(const MovesGraph* g, const std::filesystem::path& positionsFile)
{
    static const MovesGraph kNone;
    graph.SetGraph(g ? *g : kNone, positionsFile);
}

void StatesPanel::ShowState(int state)
{
    graph.FocusOn(state);
    open = true;
}

void StatesPanel::Draw()
{
    if (open) ld_.Ensure(false);
    if (!open) return;
    Perf::Scope perfScope("ui:   CfgMoves states graph");
    const float dpi = host_.Dpi();
    ImGui::SetNextWindowSize(ImVec2(1200 * dpi, 720 * dpi), ImGuiCond_FirstUseEver);
    if (!ui::Begin("CfgMoves states", &open))
    {
        ImGui::End();
        return;
    }
    ui::HelpMarker("CfgMoves states graph.\n"
                   "1. Each node is a state; edges are ConnectTo (wait for the cycle end) and InterpolateTo (blend now) with their costs.\n"
                   "2. Click selects a state; double click runs playMove to it (the path is highlighted).\n"
                   "3. Right click: playMove (path) / switchMove (jump) / Select; Alt+drag from a state to another adds a link (own configs only).\n"
                   "4. Wheel zooms, right / middle drag pans, left drag moves a node.");
    ImGui::SameLine();
    const MovesGraph& moves = s_.moves;
    if (moves.Empty())
    {
        ui::EmptyState("No CfgMoves for this model: open a character model whose skeleton has a move config (see the Moves panel).");
        ImGui::End();
        return;
    }
    const StatesGraph::Actions act = graph.Draw(moves, sim_.player.Current(), sim_.player.PathShown, dpi);
    if (act.play >= 0) view_.MovesPlay(moves.states[size_t(act.play)].name, false, true);
    if (act.switchTo >= 0) view_.MovesPlay(moves.states[size_t(act.switchTo)].name, true, true);
    if (act.linkFrom >= 0) ed_.Link(s_.CrewConfig(), act.linkFrom, act.linkTo, act.linkInterpolate);
    ImGui::End();
}
