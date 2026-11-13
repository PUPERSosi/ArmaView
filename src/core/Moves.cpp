#include "Moves.h"
#include "ArmaConfig.h"
#include "Strings.h"
#include "Text.h"
#include "RTM.h"
#include "Skeleton.h"

#include <algorithm>
#include <fstream>
#include <cmath>
#include <format>
#include <queue>
#include <set>

#include "CoreGuard.h"

namespace fs = std::filesystem;

namespace {

std::string Lower(std::string s)
{
    for (auto& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

// "ConnectTo[] = {state, cost, state, cost ...}"; also accepts a plain list of names (cost 1)
std::vector<std::pair<std::string, float>> Pairs(const CfgClass& c, const char* entry)
{
    std::vector<std::pair<std::string, float>> out;
    const CfgEntry* e = c.Find(entry);
    if (!e) return out;
    std::vector<const CfgValue*> flat;
    auto walk = [&](auto&& self, const CfgValue& v) -> void {
        if (v.type == CfgValue::Type::Array)
            for (const auto& item : v.items) self(self, item);
        else
            flat.push_back(&v);
    };
    walk(walk, e->value);
    for (size_t i = 0; i < flat.size(); ++i)
    {
        if (flat[i]->type != CfgValue::Type::String || flat[i]->str.empty()) continue;
        float cost = 1.0f;
        if (i + 1 < flat.size() && flat[i + 1]->type == CfgValue::Type::Number) cost = float(flat[i + 1]->num);
        out.emplace_back(flat[i]->str, cost);
    }
    return out;
}

} // namespace

// ============================================================================================ graph

// the values of one state class (inheritance included); links and actions are the graph's (Build)
static void FillState(const CfgClass& stc, const fs::path& projectRoot, MoveState& s)
{
    const CfgClass* st = &stc;
    s.name = st->name;
    s.file = st->GetString("file");
    s.speed = float(st->GetNumber("speed", 1.0));
    s.duration = DurationFromMovesSpeed(s.speed);
    if (s.duration <= 0.0f) s.duration = 1.0f;
    s.looped = st->GetNumber("looped", 1.0) != 0.0;
    s.interpolationSpeed = float(st->GetNumber("interpolationSpeed", 6.0));
    s.interpolationRestart = int(st->GetNumber("interpolationRestart", 0.0));
    s.actions = st->GetString("actions");
    s.equivalentTo = st->GetString("equivalentTo");
    s.variantsPlayer = Pairs(*st, "variantsPlayer");
    s.variantsAI = Pairs(*st, "variantsAI");
    s.cfgFile = st->file;
    s.cfgLine = st->line;
    s.abstract = s.file.empty();
    s.terminal = st->GetNumber("terminal", 0.0) != 0.0;
    s.mask = st->GetString("mask");
    s.aiming = st->GetString("aiming");
    s.aimingBody = st->GetString("aimingBody");
    auto curve = [&](const char* entry) {
        std::vector<float> c;
        if (const CfgEntry* e = st->Find(entry))
            for (const auto& v : e->value.items)
                if (v.type == CfgValue::Type::Number) c.push_back(float(v.num));
        return c;
    };
    s.leftHandIKCurve = curve("leftHandIKCurve");
    s.rightHandIKCurve = curve("rightHandIKCurve");
    if (!s.abstract)
    {
        fs::path p = ResolveArmaPath(s.file, projectRoot);
        if (!p.has_extension()) p += ".rtm";
        std::error_code ec;
        s.fileExists = fs::exists(p, ec);
        s.path = p;
    }
}

bool MovesGraph::Build(const ArmaConfig& cfg, const std::string& clsName, const fs::path& projectRoot)
{
    *this = MovesGraph{};
    cls = clsName;
    const CfgClass* moves = cfg.Root().FindClass(clsName);
    if (!moves) return false;
    skeletonName = moves->GetString("skeletonName");
    gestures = moves->GetString("gestures");
    const CfgClass* statesCls = moves->FindClass("States");
    if (!statesCls) return false;
    std::set<int> files;
    for (const CfgClass* st : MergedChildClasses(*statesCls))
    {
        MoveState s;
        FillState(*st, projectRoot, s);
        files.insert(st->file);
        index_[Lower(s.name)] = int(states.size());
        states.push_back(std::move(s));
    }
    // edges (second pass: all names known)
    int i = 0;
    for (const CfgClass* st : MergedChildClasses(*statesCls))
    {
        MoveState& s = states[i++];
        s.connectRaw = Pairs(*st, "ConnectTo");
        s.interpolateRaw = Pairs(*st, "InterpolateTo");
        s.base = st->baseName;
        for (int interp = 0; interp < 2; ++interp)
            for (const auto& [to, cost] : interp ? s.interpolateRaw : s.connectRaw)
            {
                const int t = Find(to);
                if (t < 0) { s.badLinks.push_back(std::format("{} {} (cost {:g})", interp ? "InterpolateTo" : "ConnectTo", to, cost)); continue; }
                // one edge per target: the cheaper one; on a tie InterpolateTo (switches at once, ConnectTo waits for the cycle end)
                MoveEdge e{ t, std::max(cost, 0.0f), interp == 1 };
                auto it = std::find_if(s.edges.begin(), s.edges.end(), [&](const MoveEdge& x) { return x.to == t; });
                if (it == s.edges.end()) s.edges.push_back(e);
                else if (e.cost < it->cost || (e.cost == it->cost && e.interpolate)) *it = e;
            }
    }
    // action names: the entries of ManActions (CfgMovesBasic) - a string action entry that names no state is broken
    std::set<std::string> actionKeys;
    if (const CfgClass* ma = moves->FindClass("ManActions"))
        for (const CfgClass* c : ma->Chain())
            for (const auto& e : c->entries) actionKeys.insert(Lower(e.name));
    // gesture actions name states of the gesture config
    std::set<std::string> gestureNames;
    if (const CfgClass* gc = gestures.empty() ? nullptr : cfg.Root().FindClass(gestures))
        if (const CfgClass* gs = gc->FindClass("States"))
            for (const CfgClass* k : MergedChildClasses(*gs)) gestureNames.insert(Lower(k->name));
    gestureStates = gestureNames.size();
    // actions classes: action -> state (strings naming a state; gesture arrays are skipped). Broken entries of the class
    // itself are listed one by one; inherited ones (e.g. the game's NoActions under a custom move set) once per class.
    auto readActions = [&](const CfgClass& a, std::map<std::string, std::string>& out, const std::string& where, bool aggregateAll,
                           std::set<std::string>* none) {
        int inheritedMissing = 0;
        std::string inheritedFrom, example;
        for (const CfgClass* c : a.Chain())
            for (const auto& e : c->entries)
            {
                if (e.value.type != CfgValue::Type::String || e.isArray) continue;
                const std::string key = Lower(e.name);
                if (out.count(key)) continue;
                if (e.value.str.empty())
                {
                    // the nearest definition decides: "" = no action (the inherited value does not apply)
                    out[key] = std::string();
                    if (none && actionKeys.count(key)) none->insert(key);
                    continue;
                }
                if (Find(e.value.str) >= 0)
                {
                    out[key] = e.value.str;
                    actionNames.emplace(key, e.name);
                    continue;
                }
                if (gestureNames.count(Lower(e.value.str))) continue;
                const bool looksLikeState = e.value.str.find_first_of("\\/.") == std::string::npos &&
                                            (actionKeys.count(key) || ((e.value.str.starts_with("A") || e.value.str.starts_with("a")) &&
                                                                       e.value.str.find("Perc") != std::string::npos));
                if (!looksLikeState) continue;
                out[key] = std::string(); // the nearest definition decides
                if (c == &a && !aggregateAll)
                    badActions.push_back(std::format("{} >> {} = \"{}\": no such state", where, e.name, e.value.str));
                else
                {
                    if (!inheritedMissing++) { inheritedFrom = c->name; example = std::format("{} = \"{}\"", e.name, e.value.str); }
                }
            }
        for (auto it = out.begin(); it != out.end();)
            it = it->second.empty() ? out.erase(it) : std::next(it);
        if (inheritedMissing)
            badActions.push_back(std::format("{}: {} inherited / shared action(s) name states this move config does not have (from {}, e.g. {})",
                                             where, inheritedMissing, inheritedFrom, example));
    };
    // only the actions classes this move config's states use (a config inheriting CfgMovesBasic also inherits the
    // game's ~2000 classes, which name the game's states)
    std::set<std::string> used;
    for (const auto& st : states)
        if (!st.actions.empty() && used.insert(Lower(st.actions)).second) actionsOrder.push_back(Lower(st.actions));
    if (const CfgClass* acts = moves->FindClass("Actions"))
        for (const CfgClass* a : MergedChildClasses(*acts))
        {
            const std::string k = Lower(a->name);
            if (!used.count(k)) continue;
            readActions(*a, actions[k], "Actions >> " + a->name, false, &noAction[k]);
            if (const CfgEntry* u = a->Find("upDegree"))
                upDegree[k] = u->value.type == CfgValue::Type::String ? u->value.str : std::format("{:g}", u->value.num);
            files.insert(a->file);
        }
    // ManActions is shared by every move config (CfgMovesBasic): its dead entries are summarised in one line
    if (const CfgClass* ma = moves->FindClass("ManActions")) readActions(*ma, actions["#manactions"], "ManActions", true, nullptr);
    for (int f : files)
        if (auto p = cfg.FileOf(f); !p.empty() && std::find(configFiles.begin(), configFiles.end(), p) == configFiles.end()) configFiles.push_back(p);
    return true;
}

float GestureFadeWeight(double t, float duration, float interpolationSpeed, bool looped)
{
    const double fade = 1.0 / std::max(0.5f, interpolationSpeed);
    double w = std::min(1.0, t / fade);
    if (!looped) w = std::min(w, (double(duration) - t) / fade);
    return float(std::clamp(w, 0.0, 1.0));
}

int MovesGraph::Find(const std::string& name) const
{
    auto it = index_.find(Lower(name));
    return it == index_.end() ? -1 : it->second;
}

std::vector<int> MovesGraph::Path(int from, int to, float* cost) const
{
    const int n = int(states.size());
    if (from < 0 || to < 0 || from >= n || to >= n) return {};
    if (from == to) { if (cost) *cost = 0; return { from }; }
    std::vector<float> dist(n, 1e30f);
    std::vector<int> prev(n, -1);
    using Q = std::pair<float, int>;
    std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
    dist[from] = 0;
    q.push({ 0.0f, from });
    while (!q.empty())
    {
        auto [d, u] = q.top();
        q.pop();
        if (d > dist[u]) continue;
        if (u == to) break;
        for (const auto& e : states[u].edges)
            if (d + e.cost < dist[e.to])
            {
                dist[e.to] = d + e.cost;
                prev[e.to] = u;
                q.push({ dist[e.to], e.to });
            }
    }
    if (prev[to] < 0) return {};
    std::vector<int> path;
    for (int v = to; v >= 0; v = prev[v]) path.push_back(v);
    std::reverse(path.begin(), path.end());
    if (cost) *cost = dist[to];
    return path;
}

int MovesGraph::StateOfAction(int state, const std::string& action) const
{
    const std::string a = Lower(action);
    if (state >= 0 && state < int(states.size()))
    {
        const std::string ac = Lower(states[state].actions);
        if (auto it = actions.find(ac); it != actions.end())
            if (auto jt = it->second.find(a); jt != it->second.end()) return Find(jt->second);
        // set to "" in the state's actions class: no action (ManActions' default, e.g. Die = "Unconscious", does not apply)
        if (auto it = noAction.find(ac); it != noAction.end() && it->second.count(a)) return -1;
    }
    if (auto it = actions.find("#manactions"); it != actions.end())
        if (auto jt = it->second.find(a); jt != it->second.end()) return Find(jt->second);
    return Find(action); // a state name works too
}

bool MovesGraph::IsDeathLike(int state) const
{
    if (state < 0 || state >= int(states.size())) return false;
    const MoveState& s = states[size_t(state)];
    if (s.terminal) return true;
    const std::string n = Lower(s.name), b = Lower(s.base), a = Lower(s.actions);
    for (const char* k : { "die", "dead", "death", "unconscious", "agony", "ragdoll" })
        if (n.find(k) != std::string::npos || b.find(k) != std::string::npos) return true;
    if (n.starts_with("adth") || a.find("dead") != std::string::npos) return true; // BI: Adth* states, DeadActions
    if (auto it = upDegree.find(a); it != upDegree.end() && Lower(it->second) == "manposdead") return true;
    return false;
}

int MovesGraph::StartState(std::string& why) const
{
    why.clear();
    std::string skipped;
    auto usable = [&](int s, const std::string& via) {
        if (s < 0) return false;
        if (IsDeathLike(s)) { skipped += TF("; {} = {} skipped (death / unconscious)", via, states[size_t(s)].name); return false; }
        if (!states[size_t(s)].fileExists)
        {
            skipped += TF("; {} = {} skipped (RTM not on disk: {})", via, states[size_t(s)].name, states[size_t(s)].file);
            return false;
        }
        return true;
    };
    auto actionState = [&](const std::string& ac, const char* act) {
        auto it = actions.find(ac);
        if (it == actions.end()) return -1;
        auto jt = it->second.find(act);
        return jt == it->second.end() ? -1 : Find(jt->second);
    };
    // 1. as the engine: Default of the actions class with the unit's upDegree (a unit without a weapon first)
    for (const char* deg : { "ManPosNoWeapon", "ManPosStand", "ManPosCombat", "ManPosCrouch" })
        for (const auto& ac : actionsOrder)
        {
            auto u = upDegree.find(ac);
            if (u == upDegree.end() || Lower(u->second) != Lower(deg)) continue;
            const int s = actionState(ac, "default");
            if (usable(s, std::format("Actions >> {} (upDegree {}) >> Default", ac, deg)))
            {
                why = TF("as the engine: Default of the actions class with the unit's upDegree - Actions >> {} (upDegree {}) >> Default = {}",
                                  ac, deg, states[size_t(s)].name);
                why += skipped;
                return s;
            }
        }
    // 2. Default, then Stop, of any actions class the states use (custom configs often keep upDegree = -1)
    for (const char* act : { "default", "stop" })
        for (const auto& ac : actionsOrder)
        {
            if (auto u = upDegree.find(ac); u != upDegree.end() && Lower(u->second) == "manposdead") continue;
            const int s = actionState(ac, act);
            if (usable(s, std::format("Actions >> {} >> {}", ac, act)))
            {
                why = TF("no actions class with a standing upDegree - {} of Actions >> {} = {}", act, ac, states[size_t(s)].name);
                why += skipped;
                return s;
            }
        }
    // 3. the first standing idle state (StandIdle / *MstpS* / *idle*)
    for (int i = 0; i < int(states.size()); ++i)
    {
        const std::string n = Lower(states[size_t(i)].name);
        if ((n.find("mstps") != std::string::npos || n.find("idle") != std::string::npos) && !IsDeathLike(i) && states[size_t(i)].fileExists)
        {
            why = TF("no Default / Stop action leads to a usable state - first standing idle state {}", states[size_t(i)].name) + skipped;
            return i;
        }
    }
    // 4. any state with an RTM that is not death
    for (int i = 0; i < int(states.size()); ++i)
        if (!IsDeathLike(i) && !states[size_t(i)].abstract && states[size_t(i)].fileExists)
        {
            why = TF("no idle state - first state with an RTM on disk that is not death: {}", states[size_t(i)].name) + skipped;
            return i;
        }
    why = T("no state with an RTM on disk (other than death)") + skipped;
    return -1;
}

std::vector<std::pair<std::string, int>> MovesGraph::ActionsOf(int state) const
{
    std::vector<std::pair<std::string, int>> out;
    if (state < 0 || state >= int(states.size())) return out;
    if (auto it = actions.find(Lower(states[state].actions)); it != actions.end())
        for (const auto& [a, s] : it->second)
        {
            auto nm = actionNames.find(a);
            out.emplace_back(nm != actionNames.end() ? nm->second : a, Find(s));
        }
    return out;
}

std::vector<int> MovesGraph::Predecessors(int state) const
{
    std::vector<int> out;
    for (int i = 0; i < int(states.size()); ++i)
        for (const auto& e : states[i].edges)
            if (e.to == state) { out.push_back(i); break; }
    return out;
}

std::vector<std::string> ListMovesClasses(const ArmaConfig& cfg)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const ArmaConfig* c = &cfg; c; c = c->Fallback())
        for (const auto& k : c->Root().classes)
            if (!k->external && seen.insert(Lower(k->name)).second)
            {
                const CfgClass* full = cfg.Root().FindClass(k->name);
                if (full && full->FindClass("States") && full->Find("skeletonName") && !full->GetString("skeletonName").empty())
                    out.push_back(k->name);
            }
    std::sort(out.begin(), out.end());
    return out;
}

