#pragma once
// Input of the viewer each frame: the mouse in the scene (orbit, pan, zoom, click picks a point, Ctrl+click picks a
// face) and the hotkeys. Every hotkey is a command of the CommandRegistry ("key.<group>.<what>": also reachable from
// the ui-script and the palette); a key press runs the command. Keys are physical (platform/Input maps the scan code to
// the US key of that place), nothing fires while a text field has the keyboard.
#include "Camera.h"
#include "CommandPalette.h"
#include "CommandStack.h"
#include "Commands.h"
#include "InspectorPanel.h"
#include "MovesPanel.h"
#include "MovesSim.h"
#include "PointOverlay.h"
#include "SceneView.h"
#include "Session.h"
#include "ViewHost.h"

#include <functional>
#include <string>
#include <vector>

class SceneInput
{
public:
    SceneInput(Session& session, SceneView& scene, OrbitCamera& camera, PointOverlay& points, InspectorPanel& inspector, CommandPalette& palette,
               bool& helpOpen, const MovesControl& control, CommandRegistry& commands, std::vector<Hotkey>& hotkeys, const IViewport& viewport)
        : s_(session), scene_(scene), camera_(camera), points_(points), inspector_(inspector), palette_(palette), helpOpen_(helpOpen), control_(control),
          commands_(commands), hotkeys_(hotkeys), viewport_(viewport) {}

    // a key (ImGuiKey_None: palette only) for a command registered under "key.<group>.<what>"
    void Bind(int chord, const char* group, const char* description, std::function<void()> run, bool needsModel = false);
    void Info(const char* keys, const char* group, const char* description); // mouse / window-local keys, listed by Help
    void PublishKeys();  // tooltips of controls named like a command show its key
    void Frame();
    void FocusBone(int bone);                                  // the camera frames what the bone moves
    void SelectBone(int bone, bool focus, bool materialsMode); // again / -1: deselect
    void SetView(float yaw, float pitch);
    bool Control(float dt, MovesControl& control, MovesSim& sim, Playback& playback, IMovesView& moves); // true: pose changed

private:
    Session& s_;
    SceneView& scene_;
    OrbitCamera& camera_;
    PointOverlay& points_;
    InspectorPanel& inspector_;
    CommandPalette& palette_;
    bool& helpOpen_;
    const MovesControl& control_;
    CommandRegistry& commands_;
    std::vector<Hotkey>& hotkeys_;
    const IViewport& viewport_;
    int dragButton_ = -1;
    float pressX_ = 0, pressY_ = 0; // click detection (press / release without drag)
};
