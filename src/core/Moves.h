#pragma once
// CfgMoves state machine: states with inheritance (ConnectTo / InterpolateTo with costs, equivalentTo, speed, looped,
// interpolationSpeed, interpolationRestart, variants, file), Actions / ManActions, shortest paths (Dijkstra over
// the costs, as the engine picks a move path) and a player that walks such a path with the config's blends.
// Own implementation from the BI documentation (the CWR sources contain no move system).
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class ArmaConfig;
class CfgClass;
struct Skeleton;

struct MoveEdge
{
    int to = -1;
    float cost = 1.0f;
    bool interpolate = false; // InterpolateTo (blend now) vs ConnectTo (at the end of the cycle)
};

struct MoveState
{
    std::string name;
    std::string file;          // RTM as written
    std::filesystem::path path; // resolved on disk (empty if not found)
    bool fileExists = false;
    float speed = 1.0f;        // > 0 cycles per second, < 0 seconds per cycle
    float duration = 1.0f;     // seconds per cycle
    bool looped = true;
    float interpolationSpeed = 6.0f;
    int interpolationRestart = 0;
    std::string actions, equivalentTo;
    std::vector<std::pair<std::string, float>> variantsPlayer, variantsAI;
    std::vector<MoveEdge> edges;
    std::vector<std::pair<std::string, float>> connectRaw, interpolateRaw; // as written (inherited if not own)
    std::string base;          // base class name
    std::vector<std::string> badLinks; // ConnectTo / InterpolateTo targets that are no states
    int cfgFile = 0, cfgLine = 0;
    bool abstract = false;     // no file (base class only)
    bool terminal = false;     // terminal=1 (death): no way out by design
    std::string mask;          // gestures: BlendAnims mask
    std::string aiming, aimingBody;                     // BlendAnims masks of weapon aiming / body turning
    std::vector<float> leftHandIKCurve, rightHandIKCurve; // pairs (phase, weight)
};

struct MovesValidation
{
    struct Item { int state = -1; std::string text; bool error = true; bool info = false; }; // info: not a problem by itself
    std::vector<Item> items;
    int unreachable = 0, badLinks = 0, missingRtm = 0, wrongSkeleton = 0, seams = 0, badActions = 0, deadEnds = 0;
    bool done = false;
    std::string summary;
};

class MovesGraph
{
public:
    // cls: root class name (CfgMovesMaleSdr, CfgGesturesMale ...); projectRoot resolves RTM paths
    bool Build(const ArmaConfig& cfg, const std::string& cls, const std::filesystem::path& projectRoot);
    // the built graph on disk (the game's CfgMovesMaleSdr takes seconds to build): key from MovesCacheKey
    bool SaveCache(const std::filesystem::path& file, const std::string& key) const;
    bool LoadCache(const std::filesystem::path& file, const std::string& key); // false: missing / other key / damaged
    bool Empty() const { return states.empty(); }
    int Find(const std::string& name) const;
    // cheapest path from -> to (inclusive); empty if unreachable. cost: total.
    std::vector<int> Path(int from, int to, float* cost = nullptr) const;
    // state for an action of the given state's actions class (or a ManActions entry); -1 none
    int StateOfAction(int state, const std::string& action) const;
    // actions of a state's actions class that lead to a state: (action, state)
    std::vector<std::pair<std::string, int>> ActionsOf(int state) const;
    std::vector<int> Predecessors(int state) const;
    // death / unconscious state (terminal, a DefaultDie base, or a death-like name): never entered automatically
    bool IsDeathLike(int state) const;
    // Start state as the engine picks it (Man::GetDefaultMove, CWR SoldierOld.cpp:344 / SoldierOldSim.cpp:34): the Default
    // action of the actions class of the unit's upDegree (unarmed: ManPosNoWeapon, then ManPosStand, ManPosCombat). Then
    // the Default / Stop of any used actions class, then the first standing idle state. Never a death state, always with
    // an RTM on disk. why: how it was chosen (and what was skipped). -1: no state qualifies.
    int StartState(std::string& why) const;

