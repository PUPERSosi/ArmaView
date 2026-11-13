#include "MovesLoader.h"
#include "Log.h"
#include "Perf.h"
#include "Text.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

#include "CoreGuard.h"

namespace fs = std::filesystem;

namespace {
bool ContainsNoCase(const std::string& hay, const std::string& needle)
{
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                       [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); }) != hay.end();
}
} // namespace

void MovesLoader::Rebuild()
{
    const ArmaConfig* cfg = s_.CrewConfig();
    MovesGraph& moves = s_.moves;
    const MovesPlayer& p = sim_.player;
    keepCur_ = p.Current() >= 0 && p.Current() < int(moves.states.size()) ? moves.states[size_t(p.Current())].name : "";
    keepStart_ = p.Cur().start;
    moves = MovesGraph{};
    classes.clear();
    validation = {};
    lazyCls_.clear();
    if (!cfg) return;
    classes = ListMovesClasses(*cfg);
    std::string cls = chosen;
    if (cls.empty())
    {
        // the move config of the loaded skeleton (gesture configs have the same skeletonName: skip them)
        for (const auto& c : classes)
        {
            const CfgClass* k = cfg->Root().FindClass(c);
            if (k && !s_.skeleton.Empty() && IEquals(k->GetString("skeletonName"), s_.skeleton.name) && !ContainsNoCase(c, "gesture"))
            {
                cls = c;
                break;
            }
        }
        // a weapon / vehicle / prop: no move config of its own - the game's man config only when a Moves window or
        // command needs it (Ensure), never while the model opens
        if (cls.empty() && cfg->Root().FindClass("CfgMovesMaleSdr"))
        {
            lazyCls_ = "CfgMovesMaleSdr";
            return;
        }
    }
    if (!cls.empty()) StartBuild(cls);
}