// ============================================================================================ validation

MovesValidation ValidateMoves(const MovesGraph& g, const Skeleton* skel, bool checkSeams)
{
    MovesValidation v;
    const int n = int(g.states.size());
    // reachability from every state an action leads to
    std::vector<uint8_t> reached(n, 0);
    std::queue<int> q;
    auto seed = [&](int s) { if (s >= 0 && !reached[s]) { reached[s] = 1; q.push(s); } };
    for (const auto& [cls, map] : g.actions)
        for (const auto& [a, s] : map) seed(g.Find(s));
    while (!q.empty())
    {
        int u = q.front();
        q.pop();
        for (const auto& e : g.states[u].edges) seed(e.to);
        if (!g.states[u].equivalentTo.empty()) seed(g.Find(g.states[u].equivalentTo));
        for (const auto& [name, p] : g.states[u].variantsPlayer) seed(g.Find(name)); // idle variants
        for (const auto& [name, p] : g.states[u].variantsAI) seed(g.Find(name));
    }
    std::set<std::string> skelBones;
    if (skel)
        for (const auto& b : skel->bones) skelBones.insert(Lower(b.name));
    for (int i = 0; i < n; ++i)
    {
        const MoveState& s = g.states[i];
        for (const auto& b : s.badLinks)
        {
            v.items.push_back({ i, std::format("{}: {} - no such state", s.name, b), true });
            ++v.badLinks;
        }
        if (s.abstract) continue;
        bool out = false;
        for (const auto& e : s.edges) out = out || e.to != i;
        if (!out && !s.terminal && reached[i]) // script-only states (cutscenes, Acts_*) are left by switchMove by design
        {
            v.items.push_back({ i, std::format("{}: dead end - no ConnectTo / InterpolateTo to another state (playMove cannot leave it)", s.name), false });
            ++v.deadEnds;
        }
        if (!reached[i])
        {
            v.items.push_back({ i, std::format("{}: reachable only by script (switchMove / playMove): no action, link or variant leads here", s.name), false, true });
            ++v.unreachable;
        }
        if (!s.fileExists)
        {
            v.items.push_back({ i, std::format("{}: RTM not on disk: {}", s.name, s.file), true });
            ++v.missingRtm;
            continue;
        }
        if (!skelBones.empty())
        {
            RtmInfo info;
            std::string err;
            if (ReadRtmHeader(s.path, info, err))
            {
                int match = 0;
                for (const auto& b : info.bones) match += skelBones.count(Lower(b)) ? 1 : 0;
                if (!info.bones.empty() && match * 2 < int(info.bones.size()))
                {
                    v.items.push_back({ i, std::format("{}: RTM bones do not fit skeleton {} ({} of {} match)", s.name, g.skeletonName, match, info.bones.size()), true });
                    ++v.wrongSkeleton;
                }
            }
            else
                v.items.push_back({ i, std::format("{}: RTM unreadable: {}", s.name, err), true });
        }
        if (checkSeams && s.looped && skel)
        {
            auto a = std::make_shared<RtmAnimation>();
            std::string err;
            std::vector<std::string> warn;
            if (LoadRtm(s.path, *a, err, warn))
            {
                std::shared_ptr<RtmAnimation> ms = a;
                if (ms)
                {
                    auto jumps = CheckLoopSeam(*ms);
                    if (!jumps.empty())
                    {
                        v.items.push_back({ i, std::format("{}: looped, but first and last frame differ on {} bone(s), worst {} ({:.3f} m, {:.1f} deg)",
                                                           s.name, jumps.size(), jumps[0].bone, jumps[0].distance, jumps[0].angle), false });
                        ++v.seams;
                    }
                }
            }
        }
    }
    for (const auto& b : g.badActions)
    {
        v.items.push_back({ -1, b + " (the game ignores such an action)", false });
        ++v.badActions;
    }
    v.done = true;
    const int errors = v.badLinks + v.missingRtm + v.wrongSkeleton;
    v.summary = std::format("{} states: {} error(s) ({} links to missing states, {} RTMs missing, {} RTMs not for {}); warnings: {} loop seams, "
                            "{} actions to states that exist nowhere, {} dead ends; info: {} states only reachable by script",
                            n, errors, v.badLinks, v.missingRtm, v.wrongSkeleton, g.skeletonName.empty() ? "the skeleton" : g.skeletonName,
                            v.seams, v.badActions, v.deadEnds, v.unreachable);
    return v;
}