    std::string cls, skeletonName, gestures;
    size_t gestureStates = 0; // states of the gesture config (gesture actions point there)
    std::vector<MoveState> states;
    std::map<std::string, std::map<std::string, std::string>> actions; // actions class (lower) -> action (lower) -> state name
    std::map<std::string, std::string> actionNames;                    // action (lower) -> as written
    std::map<std::string, std::string> upDegree;                       // actions class (lower) -> upDegree as written
    std::map<std::string, std::set<std::string>> noAction;             // actions class (lower) -> actions set to "" (no action, not inherited)
    std::vector<std::string> actionsOrder;                             // actions classes (lower) in the order the states first use them
    std::vector<std::filesystem::path> configFiles;                     // files the classes come from (hot reload)
    std::vector<std::string> badActions;                                // "Actions >> X >> a: state Y does not exist"

private:
    std::unordered_map<std::string, int> index_;
};

// BlendAnims masks of a move / gesture config: mask -> (lower-case bone -> weight). Aliases (mask = "other") resolved.
using BoneMask = std::map<std::string, float>;
// Gesture weight over its own time (approximation - no exact formula known): linear fade in over
// 1 / interpolationSpeed seconds of the gesture state, and out over the same time before its end (not looped).
float GestureFadeWeight(double t, float duration, float interpolationSpeed, bool looped);
std::map<std::string, BoneMask> ReadBlendAnims(const ArmaConfig& cfg, const std::string& cls);
// Piecewise linear curve of (phase, value) pairs at phase; empty -> -1.
float CurveAt(const std::vector<float>& curve, float phase);

// One state of a move / gesture config by name (values with inheritance; no links, no actions) - instead of building
// the whole graph when only a state is needed (the reload gesture of a weapon).
bool ReadMoveState(const ArmaConfig& cfg, const std::string& cls, const std::string& name, const std::filesystem::path& projectRoot, MoveState& out);
// what a cached graph depends on: the class, the project root and every config file's size and time
std::string MovesCacheKey(const ArmaConfig& cfg, const std::string& cls, const std::filesystem::path& projectRoot);

// Root classes that are move / gesture configs: have States and skeletonName.
std::vector<std::string> ListMovesClasses(const ArmaConfig& cfg);

// Validation: unreachable states, links to missing states, RTMs missing on disk, RTMs not for the skeleton,
// loop seams (looped states), actions to missing states. skel may be null (no bone check). Slow (reads RTMs).
MovesValidation ValidateMoves(const MovesGraph& g, const Skeleton* skel, bool checkSeams);

// Walks move paths like the engine: InterpolateTo switches at once with a blend of 1 / interpolationSpeed,
// ConnectTo waits for the end of the current cycle. Produces the current (and previous, blending) state + phase.
class MovesPlayer
{
public:
    struct Slot { int state = -1; double start = 0; float blend = 1.0f; };
    void Reset() { cur_ = prev_ = {}; queue_.clear(); edgeInterp_.clear(); }
    void SwitchMove(const MovesGraph& g, int state, double now);             // immediate, no path, no blend
    bool PlayMove(const MovesGraph& g, int target, double now, float* cost = nullptr); // shortest path from the current state
    void Update(const MovesGraph& g, double now);
    int Current() const { return cur_.state; }
    int Previous() const { return prev_.state; }
    float PhaseOf(const MovesGraph& g, const Slot& s, double now) const;
    const Slot& Cur() const { return cur_; }
    const Slot& Prev() const { return prev_; }
    float BlendWeight(const MovesGraph& g, double now) const; // weight of the current state (prev = 1 - this)
    const std::vector<int>& Queue() const { return queue_; }
    void KeepStart(double start) { cur_.start = start; } // hot reload: same phase
    std::vector<int> PathShown;                                   // last path (for the graph highlight)

private:
    void Enter(const MovesGraph& g, int state, double now, bool interpolate);
    double CycleEnd(const MovesGraph& g, double now) const;
    double waitUntil_ = -1;
    Slot cur_, prev_;
    double blendStart_ = 0;
    float blendTime_ = 0;
    std::vector<int> queue_;
    std::vector<bool> edgeInterp_; // per queued state: reached by InterpolateTo
};
