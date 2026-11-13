#include "MovesSim.h"
#include "Log.h"
#include "Strings.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <format>

#include "CoreGuard.h"

int MovesSim::StartState(std::string* why) const
{
    const MovesGraph& moves = s_.moves;
    std::string w;
    if (auto it = startPick.find(ToLower(PathToUtf8(s_.model.path))); it != startPick.end() && !it->second.empty())
    {
        const int s = moves.Find(it->second);
        if (s >= 0 && moves.states[size_t(s)].fileExists)
        {
            if (why) *why = TF("picked in the Moves panel (Start state) for this model{}", moves.IsDeathLike(s) ? T(" - a death state!") : "");
            return s;
        }
        w = TF("the picked start state {} is {}; ", it->second, T(s < 0 ? "not in this CfgMoves" : "without an RTM on disk"));
    }
    const int s = moves.StartState(*(why ? why : &w));
    if (why && !w.empty() && w != *why) *why = w + *why;
    return s;
}

std::shared_ptr<RtmAnimation> MovesSim::Rtm(int state)
{
    const MovesGraph& moves = s_.moves;
    if (state < 0 || state >= int(moves.states.size())) return nullptr;
    const MoveState& s = moves.states[size_t(state)];
    if (!s.fileExists) return nullptr;
    const std::string key = ToLower(PathToUtf8(s.path));
    if (auto it = rtm_.find(key); it != rtm_.end()) return it->second;
    auto a = std::make_shared<RtmAnimation>();
    std::string err;
    std::vector<std::string> warnings;
    if (!LoadRtm(s.path, *a, err, warnings))
    {
        Log::Get().Warn("CfgMoves state {}: RTM '{}': {}", s.name, s.file, err);
        a = nullptr;
    }
    rtm_[key] = a;
    return a;
}

void MovesSim::Replace(const std::filesystem::path& file, const std::shared_ptr<RtmAnimation>& a)
{
    std::error_code ec;
    for (auto& [k, m] : rtm_)
        if (m && std::filesystem::equivalent(m->path, file, ec)) m = a;
}

void MovesSim::SetDrive(bool on, double time, int* startedAt)
{
    if (startedAt) *startedAt = -2;
    if (on == drive) return;
    drive = on;
    if (on)
    {
        saved_ = std::move(s_.anim.layers);
        s_.anim.layers.clear();
        if (player.Current() < 0 && !s_.moves.Empty())
        {
            const int s = StartState(&startWhy);
            if (s >= 0) player.SwitchMove(s_.moves, s, time);
            if (s >= 0) Log::Get().Info("CfgMoves start state {}: {}", s_.moves.states[size_t(s)].name, startWhy);
            else Log::Get().Warn("CfgMoves {}: no start state - {}", s_.moves.cls, startWhy);
            if (startedAt) *startedAt = s;
        }
    }
    else
    {
        s_.anim.layers = std::move(saved_);
        saved_.clear();
        s_.anim.Bind(s_.skeleton);
    }
}

int MovesSim::Update(double time)
{
    const MovesGraph& moves = s_.moves;
    AnimStack& anim = s_.anim;
    if (!drive || moves.Empty() || player.Current() < 0) return -1;
    player.Update(moves, time);
    struct Want { int state; float weight; float phase; };
    std::vector<Want> want;
    const float w = player.BlendWeight(moves, time);
    want.push_back({ player.Current(), w, player.PhaseOf(moves, player.Cur(), time) });
    if (player.Previous() >= 0 && w < 1.0f) want.push_back({ player.Previous(), 1.0f - w, player.PhaseOf(moves, player.Prev(), time) });
    std::vector<std::shared_ptr<RtmAnimation>> anims;
    for (const auto& x : want) anims.push_back(Rtm(x.state));
    std::vector<AnimLayer> extra; // viewer layers on top (gesture)
    for (const auto& l : anim.layers)
        if (l.tag) extra.push_back(l);
    const size_t base = anim.layers.size() - extra.size();
    size_t wanted = 0;
    for (const auto& a : anims) wanted += a ? 1 : 0;
    bool same = base == wanted;
    for (size_t i = 0, li = 0; same && i < want.size(); ++i)
        if (anims[i])
        {
            same = anim.layers[li++].anim == anims[i];
        }
    if (!same)
    {
        anim.layers.clear();
        for (size_t i = 0; i < want.size(); ++i)
        {
            if (!anims[i]) continue;
            AnimLayer l;
            l.anim = anims[i];
            anim.layers.push_back(l);
        }
        for (auto& e : extra) anim.layers.push_back(std::move(e));
        anim.Bind(s_.skeleton);
    }
    size_t li = 0;
    for (size_t i = 0; i < want.size() && li < anim.layers.size(); ++i)
    {
        if (!anims[i]) continue;
        AnimLayer& l = anim.layers[li++];
        // phase is set by the player: no own clock in the layer
        l.speed = 0.0f;
        l.loop = false;
        l.pingPong = false;
        l.offset = want[i].phase;
        l.weight = want[i].weight;
        l.duration = moves.states[size_t(want[i].state)].duration;
        l.mode = LayerMode::Blend;
    }
    return same ? 0 : 1;
}

