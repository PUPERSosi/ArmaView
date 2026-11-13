#pragma once
// CfgMoves states window: the whole graph of the move config (StatesGraph canvas) - select, playMove / switchMove from
// it, Alt+drag adds a link in the own config.
#include "MovesEditor.h"
#include "MovesLoader.h"
#include "MovesPanel.h"
#include "MovesSim.h"
#include "Session.h"
#include "StatesGraph.h"
#include "ViewHost.h"

#include <string>

class StatesPanel
{
public:
    StatesPanel(Session& session, MovesLoader& loader, MovesSim& sim, MovesEditor& editor, IViewHost& host, IMovesView& view)
        : s_(session), ld_(loader), sim_(sim), ed_(editor), host_(host), view_(view) {}
    void Draw();
    void Show(int state); // opened, focused on the state
    StatesGraph graph;    // the canvas (positions per move config next to the exe)
    bool open = false;

private:
    Session& s_;
    MovesLoader& ld_;
    MovesSim& sim_;
    MovesEditor& ed_;
    IViewHost& host_;
    IMovesView& view_;
};
