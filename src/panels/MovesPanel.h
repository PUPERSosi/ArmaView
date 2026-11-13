#pragma once
// Moves window: CfgMoves of the model as in the game - which move config, the start state, game / control mode, the
// current state, its actions as buttons, the transition log, gestures with their masks, the key map, test scenarios,
// traps and the validation of the config. The logic is core (MovesLoader, MovesSim, MovesControl, GesturePlayer,
// MovesEditor); this window shows it and sends commands.
#include "MovesEditor.h"
#include "MovesLoader.h"
#include "MovesSim.h"
#include "Playback.h"
#include "Session.h"
#include "ViewHost.h"

#include <string>

// What the Moves / States / Move editor windows ask of the viewer (implemented by App): the commands that change what
// plays also move the camera, the grid, the footsteps and the States graph.
class IMovesView
{
public:
    virtual ~IMovesView() = default;
    virtual void SetMovesDrive(bool on) = 0;                                          // game mode on / off
    virtual void MovesPlay(const std::string& stateOrAction, bool switchMove, bool user) = 0; // playMove / switchMove
    virtual void MovesReset() = 0;                                                    // start state at the origin
    virtual void SetControlMode(bool on) = 0;                                         // keys drive the character
    virtual void PlayGesture(const std::string& name) = 0;
    virtual void SetMaskHeat(bool on, const std::string& mask) = 0;                   // the mesh coloured by a mask
    virtual bool MaskHeat() const = 0;
    virtual const std::string& MaskView() const = 0;
    virtual void ShowState(int state) = 0;                                            // the States graph opened on a state
    virtual void ToggleStatesGraph() = 0;
    virtual bool GameConfigsLoading() const = 0;
};

class MovesPanel
{
public:
    MovesPanel(Session& session, MovesLoader& loader, MovesSim& sim, MovesControl& control, GesturePlayer& gestures, MovesEditor& editor,
               Playback& playback, IViewHost& host, IMovesView& view)
        : s_(session), ld_(loader), sim_(sim), ctl_(control), gp_(gestures), ed_(editor), pb_(playback), host_(host), view_(view) {}
    void Draw(const std::string& windowName, const std::string& dockWith);
    void DrawValidation(); // also in the Validation window

private:
    bool DrawHeader();       // false: nothing more to show
    void DrawStartState();
    void DrawCurrentState();
    void DrawActions();
    void DrawLog();
    void DrawGestures();
    void DrawControl();

    Session& s_;
    MovesLoader& ld_;
    MovesSim& sim_;
    MovesControl& ctl_;
    GesturePlayer& gp_;
    MovesEditor& ed_;
    Playback& pb_;
    IViewHost& host_;
    IMovesView& view_;
    char cmd_[96] = {}, gestureCmd_[96] = {}, valFilter_[64] = {};
};
