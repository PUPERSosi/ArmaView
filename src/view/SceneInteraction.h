#pragma once
// What the scene does with the input, apart from reading it: orbit / pan / zoom of the camera, a click picks a point
// (Ctrl+click a face), bone selection and the camera framing a bone, the CfgMoves control mode step (key actions, Q / E
// turn, root motion, the camera follows the character). Input comes as an InputState (view/SceneInput fills it from
// ImGui; another UI from its own events); what it changes in the viewer goes through ISceneInteractionHost. No ImGui.
#include "Camera.h"
#include "InputState.h"
#include "Moves.h"
#include "MovesSim.h"
#include "Playback.h"

#include <glm/glm.hpp>

#include <string>

// What the inspector panel shows (only what the scene interaction sets or clears).
enum class InspectTarget { None, Point, Bone, Other };

class ISceneInteractionHost
{
public:
    virtual ~ISceneInteractionHost() = default;
    virtual bool HasModel() const = 0;
    virtual bool ClickPoint(float x, float y) = 0;                          // the point overlay at window pixels: true if one was picked
    virtual void ToggleFace(const glm::mat4& viewProj, float nx, float ny) = 0; // Ctrl+click: the face under (nx, ny) in / out of the pick
    virtual InspectTarget Inspected() const = 0;
    virtual void Inspect(InspectTarget what) = 0;
    virtual int BoneCount() const = 0;
    virtual int SelectedBone() const = 0;
    virtual void SetSelectedBone(int bone) = 0;                             // -1 none; highlight + geometry follow
    virtual void BoneExtent(int bone, glm::vec3& mn, glm::vec3& mx) const = 0; // what the bone moves (posed) and its pivot
    virtual void GridFollow(const glm::vec2& cell) = 0;                     // the floor grid around the character
    virtual void MovesPlay(const std::string& action) = 0;                  // a control key's action (never into death states)
    virtual void SetMovesDrive(bool on) = 0;                                // CfgMoves game mode
};

struct SceneRect { float x = 0, y = 0, w = 1, h = 1; }; // the 3D view inside the window (pixels from the origin)

class SceneInteraction
{
public:
    SceneInteraction(OrbitCamera& camera, ISceneInteractionHost& host) : camera_(camera), host_(host) {}
    // orbit (left drag), pan (right / middle drag), zoom (wheel over the scene); left click without a drag picks a
    // point, with Ctrl a face
    void Mouse(const InputState& in, const SceneRect& rect, const glm::mat4& viewProj);
    // one step of the control mode: keys -> actions of the current state, Q / E turn, root motion, the camera and the
    // grid follow. False when the mode is off (or a text field has the keyboard); true: the pose follows
    bool Control(const InputState& in, float dt, MovesControl& control, MovesSim& sim, const Playback& playback, const MovesGraph& moves);
    void FocusBone(int bone);                                  // the camera frames what the bone moves
    void SelectBone(int bone, bool focus, bool materialsMode); // again / -1: deselect
    void SetView(float yaw, float pitch);

private:
    OrbitCamera& camera_;
    ISceneInteractionHost& host_;
    int dragButton_ = -1;
    float pressX_ = 0, pressY_ = 0; // click detection (press / release without drag)
};