// ============================================================================================ player

void MovesPlayer::SwitchMove(const MovesGraph& g, int state, double now)
{
    if (state < 0 || state >= int(g.states.size())) return;
    queue_.clear();
    edgeInterp_.clear();
    prev_ = {};
    cur_ = { state, now, 1.0f };
    blendTime_ = 0;
    waitUntil_ = -1;
    PathShown = { state };
}

bool MovesPlayer::PlayMove(const MovesGraph& g, int target, double now, float* cost)
{
    if (target < 0) return false;
    if (cur_.state < 0) { SwitchMove(g, target, now); return true; }
    std::vector<int> path = g.Path(cur_.state, target, cost);
    if (path.empty()) return false;
    PathShown = path;
    queue_.clear();
    edgeInterp_.clear();
    waitUntil_ = -1;
    for (size_t k = 1; k < path.size(); ++k)
    {
        bool interp = false;
        for (const auto& e : g.states[path[k - 1]].edges)
            if (e.to == path[k]) { interp = e.interpolate; break; }
        queue_.push_back(path[k]);
        edgeInterp_.push_back(interp);
    }
    Update(g, now);
    return true;
}

float MovesPlayer::PhaseOf(const MovesGraph& g, const Slot& s, double now) const
{
    if (s.state < 0) return 0.0f;
    const MoveState& st = g.states[s.state];
    const double cyc = (now - s.start) / std::max(st.duration, 1e-4f);
    if (!st.looped) return float(std::clamp(cyc, 0.0, 1.0));
    return float(cyc - std::floor(cyc));
}

