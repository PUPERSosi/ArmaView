#include "AnimationView.h"
#include "Lang.h"
#include "Log.h"
#include "RunProgram.h"
#include "Text.h"

#include <algorithm>
#include <format>

namespace fs = std::filesystem;

bool AnimationView::LayersPreview()
{
    if (!layers_.Bake()) return false;
    SetMovesDrive(false);
    if (rt_.previewOn) RetargetStopPreview();
    layers_.StartPreview();
    if (layers_.previewAB)
    {
        playback_.compare.layers.clear();
        AnimLayer b;
        b.anim = layers_.base;
        b.duration = layers_.proj.duration;
        playback_.compare.layers.push_back(b);
        playback_.compare.Bind(session_.skeleton);
        playback_.compareMode = 2;
    }
    else
        playback_.compareMode = 0;
    playback_.playing = true;
    host_.PoseChanged();
    return true;
}

void AnimationView::LayersStopPreview()
{
    if (!layers_.previewOn) return;
    layers_.StopPreview();
    playback_.compareMode = 0;
    host_.PoseChanged();
}

void AnimationView::ShowMaskHeat(std::vector<float> boneWeights)
{
    scene_.customHeat = std::move(boneWeights);
    scene_.maskHeat = true;
    scene_.RebuildLodGeometry();
}

bool AnimationView::PollMovesBuild(bool wait)
{
    if (!movesLoader_.Poll(wait)) return false;
    states_.SetGraph(&session_.moves, paths_.Graphs() / (movesLoader_.BuildClass() + ".states.txt"));
    gesturePlayer_.Stop();
    host_.WatchedFilesChanged();
    return true;
}

bool AnimationView::EnsureMoves(bool wait)
{
    movesLoader_.Ensure(false); // a lazy build starts; applied by PollMovesBuild
    if (wait) PollMovesBuild(true);
    return !session_.moves.Empty();
}

void AnimationView::SetControlMode(bool on)
{
    control_.on = on;
    if (on)
    {
        if (control_.keymap.empty()) control_.DefaultKeymap();
        MovesReset();
    }
    else
    {
        scene_.gridCell = glm::vec2(0);
        scene_.RebuildGrid();
    }
}

void AnimationView::PlayGesture(const std::string& name)
{
    if (!gesturePlayer_.Play(name, playback_.time)) return;
    playback_.playing = true;
    if (scene_.maskHeat) SetMaskHeat(true, session_.gestures.states[size_t(gesturePlayer_.current)].mask);
}

void AnimationView::SetMaskHeat(bool on, const std::string& mask)
{
    scene_.maskHeat = on;
    if (!mask.empty() && mask != scene_.maskView)
    {
        scene_.maskView = mask;
        scene_.customHeat.clear();
    }
    scene_.RebuildLodGeometry();
}

