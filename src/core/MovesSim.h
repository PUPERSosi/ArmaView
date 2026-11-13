#pragma once
// Game mode of CfgMoves: the state machine (MovesPlayer: playMove with path search, switchMove, blends) drives the
// Session's AnimStack; the start state as the engine picks it (or the one picked for the model), the transition log and
// the RTM cache of the states. No view: the caller turns the results into pose / graph updates.
#include "Moves.h"
#include "RTM.h"
#include "Session.h"

#include <glm/glm.hpp>

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

class MovesSim
{
public:
    struct LogEntry { double time; std::string action, from, path, kinds; float blend; }; // kinds: C / I per step, "switch"

    explicit MovesSim(Session& session) : s_(session) {}

    MovesPlayer player;
    bool drive = false;                            // game mode on: the player owns session.anim
    std::string msg;                               // last result for the Moves panel
    std::string startWhy;                          // how the start state was chosen
    std::map<std::string, std::string> startPick;  // model path (lower) -> start state picked in the Moves panel
    std::vector<LogEntry> log;                     // transitions (newest last, at most 500)

    // the start state: the one picked for this model, else as the engine (Default action of the unit's actions class)
    int StartState(std::string* why) const;
    // RTM of a state (loaded once; null when missing / unreadable)
    std::shared_ptr<RtmAnimation> Rtm(int state);
    // hot reload: every cached state RTM loaded from this file is replaced by the new version
    void Replace(const std::filesystem::path& file, const std::shared_ptr<RtmAnimation>& a);
    // game mode on: the user's layers are kept aside, the player starts at the start state if it has none.
    // startedAt: the state it started in (-2: it already had one / turned off)
    void SetDrive(bool on, double time, int* startedAt);
    // the player's states as animation layers at time; -1: not driving, 0: weights / phases only, 1: layers rebuilt
    int Update(double time);
    // back to the start state (the caller has the game mode on); returns the state or -1
    int Reset(double time);
    // an action or state name from the current state: the target or -1 (msg says why). user: an explicit command
    // (death / unconscious states only then)
    int Resolve(const std::string& text, bool switchMove, bool user);
    // plays the resolved target (playMove along the cheapest path, or switchMove) and logs it
    void Go(const std::string& text, int target, bool switchMove, double time);

private:
    Session& s_;
    std::vector<AnimLayer> saved_; // the user's layers while the game mode runs
    std::map<std::string, std::shared_ptr<RtmAnimation>> rtm_;
};

// Control mode: keys -> CfgMoves actions like the game (the most specific held combination wins; C / Z toggle the stance
// by the current state), turning, root motion from the RTM step of the playing states, footsteps. Input comes as a
// KeyState (the UI reads the keyboard); the action to play goes back to the caller (it plays it through MovesSim).
class MovesControl
{
public:
    struct KeyBind { std::string keys, action; }; // "Shift+W" -> "FastF"
    struct KeyState
    {
        bool shift = false, ctrl = false;
        std::array<bool, 26> letter{}; // A..Z held
    };

    bool on = false;             // control mode: the keys drive the character
    std::vector<KeyBind> keymap;
    std::string action;          // the key action being held ("" none)
    bool moving = false;         // a walk / run key is held (release -> Stop)
    glm::vec3 rootPos{ 0.0f };   // character position on the ground
    float rootYaw = 0.0f;        // degrees
    std::vector<glm::vec3> footsteps;

    void DefaultKeymap();
    void ResetRoot() { rootPos = glm::vec3(0); rootYaw = 0; footsteps.clear(); action.clear(); moving = false; }
    // the action to play now ("" none): a newly held binding, or Stop when the walk keys were released
    std::string KeyAction(const KeyState& keys, const MovesGraph& moves, int currentState);
    // turning (Q / E as left / right) and root motion over dt from the RTM step of the playing states
    void Move(MovesSim& sim, const MovesGraph& moves, double time, float dt, float speed, bool turnLeft, bool turnRight);
    glm::mat4 RootMatrix() const;
    // feet near the ground that stopped moving become footsteps; true when one was added
    bool AddFootsteps(const std::vector<glm::mat4>& bones, const Skeleton& skel);
};