float MovesPlayer::BlendWeight(const MovesGraph&, double now) const
{
    if (prev_.state < 0 || blendTime_ <= 0) return 1.0f;
    return float(std::clamp((now - blendStart_) / blendTime_, 0.0, 1.0));
}

void MovesPlayer::Enter(const MovesGraph& g, int state, double now, bool interpolate)
{
    const MoveState& to = g.states[state];
    Slot next{ state, now, 1.0f };
    // interpolationRestart 0: an interpolated state keeps the phase of the one it blends from (walk <-> run in step)
    if (interpolate && to.interpolationRestart == 0 && cur_.state >= 0 && to.looped)
        next.start = now - double(PhaseOf(g, cur_, now)) * to.duration;
    prev_ = cur_;
    cur_ = next;
    waitUntil_ = -1;
    blendStart_ = now;
    blendTime_ = interpolate ? 1.0f / std::max(to.interpolationSpeed, 0.01f) : 0.05f;
}

double MovesPlayer::CycleEnd(const MovesGraph& g, double now) const
{
    const MoveState& cs = g.states[cur_.state];
    if (!cs.looped) return cur_.start + cs.duration;
    const double k = std::ceil((now - cur_.start) / cs.duration - 1e-9);
    return cur_.start + cs.duration * std::max(1.0, k);
}