int MovesSim::Reset(double time)
{
    const int s = StartState(&startWhy);
    player.Reset();
    if (s >= 0)
    {
        player.SwitchMove(s_.moves, s, time);
        msg = TF("Reset: start state {} at the origin", s_.moves.states[size_t(s)].name);
        Log::Get().Info("CfgMoves reset: start state {} ({})", s_.moves.states[size_t(s)].name, startWhy);
    }
    else
    {
        msg = TF("No start state: {}", startWhy);
        Log::Get().Warn("CfgMoves reset: no start state - {}", startWhy);
    }
    return s;
}

int MovesSim::Resolve(const std::string& text, bool switchMove, bool user)
{
    const MovesGraph& moves = s_.moves;
    if (moves.Empty()) { msg = T("No CfgMoves loaded (game configs not loaded yet?)"); return -1; }
    const int cur = player.Current();
    const int t = moves.StateOfAction(cur, text);
    if (t < 0) { msg = TF("\"{}\": no such state, nor an action of {}", text, cur >= 0 ? moves.states[size_t(cur)].actions : "-"); return -1; }
    // death / unconscious only on an explicit request; a path through such a state is reported
    if (!user && moves.IsDeathLike(t))
    {
        msg = TF("{} -> {}: a death / unconscious state is entered only by an explicit command (not by a key or an empty action)", text, moves.states[size_t(t)].name);
        Log::Get().Warn("CfgMoves: {}", msg);
        return -1;
    }
    if (!switchMove && cur >= 0)
        for (int s : moves.Path(cur, t))
            if (s != t && s != cur && moves.IsDeathLike(s))
            {
                Log::Get().Warn("CfgMoves: the path {} -> {} goes through the death / unconscious state {}{}", moves.states[size_t(cur)].name,
                                moves.states[size_t(t)].name, moves.states[size_t(s)].name, user ? "" : " - not taken (control mode)");
                if (!user) { msg = TF("path to {} goes through {} (death) - not taken", moves.states[size_t(t)].name, moves.states[size_t(s)].name); return -1; }
            }
    if (moves.IsDeathLike(t)) Log::Get().Warn("CfgMoves: {} is a death / unconscious state (explicit command)", moves.states[size_t(t)].name);
    return t;
}

void MovesSim::Go(const std::string& text, int t, bool switchMove, double time)
{
    const MovesGraph& moves = s_.moves;
    const int cur = player.Current();
    if (switchMove || cur < 0)
    {
        player.SwitchMove(moves, t, time);
        msg = std::format("switchMove {}", moves.states[size_t(t)].name);
    }
    else
    {
        float cost = 0;
        if (!player.PlayMove(moves, t, time, &cost))
            msg = std::format("playMove {}: no path from {} (ConnectTo / InterpolateTo)", moves.states[size_t(t)].name, moves.states[size_t(cur)].name);
        else
        {
            std::string p;
            for (int s : player.PathShown) p += (p.empty() ? "" : " > ") + moves.states[size_t(s)].name;
            msg = std::format("playMove {}: cost {:.3g}, {} step(s): {}", moves.states[size_t(t)].name, cost, player.PathShown.size() - 1, p);
        }
    }
    Log::Get().Info("CfgMoves: {}", msg);
    LogEntry e{ time, text, cur >= 0 ? moves.states[size_t(cur)].name : std::string("-"), "", "", 0.0f };
    const auto& p = player.PathShown;
    for (size_t k = 0; k < p.size(); ++k)
    {
        e.path += (k ? " > " : "") + moves.states[size_t(p[k])].name;
        if (k)
        {
            bool interp = false;
            for (const auto& ed : moves.states[size_t(p[k - 1])].edges)
                if (ed.to == p[k]) interp = ed.interpolate;
            e.kinds += interp ? "I" : "C";
            if (interp) e.blend += 1.0f / std::max(moves.states[size_t(p[k])].interpolationSpeed, 0.01f);
        }
    }
    if (switchMove) e.kinds = "switch";
    log.push_back(e);
    if (log.size() > 500) log.erase(log.begin());
}

// ============================================================================================ control mode

void MovesControl::DefaultKeymap()
{
    // stance keys toggle like the game: C crouch <-> stand, Z prone <-> stand; X (or Shift+C) stands up from anything.
    // ToggleCrouch / ToggleProne are resolved by the current state's stance (KeyAction).
    keymap = { { "W", "WalkF" }, { "Shift+W", "FastF" }, { "S", "WalkB" }, { "A", "WalkL" }, { "D", "WalkR" }, { "W+A", "WalkLF" }, { "W+D", "WalkRF" },
               { "C", "ToggleCrouch" }, { "Shift+C", "PlayerStand" }, { "Z", "ToggleProne" }, { "X", "PlayerStand" } };
}