void AnimationView::UpdateRetargetPreview()
{
    if (!rt_.previewOn || !rt_.res.anim || rt_.res.sourceJoints.empty() || rt_.previewMode == 0)
    {
        if (rtLinesShown_) { renderer_.SetLines(LineLayer::Retarget, {}); rtLinesShown_ = false; }
        return;
    }
    auto it = std::find_if(session_.anim.layers.begin(), session_.anim.layers.end(), [](const AnimLayer& l) { return l.tag == 2; });
    if (it == session_.anim.layers.end()) return;
    const float phase = session_.anim.Phase(*it, playback_.time);
    const auto& times = rt_.res.anim->times;
    size_t f = 0;
    while (f + 1 < times.size() && times[f + 1] <= phase) ++f;
    const auto& P = rt_.res.sourceJoints[std::min(f, rt_.res.sourceJoints.size() - 1)];
    // side by side: the source skeleton left of the model
    // beside: to the left on screen, whatever the view
    const glm::mat4 view = camera_.View();
    glm::vec3 right(view[0][0], view[1][0], view[2][0]); // screen x axis in world space
    right.y = 0;
    right = glm::length(right) > 1e-4f ? glm::normalize(right) : glm::vec3(1, 0, 0);
    const glm::vec3 off = rt_.previewMode == 1 ? -right * (std::max(scene_.restMax.x - scene_.restMin.x, scene_.restMax.z - scene_.restMin.z) + 0.6f) : glm::vec3(0);
    std::vector<LineVertex> v;
    // only source bones the mapping uses (helper bones like weapon / camera have joints away from the body)
    std::vector<uint8_t> used(rt_.src.skel.bones.size(), 0);
    for (int sb : rt_.map.srcOf)
        if (sb >= 0 && sb < int(used.size())) used[sb] = 1;
    for (size_t b = 0; b < rt_.src.skel.bones.size() && b < P.size(); ++b)
    {
        if (!used[b]) continue;
        int p = rt_.src.skel.bones[b].parent; // nearest used ancestor (unmapped bones in between are skipped)
        while (p >= 0 && !used[p]) p = rt_.src.skel.bones[p].parent;
        if (p < 0) continue;
        v.push_back({ P[p] + off, glm::vec4(0.3f, 0.95f, 1.0f, 1) });
        v.push_back({ P[b] + off, glm::vec4(0.3f, 0.95f, 1.0f, 1) });
    }
    renderer_.SetLines(LineLayer::Retarget, v);
    if (!rtLinesShown_ && v.size() >= 4)
        Log::Get().Debug("Retarget preview lines: {} vertices, first ({:.2f} {:.2f} {:.2f})-({:.2f} {:.2f} {:.2f}), offset ({:.2f} {:.2f} {:.2f}), frame {}", v.size(), v[0].pos.x,
                         v[0].pos.y, v[0].pos.z, v[1].pos.x, v[1].pos.y, v[1].pos.z, off.x, off.y, off.z, f);
    rtLinesShown_ = true;
}

bool AnimationView::RetargetRun()
{
    if (!rt_.Prepare(session_)) return false;
    SetMovesDrive(false); // the preview plays as the only layer: game mode off
    rt_.StartPreview(session_);
    playback_.time = 0;
    playback_.playing = true;
    host_.PoseChanged();
    return true;
}

void AnimationView::RetargetStopPreview()
{
    if (!rt_.previewOn) return;
    rt_.StopPreview(session_);
    renderer_.SetLines(LineLayer::Retarget, {});
    host_.PoseChanged();
}

void AnimationView::SetMovesDrive(bool on)
{
    if (on == movesSim_.drive) return;
    gesturePlayer_.Stop();
    int started = -2;
    movesSim_.SetDrive(on, playback_.time, &started);
    if (on) playback_.playing = true;
    host_.WatchedFilesChanged();
    host_.PoseChanged();
}

void AnimationView::UpdateMovesPlayback()
{
    const int r = movesSim_.Update(playback_.time);
    if (r < 0) return;
    if (r > 0) host_.WatchedFilesChanged();
    host_.PoseChanged();
}

void AnimationView::MovesReset()
{
    if (session_.moves.Empty()) { movesSim_.msg = T("No CfgMoves for this model - nothing to reset"); return; }
    if (!movesSim_.drive) SetMovesDrive(true);
    gesturePlayer_.Stop();
    control_.rootPos = glm::vec3(0);
    control_.rootYaw = 0;
    control_.footsteps.clear();
    renderer_.SetLines(LineLayer::Footsteps, {});
    control_.action.clear();
    control_.moving = false;
    movesSim_.Reset(playback_.time);
    playback_.playing = true;
    host_.PoseChanged();
}

void AnimationView::MovesPlay(const std::string& text, bool switchMove, bool user)
{
    const int t = movesSim_.Resolve(text, switchMove, user);
    if (t < 0) return;
    if (!movesSim_.drive) SetMovesDrive(true);
    movesSim_.Go(text, t, switchMove, playback_.time);
    playback_.playing = true;
    host_.PoseChanged();
}

bool AnimationView::LayersOpenInBlender() { return layers_.OpenInBlender(false, RunProgram); }
fs::path AnimationView::RetargetPresetFile() const { return rt_.PresetFile(session_.skeleton); }
void AnimationView::RetargetLoadSource() { rt_.LoadSource(session_); }
bool AnimationView::RetargetExport(bool overwrite) { return rt_.Export(overwrite); }