// built on a worker (the game's CfgMovesMaleSdr: ~4 s) or read from its disk cache (key: the config files' sizes and times)
void MovesLoader::StartBuild(const std::string& cls)
{
    std::shared_ptr<const ArmaConfig> cfg = s_.modCfg ? std::shared_ptr<const ArmaConfig>(s_.modCfg) : s_.game;
    if (!cfg) return;
    if (job_.valid()) job_.wait(); // a previous build (same inputs at most once at a time)
    const fs::path root = s_.projectRoot;
    const fs::path cacheFile = cacheDir_.empty() ? fs::path() : cacheDir_ / ("moves_" + ToLower(cls) + ".bin");
    jobCls_ = cls;
    job_ = std::async(std::launch::async, [cfg, cls, root, cacheFile]() -> std::shared_ptr<MovesGraph> {
        auto g = std::make_shared<MovesGraph>();
        const auto t0 = std::chrono::steady_clock::now();
        const std::string key = MovesCacheKey(*cfg, cls, root);
        const bool cached = !cacheFile.empty() && g->LoadCache(cacheFile, key);
        if (!cached)
        {
            if (!g->Build(*cfg, cls, root)) return nullptr;
            if (!cacheFile.empty()) g->SaveCache(cacheFile, key);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        Perf::Get().Note(cached ? "CfgMoves graph (disk cache)" : "CfgMoves graph (built)", ms);
        Log::Get().Debug("CfgMoves {}: {} in {:.0f} ms (background)", cls, cached ? "read from the cache" : "built and cached", ms);
        return g;
    });
}

bool MovesLoader::Poll(bool wait)
{
    if (!job_.valid()) return false;
    if (!wait && job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    std::shared_ptr<MovesGraph> g = job_.get();
    if (!g) return false;
    MovesGraph& moves = s_.moves;
    moves = std::move(*g);
    size_t actions = 0;
    for (const auto& [k, mm] : moves.actions) actions += mm.size();
    Log::Get().Info("CfgMoves {} (skeleton {}, gestures {} with {} states): {} states, {} action classes ({} entries), from {} file(s)", jobCls_,
                    moves.skeletonName, moves.gestures, moves.gestureStates, moves.states.size(), moves.actions.size(), actions, moves.configFiles.size());
    MovesPlayer& p = sim_.player;
    if (const int s = keepCur_.empty() ? -1 : moves.Find(keepCur_); s >= 0)
    {
        p.SwitchMove(moves, s, keepStart_);
        p.KeepStart(keepStart_); // keep the phase across a reload
    }
    else
        p.Reset();
    RebuildGestures();
    return true;
}

bool MovesLoader::Ensure(bool wait)
{
    if (s_.moves.Empty() && !job_.valid() && !lazyCls_.empty())
    {
        StartBuild(lazyCls_);
        lazyCls_.clear();
    }
    if (wait) Poll(true);
    return !s_.moves.Empty();
}

void MovesLoader::RebuildGestures()
{
    s_.gestures = MovesGraph{};
    s_.masks.clear();
    const ArmaConfig* cfg = s_.CrewConfig();
    const MovesGraph& moves = s_.moves;
    if (!cfg || moves.Empty()) return;
    s_.masks = ReadBlendAnims(*cfg, moves.cls);
    if (!moves.gestures.empty() && s_.gestures.Build(*cfg, moves.gestures, s_.projectRoot))
        for (auto& [k, m] : ReadBlendAnims(*cfg, moves.gestures)) s_.masks[k] = m; // gesture masks win
    Log::Get().Info("Gestures {}: {} states, {} BlendAnims masks", moves.gestures.empty() ? "-" : moves.gestures, s_.gestures.states.size(), s_.masks.size());
}

void MovesLoader::StartValidation(bool sync)
{
    const MovesGraph& moves = s_.moves;
    if (moves.Empty() || valJob_.valid()) return;
    auto g = std::make_shared<MovesGraph>(moves);
    std::shared_ptr<Skeleton> skel;
    if (!s_.skeleton.Empty() && IEquals(s_.skeleton.name, moves.skeletonName)) skel = std::make_shared<Skeleton>(s_.skeleton);
    const bool checkSeams = seams;
    validation = {};
    valJob_ = std::async(std::launch::async, [g, skel, checkSeams] {
        MovesValidation v = ValidateMoves(*g, skel.get(), checkSeams);
        if (!skel) v.summary += " (RTM bones not checked: load a model with skeleton " + g->skeletonName + ")";
        return v;
    });
    if (sync) valJob_.wait();
}

bool MovesLoader::PollValidation()
{
    if (!valJob_.valid() || valJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    validation = valJob_.get();
    Log::Get().Info("CfgMoves validation {}: {}", s_.moves.cls, validation.summary);
    int n = 0, m = 0;
    for (const auto& it : validation.items)
    {
        const bool action = it.text.starts_with("Actions") || it.text.starts_with("ManActions");
        if (!it.info && (action ? m++ < 25 : n++ < 25)) Log::Get().Debug("  {}", it.text);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------- gestures

std::vector<float> GesturePlayer::MaskWeights(const std::string& mask) const
{
    const Skeleton& sk = s_.skeleton;
    std::vector<float> w(sk.bones.size(), 0.0f);
    auto it = s_.masks.find(ToLower(mask));
    if (it == s_.masks.end()) return w;
    for (size_t b = 0; b < sk.bones.size(); ++b)
        if (auto jt = it->second.find(ToLower(sk.bones[b].name)); jt != it->second.end()) w[b] = jt->second;
    return w;
}

const AnimLayer* GesturePlayer::Layer() const
{
    auto it = std::find_if(s_.anim.layers.begin(), s_.anim.layers.end(), [](const AnimLayer& l) { return l.tag == 1; });
    return it == s_.anim.layers.end() ? nullptr : &*it;
}

bool GesturePlayer::Play(const std::string& name, double time)
{
    const MovesGraph& gestures = s_.gestures;
    std::string& msg = sim_.msg;
    const int g = gestures.Find(name);
    if (g < 0)
    {
        msg = std::format("playGesture {}: no such gesture in {}", name, s_.moves.gestures);
        return false;
    }
    auto anim = std::make_shared<RtmAnimation>();
    std::string err;
    std::vector<std::string> warn;
    const MoveState& s = gestures.states[size_t(g)];
    if (!s.fileExists || !LoadRtm(s.path, *anim, err, warn))
    {
        msg = std::format("playGesture {}: RTM {} {}", name, s.file, s.fileExists ? err : "not on disk");
        return false;
    }
    Stop();
    AnimLayer l;
    l.anim = anim;
    l.tag = 1;
    l.mode = LayerMode::Override;
    l.boneWeight = MaskWeights(s.mask);
    l.duration = s.duration;
    l.speed = 0.0f;
    l.loop = false;
    l.weight = 0.0f;
    s_.anim.layers.push_back(l);
    s_.anim.Bind(s_.skeleton);
    current = g;
    start = time;
    msg = std::format("playGesture {}: mask {} ({} bones), {:.2f} s{}", s.name, s.mask.empty() ? "-" : s.mask,
                      std::count_if(l.boneWeight.begin(), l.boneWeight.end(), [](float w) { return w > 0; }), s.duration, s.looped ? " looped" : "");
    Log::Get().Info("CfgMoves: {}", msg);
    s_.Events().Publish(SessionEvent::AnimationChanged);
    return true;
}

void GesturePlayer::Stop()
{
    current = -1;
    if (std::erase_if(s_.anim.layers, [](const AnimLayer& l) { return l.tag == 1; })) s_.Events().Publish(SessionEvent::AnimationChanged);
}

bool GesturePlayer::Update(double time)
{
    if (current < 0) return false;
    auto it = std::find_if(s_.anim.layers.begin(), s_.anim.layers.end(), [](const AnimLayer& l) { return l.tag == 1; });
    if (it == s_.anim.layers.end())
    {
        current = -1;
        return false;
    }
    const MoveState& s = s_.gestures.states[size_t(current)];
    const double t = time - start;
    const double cyc = t / std::max(s.duration, 1e-4f);
    if (!s.looped && cyc >= 1.0)
    {
        Stop();
        return true;
    }
    it->offset = float(s.looped ? cyc - std::floor(cyc) : cyc);
    // fade in / out over 1 / interpolationSpeed of the gesture state (approximation)
    it->weight = GestureFadeWeight(t, s.duration, s.interpolationSpeed, s.looped);
    return true;
}

// Approximation of weapon aiming: the bones of the state's aiming mask pitch, of aimingBody yaw, around the pelvis,
// scaled by the mask weights (the engine aims with its own controllers - this only shows which bones follow).
void GesturePlayer::ApplyAim(std::vector<glm::mat4>& bones) const
{
    const MovesGraph& moves = s_.moves;
    const int cur = sim_.player.Current();
    if (!aimApprox || (aimYaw == 0.0f && aimPitch == 0.0f) || moves.Empty() || cur < 0) return;
    const Skeleton& sk = s_.skeleton;
    const MoveState& s = moves.states[size_t(cur)];
    const std::vector<float> pitchW = MaskWeights(s.aiming), yawW = MaskWeights(s.aimingBody);
    const int pelvis = sk.Find("Pelvis");
    const glm::vec3 pivot = pelvis >= 0 ? glm::vec3(bones[size_t(pelvis)] * glm::vec4(sk.bones[size_t(pelvis)].pivot, 1.0f)) : glm::vec3(0, 1, 0);
    for (size_t b = 0; b < bones.size(); ++b)
    {
        const float wp = b < pitchW.size() ? pitchW[b] : 0.0f, wy = b < yawW.size() ? yawW[b] : 0.0f;
        if (wp <= 0 && wy <= 0) continue;
        const glm::quat q = glm::angleAxis(glm::radians(aimYaw) * wy, glm::vec3(0, 1, 0)) * glm::angleAxis(glm::radians(-aimPitch) * wp, glm::vec3(1, 0, 0));
        bones[b] = glm::translate(glm::mat4(1.0f), pivot) * glm::mat4_cast(q) * glm::translate(glm::mat4(1.0f), -pivot) * bones[b];
    }
}