std::string MovesControl::KeyAction(const KeyState& keys, const MovesGraph& moves, int c)
{
    // most specific binding whose keys are all held
    auto held = [&](const std::string& combo, int& count) {
        count = 0;
        size_t a = 0;
        for (;;)
        {
            const size_t b = combo.find('+', a);
            const std::string k = combo.substr(a, b == std::string::npos ? std::string::npos : b - a);
            ++count;
            bool down = false;
            if (IEquals(k, "Shift")) down = keys.shift;
            else if (IEquals(k, "Ctrl")) down = keys.ctrl;
            else if (k.size() == 1 && std::isalpha(uint8_t(k[0]))) down = keys.letter[size_t(std::toupper(uint8_t(k[0])) - 'A')];
            if (!down) return false;
            if (b == std::string::npos) return true;
            a = b + 1;
        }
    };
    std::string want;
    int best = 0;
    for (const auto& kb : keymap)
    {
        int n = 0;
        if (held(kb.keys, n) && n > best) { best = n; want = kb.action; }
    }
    const bool nowMoving = !want.empty() && (want.starts_with("Walk") || want.starts_with("Fast") || want.starts_with("Slow") || want.starts_with("Tact"));
    if (want.empty() && moving) want = "Stop";
    std::string play;
    if (!want.empty() && want != action)
    {
        // stance toggles: by the stance of the current state (BI names Pknl / Ppne, else the upDegree of its actions class)
        play = want;
        if (want == "ToggleCrouch" || want == "ToggleProne")
        {
            std::string n, deg;
            if (c >= 0)
            {
                n = ToLower(moves.states[size_t(c)].name);
                if (auto u = moves.upDegree.find(ToLower(moves.states[size_t(c)].actions)); u != moves.upDegree.end()) deg = ToLower(u->second);
            }
            const bool crouched = n.find("pknl") != std::string::npos || deg.find("crouch") != std::string::npos;
            const bool prone = n.find("ppne") != std::string::npos || deg.find("lying") != std::string::npos;
            play = want == "ToggleCrouch" ? (crouched ? "PlayerStand" : "PlayerCrouch") : (prone ? "PlayerStand" : "PlayerProne");
        }
        action = want;
    }
    moving = nowMoving;
    if (want.empty()) action.clear();
    return play;
}

void MovesControl::Move(MovesSim& sim, const MovesGraph& moves, double time, float dt, float speed, bool turnLeft, bool turnRight)
{
    // Arma model space is left-handed (x right, y up, z forward): a positive rotation about y turns z towards x, i.e.
    // to the right. Q turns left, E right.
    if (turnLeft) rootYaw -= 90.0f * dt;
    if (turnRight) rootYaw += 90.0f * dt;
    // root motion: the RTM step (move per cycle) of the current state, blended with the previous one
    auto step = [&](int st) {
        if (st < 0) return glm::vec3(0);
        auto a = sim.Rtm(st);
        const MoveState& s = moves.states[size_t(st)];
        // the RTM step is in the engine's (reversed) model space: MLOD x, z are mirrored (CWR ShapeFile.cpp:426-468)
        const glm::vec3 m = a ? glm::vec3(-a->info.move.x, a->info.move.y, -a->info.move.z) : glm::vec3(0);
        return a && s.looped ? m / std::max(s.duration, 1e-3f) : glm::vec3(0);
    };
    const float w = sim.player.BlendWeight(moves, time);
    const glm::vec3 v = step(sim.player.Current()) * w + step(sim.player.Previous()) * (1.0f - w);
    const glm::mat3 r = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(rootYaw), glm::vec3(0, 1, 0)));
    rootPos += r * v * dt * speed;
}

glm::mat4 MovesControl::RootMatrix() const
{
    return glm::translate(glm::mat4(1.0f), rootPos) * glm::rotate(glm::mat4(1.0f), glm::radians(rootYaw), glm::vec3(0, 1, 0));
}

bool MovesControl::AddFootsteps(const std::vector<glm::mat4>& bones, const Skeleton& skel)
{
    bool added = false;
    for (const char* f : { "LeftFoot", "RightFoot" })
    {
        const int b = skel.Find(f);
        if (b < 0 || b >= int(bones.size())) continue;
        const glm::vec3 p = glm::vec3(bones[size_t(b)] * glm::vec4(skel.bones[size_t(b)].pivot, 1.0f));
        if (p.y < 0.2f && (footsteps.empty() || glm::distance(footsteps.back(), p) > 0.35f))
        {
            bool close = false;
            for (size_t k = footsteps.size() > 6 ? footsteps.size() - 6 : 0; k < footsteps.size(); ++k) close |= glm::distance(footsteps[k], p) < 0.25f;
            if (!close)
            {
                footsteps.push_back(glm::vec3(p.x, 0.005f, p.z));
                if (footsteps.size() > 80) footsteps.erase(footsteps.begin());
                added = true;
            }
        }
    }
    return added;
}
