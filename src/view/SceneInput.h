#pragma once
// ImGui adapter of the scene input: each frame it reads ImGui's IO into an InputState and lets SceneInteraction (no ImGui)
// do the orbit / pan / zoom, picking, bone selection and the control mode; it also runs the hotkeys. Every hotkey is a
// command of the CommandRegistry ("key.<group>.<what>": also reachable from the ui-script and the palette); a key press
// runs the command. Keys are physical (platform/Input maps the scan code to the US key of that place), nothing fires
// while a text field has the keyboard. Another UI writes its own adapter that fills InputState from its events.
#include "Camera.h"
#include "CommandPalette.h"
#include "CommandStack.h"
#include "Commands.h"
#include "InputState.h"
#include "InspectorPanel.h"
#include "MovesPanel.h"
#include "MovesSim.h"
#include "PointOverlay.h"
#include "SceneInteraction.h"
#include "SceneView.h"
#include "Session.h"
#include "ViewHost.h"

#include <functional>
#include <string>
#include <vector>

class SceneInput : private ISceneInteractionHost
{
public:
    SceneInput(Session& session, SceneView& scene, OrbitCamera& camera, PointOverlay& points, InspectorPanel& inspector, CommandPalette& palette,
               bool& helpOpen, const MovesControl& control, CommandRegistry& commands, std::vector<Hotkey>& hotkeys, const IViewport& viewport)
        : s_(session), scene_(scene), camera_(camera), points_(points), inspector_(inspector), palette_(palette), helpOpen_(helpOpen), control_(control),
          commands_(commands), hotkeys_(hotkeys), viewport_(viewport), interaction_(camera, *this) {}

    // a key (ImGuiKey_None: palette only) for a command registered under "key.<group>.<what>"
    void Bind(int chord, const char* group, const char* description, std::function<void()> run, bool needsModel = false);
    void Info(const char* keys, const char* group, const char* description); // mouse / window-local keys, listed by Help
    void PublishKeys();  // tooltips of controls named like a command show its key
    void Frame();
    void FocusBone(int bone) { interaction_.FocusBone(bone); }  // the camera frames what the bone moves
    void SelectBone(int bone, bool focus, bool materialsMode) { interaction_.SelectBone(bone, focus, materialsMode); } // again / -1: deselect
    void SetView(float yaw, float pitch) { interaction_.SetView(yaw, pitch); }
    bool Control(float dt, MovesControl& control, MovesSim& sim, Playback& playback, IMovesView& moves); // true: pose changed
    static InputState ReadInput(const IViewport& viewport); // ImGui's IO of this frame (ui-script / raw keys arrive through it)

private:
    // ISceneInteractionHost: what the interaction changes in the viewer
    bool HasModel() const override { return s_.hasModel; }
    bool ClickPoint(float x, float y) override { return points_.Click(x, y, s_.model, s_.lod); }
    void ToggleFace(const glm::mat4& viewProj, float nx, float ny) override;
    InspectTarget Inspected() const override;
    void Inspect(InspectTarget what) override;
    int BoneCount() const override { return int(s_.skeleton.bones.size()); }
    int SelectedBone() const override { return scene_.selectedBone; }
    void SetSelectedBone(int bone) override;
    void BoneExtent(int bone, glm::vec3& mn, glm::vec3& mx) const override;
    void GridFollow(const glm::vec2& cell) override;
    void MovesPlay(const std::string& action) override { if (moves_) moves_->MovesPlay(action, false, false); }
    void SetMovesDrive(bool on) override { if (moves_) moves_->SetMovesDrive(on); }

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
    IMovesView* moves_ = nullptr; // set for the control step
    SceneInteraction interaction_;
};
