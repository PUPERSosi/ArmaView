#pragma once
// The CfgMoves graph of the open model, loaded without blocking the UI: the move config of its skeleton (or the one
// picked) is built on a worker, or read from a disk cache keyed by the config files; a model without a move config of
// its own (weapon, vehicle, prop) gets the game's man config only when something needs it (Ensure). The applied graph
// keeps the player's state across a reload, and brings its gestures (CfgGestures) and BlendAnims masks. The CfgMoves
// validation runs on a worker too.
#include "Moves.h"
#include "MovesSim.h"
#include "Session.h"

#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>

class MovesLoader
{
public:
    MovesLoader(Session& session, MovesSim& sim) : s_(session), sim_(sim) {}
    void SetCacheDir(std::filesystem::path dir) { cacheDir_ = std::move(dir); }

    std::vector<std::string> classes; // move configs in the configs
    std::string chosen;               // picked in the Moves window; empty = by the model's skeleton
    MovesValidation validation;
    bool seams = false;               // the validation reads every looped RTM for its loop seam

    void Rebuild();                   // classes, the config of the skeleton (or lazy), a background build
    bool Poll(bool wait);             // a finished build applied (player kept, gestures + masks); true: new graph
    bool Ensure(bool wait);           // the lazy build started; wait: until applied. true: a graph is there
    bool Building() const { return job_.valid(); }
    const std::string& BuildClass() const { return jobCls_; }

    void StartValidation(bool sync);
    bool PollValidation();            // true: a finished validation applied (logged)
    bool Validating() const { return valJob_.valid(); }

private:
    void StartBuild(const std::string& cls);
    void RebuildGestures();

    Session& s_;
    MovesSim& sim_;
    std::filesystem::path cacheDir_;
    std::future<std::shared_ptr<MovesGraph>> job_;
    std::future<MovesValidation> valJob_;
    std::string jobCls_, lazyCls_, keepCur_;
    double keepStart_ = 0;
};

// Gestures (CfgGestures states) as an override layer over the moves, masked by their BlendAnims mask, faded in / out;
// the aiming approximation. Publishes AnimationChanged when the layers change.
class GesturePlayer
{
public:
    GesturePlayer(Session& session, MovesSim& sim) : s_(session), sim_(sim) {}

    int current = -1;          // playing gesture state (Session::gestures)
    double start = 0;          // its start on the Playback time
    bool aimApprox = false;    // aiming / aimingBody masks pitch / yaw the bones (approximation)
    float aimYaw = 0.0f, aimPitch = 0.0f;

    bool Play(const std::string& name, double time); // sim.msg tells what happened
    void Stop();
    bool Update(double time);  // offset + fade of the playing gesture; true: the pose changed
    std::vector<float> MaskWeights(const std::string& mask) const; // per skeleton bone
    void ApplyAim(std::vector<glm::mat4>& bones) const;
    const AnimLayer* Layer() const; // the gesture layer (tag 1) or null

private:
    Session& s_;
    MovesSim& sim_;
};