void MovesPlayer::Update(const MovesGraph& g, double now)
{
    if (cur_.state < 0) return;
    if (prev_.state >= 0 && BlendWeight(g, now) >= 1.0f) prev_ = {};
    while (!queue_.empty())
    {
        const int next = queue_.front();
        if (edgeInterp_.front())
        {
            // InterpolateTo: blend now, but an intermediate state is shown until its own blend has finished
            if (prev_.state >= 0) break;
            queue_.erase(queue_.begin());
            edgeInterp_.erase(edgeInterp_.begin());
            Enter(g, next, now, true);
            continue;
        }
        // ConnectTo: at the end of the current cycle (non-looped: when it has finished)
        if (waitUntil_ < 0) waitUntil_ = CycleEnd(g, now);
        if (now < waitUntil_) break;
        queue_.erase(queue_.begin());
        edgeInterp_.erase(edgeInterp_.begin());
        Enter(g, next, waitUntil_, false);
    }
}

// ============================================================================================ masks, curves

std::map<std::string, BoneMask> ReadBlendAnims(const ArmaConfig& cfg, const std::string& cls)
{
    std::map<std::string, BoneMask> out;
    std::map<std::string, std::string> alias;
    const CfgClass* c = cfg.Root().FindClass(cls);
    const CfgClass* ba = c ? c->FindClass("BlendAnims") : nullptr;
    if (!ba) return out;
    for (const CfgClass* k : ba->Chain())
        for (const auto& e : k->entries)
        {
            const std::string name = Lower(e.name);
            if (out.count(name) || alias.count(name)) continue;
            if (e.value.type == CfgValue::Type::String) { alias[name] = Lower(e.value.str); continue; }
            if (e.value.type != CfgValue::Type::Array) continue;
            BoneMask m;
            const auto& it = e.value.items;
            for (size_t i = 0; i + 1 < it.size(); i += 2)
                if (it[i].type == CfgValue::Type::String && it[i + 1].type == CfgValue::Type::Number) m[Lower(it[i].str)] = float(it[i + 1].num);
            out[name] = std::move(m);
        }
    for (int pass = 0; pass < 4; ++pass)
        for (const auto& [a, target] : alias)
            if (!out.count(a) && out.count(target)) out[a] = out[target];
    return out;
}

