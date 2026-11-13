#include "Playback.h"
#include "Log.h"
#include "Text.h"

#include <algorithm>
#include <cmath>

#include "CoreGuard.h"

int Playback::PrimaryLayer() const
{
    const auto& layers = s_.anim.layers;
    for (int i = 0; i < int(layers.size()); ++i)
        if (layers[size_t(i)].enabled && layers[size_t(i)].anim) return i;
    return -1;
}

double Playback::TimeOfPhase(const AnimLayer& l, float phase) const
{
    return double(phase - l.offset) * l.duration / std::max(l.speed, 1e-4f);
}

void Playback::StepFrame(int dir)
{
    const int p = PrimaryLayer();
    if (p < 0) return;
    const AnimLayer& l = s_.anim.layers[size_t(p)];
    playing = false;
    const double frames = double(std::max<size_t>(2, l.anim->Frames()) - 1);
    const double phase = std::round(double(s_.anim.Phase(l, time)) * frames) + dir;
    time = (phase / frames - l.offset) * l.duration / std::max(l.speed, 1e-4f);
}

void Playback::ScrubTo(float phase)
{
    const int p = PrimaryLayer();
    if (p < 0) return;
    playing = false;
    AnimLayer tmp = s_.anim.layers[size_t(p)];
    tmp.pingPong = false;
    time = TimeOfPhase(tmp, std::min(std::clamp(phase, 0.0f, 1.0f), 0.99999f));
}

bool Playback::MoveToCompare(int i)
{
    auto& layers = s_.anim.layers;
    if (i < 0 || i >= int(layers.size())) return false;
    // the layer becomes B (A = everything else); a previous B goes back to the layers
    if (!compare.layers.empty()) layers.push_back(compare.layers[0]);
    compare.layers = { layers[size_t(i)] };
    compare.layers[0].enabled = true;
    compare.layers[0].weight = 1.0f;
    layers.erase(layers.begin() + i);
    compare.missing = s_.anim.missing;
    compare.Bind(s_.skeleton);
    if (!compareMode) compareMode = 2;
    return true;
}

bool Playback::ReturnCompare()
{
    if (compare.layers.empty()) return false;
    s_.anim.layers.push_back(compare.layers[0]);
    compare.layers.clear();
    return true;
}

bool Playback::UpdateSeam()
{
    const int p = PrimaryLayer();
    if (p < 0) return false;
    const AnimLayer& l = s_.anim.layers[size_t(p)];
    if (seamAnim == l.anim.get()) return false;
    seamAnim = l.anim.get();
    seam = l.loop && !l.pingPong ? CheckLoopSeam(*l.anim) : std::vector<SeamJump>{};
    if (!seam.empty())
        Log::Get().Warn("RTM {}: loop seam - first and last frame differ on {} bone(s), worst {} ({:.3f} m, {:.1f} deg)", l.anim->Name(), seam.size(),
                        seam[0].bone, seam[0].distance, seam[0].angle);
    return true;
}

bool Playback::AddLayer(const std::filesystem::path& path)
{
    auto& log = Log::Get();
    const std::string name = PathToUtf8(path.filename());
    auto a = std::make_shared<RtmAnimation>();
    std::string err;
    std::vector<std::string> warnings;
    if (!LoadRtm(path, *a, err, warnings))
    {
        if (a->info.binarized) log.Warn("RTM '{}': {}", name, err);
        else log.Error("RTM '{}': {}", name, err);
        return false;
    }
    for (const auto& w : warnings) log.Warn("RTM '{}': {}", name, w);
    return AddLayer(a, float(std::max<size_t>(1, a->Frames() - 1)) / fps);
}

bool Playback::AddLayer(std::shared_ptr<RtmAnimation> a, float duration)
{
    auto& log = Log::Get();
    const std::string name = a->Name();
    AnimLayer layer;
    layer.anim = a;
    layer.duration = std::max(duration, 1e-3f);
    const bool first = s_.anim.layers.empty();
    s_.anim.layers.push_back(std::move(layer));
    s_.anim.Bind(s_.skeleton);
    const AnimLayer& l = s_.anim.layers.back();
    log.Info("Animation layer '{}': {} frames, {} bones ({} match the skeleton), move ({:.3f} {:.3f} {:.3f}), duration {:.2f} s",
             name, a->Frames(), a->Bones(), l.matched, a->info.move.x, a->info.move.y, a->info.move.z, l.duration);
    if (s_.skeleton.Empty()) log.Warn("Animation '{}': no skeleton loaded yet - it plays once a model with a skeleton is open", name);
    else if (l.matched == 0) log.Warn("Animation '{}': no RTM bone matches skeleton {}", name, s_.skeleton.name);
    if (first)
    {
        time = 0.0;
        playing = true;
    }
    s_.Events().Publish(SessionEvent::LayerAdded);
    return true;
}

bool Playback::LoadState(const std::string& name)
{
    auto& log = Log::Get();
    state = LookupMovesState(s_.CrewConfig(), name);
    if (!state.found)
    {
        log.Warn("CfgMovesMaleSdr >> States >> {}: not found{}", name, s_.game ? "" : " (game configs not loaded yet)");
        return false;
    }
    std::filesystem::path file = ResolveArmaPath(state.file, s_.projectRoot);
    if (!file.has_extension()) file += ".rtm";
    if (!AddLayer(file)) return false;
    AnimLayer& l = s_.anim.layers.back();
    if (float d = DurationFromMovesSpeed(state.speed); d > 0.0f) l.duration = d; // 0 = static pose: keep the fps-based one
    l.loop = state.looped;
    log.Info("State {}: {} - speed {} -> {:.2f} s per cycle, interpolationSpeed {}, {}", name, state.file, state.speed, l.duration,
             state.interpolationSpeed, l.loop ? "looped" : "not looped");
    return true;
}
