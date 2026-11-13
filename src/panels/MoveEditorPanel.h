#pragma once
// Move editor window: the state selected in the States graph (or the playing one) - its values, RTM, links with costs,
// rename / delete / copy a BI state into the own config, new states, the actions classes (drag a state onto an
// action), the diff before saving. BI states are read only. The edits are core/MovesEditor on the Session's ConfigDoc.
#include "MovesEditor.h"
#include "MovesLoader.h"
#include "MovesPanel.h"
#include "MovesSim.h"
#include "Playback.h"
#include "Session.h"
#include "StatesPanel.h"
#include "ViewHost.h"

#include <filesystem>
#include <string>
#include <vector>

class MoveEditorPanel
{
public:
    MoveEditorPanel(Session& session, MovesEditor& editor, MovesLoader& loader, MovesSim& sim, Playback& playback, StatesPanel& states,
                    IViewHost& host, IMovesView& view)
        : s_(session), ed_(editor), ld_(loader), sim_(sim), pb_(playback), states_(states), host_(host), view_(view) {}
    void Draw(const std::string& windowName, const std::string& dockWith);
    bool showDiff = false;

private:
    void DrawFileLine();
    void DrawState(int sel);
    void DrawRtmPicker(int sel);
    void DrawLinks(int sel, bool own);
    void DrawActions();
    void DrawDiff();

    Session& s_;
    MovesEditor& ed_;
    MovesLoader& ld_;
    MovesSim& sim_;
    Playback& pb_;
    StatesPanel& states_;
    IViewHost& host_;
    IMovesView& view_;
    char newName_[64] = {}, newBase_[64] = {}, rename_[64] = {};
    std::vector<std::filesystem::path> rtmChoices_;
};