float CurveAt(const std::vector<float>& c, float phase)
{
    if (c.size() < 2) return -1.0f;
    if (phase <= c[0]) return c[1];
    for (size_t i = 2; i + 1 < c.size(); i += 2)
        if (phase <= c[i])
        {
            const float t = (phase - c[i - 2]) / std::max(c[i] - c[i - 2], 1e-6f);
            return c[i - 1] + (c[i + 1] - c[i - 1]) * t;
        }
    return c.back();
}

bool ReadMoveState(const ArmaConfig& cfg, const std::string& cls, const std::string& name, const fs::path& projectRoot, MoveState& out)
{
    const CfgClass* moves = cfg.Root().FindClass(cls);
    const CfgClass* statesCls = moves ? moves->FindClass("States") : nullptr;
    if (!statesCls) return false;
    for (const CfgClass* st : MergedChildClasses(*statesCls))
        if (IEquals(st->name, name))
        {
            out = MoveState{};
            FillState(*st, projectRoot, out);
            return true;
        }
    return false;
}

// ============================================================================================ disk cache

namespace {

struct Out
{
    std::ofstream& f;
    void U(uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); }
    void F(float v) { f.write(reinterpret_cast<const char*>(&v), 4); }
    void B(bool v) { U(v ? 1u : 0u); }
    void S(const std::string& s) { U(uint32_t(s.size())); f.write(s.data(), std::streamsize(s.size())); }
    void P(const fs::path& p) { S(PathToUtf8(p)); }
    void Pairs(const std::vector<std::pair<std::string, float>>& v) { U(uint32_t(v.size())); for (const auto& [a, b] : v) { S(a); F(b); } }
    void Floats(const std::vector<float>& v) { U(uint32_t(v.size())); for (float x : v) F(x); }
    void Strings(const std::vector<std::string>& v) { U(uint32_t(v.size())); for (const auto& x : v) S(x); }
    void Map(const std::map<std::string, std::string>& m) { U(uint32_t(m.size())); for (const auto& [a, b] : m) { S(a); S(b); } }
};

