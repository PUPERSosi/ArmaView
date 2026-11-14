#include "SceneInteraction.h"

#include <algorithm>
#include <cmath>

#include "ViewCoreGuard.h"

void SceneInteraction::Mouse(const InputState& in, const SceneRect& r, const glm::mat4& viewProj)
{
    const float mx = in.mouseX - in.originX, my = in.mouseY - in.originY;
    if (dragButton_ < 0 && in.overScene)
        for (int b = 0; b < 3; ++b)
            if (in.pressed[size_t(b)])
            {
                dragButton_ = b;
                pressX_ = in.mouseX;
                pressY_ = in.mouseY;
                break;
            }
    if (dragButton_ >= 0)
    {
        if (!in.down[size_t(dragButton_)])
        {
            // left click without dragging: pick a point, or with Ctrl a face
            if (dragButton_ == InputState::Left && host_.HasModel() && std::hypot(in.mouseX - pressX_, in.mouseY - pressY_) < 4.0f)
            {
                if (in.ctrl)
                {
                    const float nx = (mx - r.x) / r.w * 2.0f - 1.0f, ny = 1.0f - (my - r.y) / r.h * 2.0f;
                    host_.ToggleFace(viewProj, nx, ny);
                }
                else if (host_.ClickPoint(in.mouseX, in.mouseY))
                    host_.Inspect(InspectTarget::Point);
            }
            dragButton_ = -1;
        }
        else if (dragButton_ == InputState::Left)
            camera_.Rotate(in.mouseDX, in.mouseDY);
        else
            camera_.Pan(in.mouseDX, in.mouseDY);
    }
    if (in.overScene && in.wheel != 0.0f) camera_.Zoom(in.wheel);
}

bool SceneInteraction::Control(const InputState& in, float dt, MovesControl& control, MovesSim& sim, const Playback& playback, const MovesGraph& moves)
{
    if (!control.on || moves.Empty()) return false;
    if (!sim.drive) host_.SetMovesDrive(true);
    if (in.textInput) return false;
    MovesControl::KeyState keys;
    keys.shift = in.shift;
    keys.ctrl = in.ctrl;
    for (int k = 0; k < 26; ++k) keys.letter[size_t(k)] = in.letters[size_t(k)];
    if (const std::string act = control.KeyAction(keys, moves, sim.player.Current()); !act.empty()) host_.MovesPlay(act);
    control.Move(sim, moves, playback.time, dt, playback.speed * (playback.playing ? 1.0f : 0.0f), in.Letter('Q'), in.Letter('E'));
    // camera follows, grid around the character
    camera_.target = glm::mix(camera_.target, control.rootPos + glm::vec3(0, 1.0f, 0), std::min(1.0f, dt * 4.0f));
    host_.GridFollow(glm::vec2(std::round(control.rootPos.x), std::round(control.rootPos.z)));
    return true; // the pose follows
}

void SceneInteraction::FocusBone(int bone)
{
    if (bone < 0 || bone >= host_.BoneCount()) return;
    glm::vec3 mn(1e30f), mx(-1e30f);
    host_.BoneExtent(bone, mn, mx);
    const glm::vec3 pad(0.12f);
    camera_.Frame(mn - pad, mx + pad);
}

void SceneInteraction::SelectBone(int bone, bool focus, bool materialsMode)
{
    if (focus && bone >= 0)
    {
        host_.SetSelectedBone(-1); // always (re)select when focusing
        FocusBone(bone);
    }
    if (bone == host_.SelectedBone() || bone < 0)
    {
        host_.SetSelectedBone(-1);
        if (host_.Inspected() == InspectTarget::Bone) host_.Inspect(InspectTarget::None);
    }
    else
    {
        if (!materialsMode) host_.Inspect(InspectTarget::Bone);
        host_.SetSelectedBone(bone);
    }
}

void SceneInteraction::SetView(float yaw, float pitch)
{
    camera_.yaw = yaw;
    camera_.pitch = pitch;
}