struct In
{
    std::ifstream& f;
    bool ok = true;
    uint32_t U() { uint32_t v = 0; if (!f.read(reinterpret_cast<char*>(&v), 4)) ok = false; return v; }
    float F() { float v = 0; if (!f.read(reinterpret_cast<char*>(&v), 4)) ok = false; return v; }
    bool B() { return U() != 0; }
    std::string S()
    {
        const uint32_t n = U();
        if (!ok || n > (1u << 24)) { ok = false; return {}; }
        std::string s(n, '\0');
        if (n && !f.read(s.data(), n)) ok = false;
        return s;
    }
    fs::path P() { return ArmaStringPath(S()); }
    std::vector<std::pair<std::string, float>> Pairs()
    {
        std::vector<std::pair<std::string, float>> v(ok ? std::min<uint32_t>(U(), 1u << 20) : 0);
        for (auto& [a, b] : v) { a = S(); b = F(); }
        return v;
    }
    std::vector<float> Floats() { std::vector<float> v(ok ? std::min<uint32_t>(U(), 1u << 20) : 0); for (auto& x : v) x = F(); return v; }
    std::vector<std::string> Strings() { std::vector<std::string> v(ok ? std::min<uint32_t>(U(), 1u << 22) : 0); for (auto& x : v) x = S(); return v; }
    std::map<std::string, std::string> Map()
    {
        std::map<std::string, std::string> m;
        for (uint32_t i = 0, n = ok ? U() : 0; i < n && ok; ++i) { std::string a = S(); m[a] = S(); }
        return m;
    }
};

constexpr uint32_t kCacheVersion = 1;

} // namespace

std::string MovesCacheKey(const ArmaConfig& cfg, const std::string& cls, const fs::path& projectRoot)
{
    std::string key = std::format("v{}|{}|{}", kCacheVersion, ToLower(cls), PathToUtf8(projectRoot));
    std::error_code ec;
    for (const auto& f : cfg.Files())
    {
        const auto t = fs::last_write_time(f, ec);
        const auto n = ec ? 0 : fs::file_size(f, ec);
        key += std::format("|{}:{}:{}", PathToUtf8(f), ec ? 0 : n, ec ? 0 : t.time_since_epoch().count());
    }
    return key;
}

bool MovesGraph::SaveCache(const fs::path& file, const std::string& key) const
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    Out o{ f };
    o.S(key);
    o.S(cls); o.S(skeletonName); o.S(gestures);
    o.U(uint32_t(gestureStates));
    o.U(uint32_t(states.size()));
    for (const auto& s : states)
    {
        o.S(s.name); o.S(s.file); o.P(s.path); o.B(s.fileExists); o.F(s.speed); o.F(s.duration); o.B(s.looped);
        o.F(s.interpolationSpeed); o.U(uint32_t(s.interpolationRestart)); o.S(s.actions); o.S(s.equivalentTo);
        o.Pairs(s.variantsPlayer); o.Pairs(s.variantsAI);
        o.U(uint32_t(s.edges.size()));
        for (const auto& e : s.edges) { o.U(uint32_t(e.to)); o.F(e.cost); o.B(e.interpolate); }
        o.Pairs(s.connectRaw); o.Pairs(s.interpolateRaw); o.S(s.base); o.Strings(s.badLinks);
        o.U(uint32_t(s.cfgFile)); o.U(uint32_t(s.cfgLine)); o.B(s.abstract); o.B(s.terminal);
        o.S(s.mask); o.S(s.aiming); o.S(s.aimingBody); o.Floats(s.leftHandIKCurve); o.Floats(s.rightHandIKCurve);
    }
    o.U(uint32_t(actions.size()));
    for (const auto& [k, m] : actions) { o.S(k); o.Map(m); }
    o.Map(actionNames);
    o.Map(upDegree);
    o.U(uint32_t(noAction.size()));
    for (const auto& [k, set] : noAction) { o.S(k); o.Strings(std::vector<std::string>(set.begin(), set.end())); }
    o.Strings(actionsOrder);
    o.U(uint32_t(configFiles.size()));
    for (const auto& p : configFiles) o.P(p);
    o.Strings(badActions);
    return bool(f);
}

bool MovesGraph::LoadCache(const fs::path& file, const std::string& key)
{
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    In in{ f };
    if (in.S() != key || !in.ok) return false;
    MovesGraph g;
    g.cls = in.S(); g.skeletonName = in.S(); g.gestures = in.S();
    g.gestureStates = in.U();
    g.states.resize(in.ok ? std::min<uint32_t>(in.U(), 1u << 20) : 0);
    for (auto& s : g.states)
    {
        s.name = in.S(); s.file = in.S(); s.path = in.P(); s.fileExists = in.B(); s.speed = in.F(); s.duration = in.F(); s.looped = in.B();
        s.interpolationSpeed = in.F(); s.interpolationRestart = int(in.U()); s.actions = in.S(); s.equivalentTo = in.S();
        s.variantsPlayer = in.Pairs(); s.variantsAI = in.Pairs();
        s.edges.resize(in.ok ? std::min<uint32_t>(in.U(), 1u << 16) : 0);
        for (auto& e : s.edges) { e.to = int(in.U()); e.cost = in.F(); e.interpolate = in.B(); }
        s.connectRaw = in.Pairs(); s.interpolateRaw = in.Pairs(); s.base = in.S(); s.badLinks = in.Strings();
        s.cfgFile = int(in.U()); s.cfgLine = int(in.U()); s.abstract = in.B(); s.terminal = in.B();
        s.mask = in.S(); s.aiming = in.S(); s.aimingBody = in.S(); s.leftHandIKCurve = in.Floats(); s.rightHandIKCurve = in.Floats();
        if (!in.ok) return false;
    }
    for (uint32_t i = 0, n = in.ok ? in.U() : 0; i < n && in.ok; ++i) { std::string k = in.S(); g.actions[k] = in.Map(); }
    g.actionNames = in.Map();
    g.upDegree = in.Map();
    for (uint32_t i = 0, n = in.ok ? in.U() : 0; i < n && in.ok; ++i)
    {
        std::string k = in.S();
        for (auto& x : in.Strings()) g.noAction[k].insert(x);
    }
    g.actionsOrder = in.Strings();
    for (uint32_t i = 0, n = in.ok ? in.U() : 0; i < n && in.ok; ++i) g.configFiles.push_back(in.P());
    g.badActions = in.Strings();
    if (!in.ok) return false;
    for (const auto& e : g.states)
        for (const auto& x : e.edges)
            if (x.to < 0 || x.to >= int(g.states.size())) return false;
    // an RTM may have appeared / gone since: the existence is checked again (cheap), the rest is the config's
    std::error_code ec;
    for (auto& s : g.states)
        if (!s.abstract) s.fileExists = fs::exists(s.path, ec);
    for (int i = 0; i < int(g.states.size()); ++i) g.index_[Lower(g.states[size_t(i)].name)] = i;
    *this = std::move(g);
    return true;
}
