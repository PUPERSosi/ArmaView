#pragma once
// The animation player of the open document: time, play / pause, speed, the default fps of new layers, rest pose,
// frame steps and scrubbing on the primary layer, the A/B compare (one layer moved to a second stack) and the loop
// seam of the primary layer. The Timeline window only shows and sets these.
#include "Attachments.h"
#include "RTM.h"
#include "Session.h"

#include <filesystem>
#include <string>
#include <vector>

class Playback
{
public:
    // a new / reloaded model: the compare layer (B) is bound to its skeleton too (the Session rebinds the layers it owns)
    explicit Playback(Session& session) : s_(session)
    {
        sub_ = s_.Events().Subscribe([this](SessionEvent e) {
            if (e == SessionEvent::ModelLoaded && !compare.layers.empty()) compare.Bind(s_.skeleton);
        });
    }
    ~Playback() { s_.Events().Unsubscribe(sub_); }
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;

    double time = 0.0;      // seconds
    bool playing = false;
    bool restPose = false;  // bind pose (layers off)
    float speed = 1.0f;     // global speed factor
    float fps = 30.0f;      // a new layer's duration = (frames - 1) / fps
    AnimStack compare;      // B of the A/B compare (one layer)
    int compareMode = 0;    // 0 off, 1 overlay, 2 side by side
    std::vector<SeamJump> seam;            // loop seam of the primary layer
    const RtmAnimation* seamAnim = nullptr; // the animation seam belongs to
    MovesStateInfo state;   // the CfgMovesMaleSdr state loaded last (LoadState)

    int PrimaryLayer() const;                                   // first enabled layer with an animation, -1 none
    double TimeOfPhase(const AnimLayer& l, float phase) const;  // the time showing phase of l
    void StepFrame(int dir);                                    // one frame of the primary layer (pauses)
    void ScrubTo(float phase);                                  // the primary layer at phase 0..1 (pauses)
    bool MoveToCompare(int layer);                              // a layer becomes B (the previous B goes back)
    bool ReturnCompare();                                       // B back to the layers
    bool UpdateSeam();                                          // seam of the primary layer when it changed (logs); true: new
    // An RTM as a new last layer (duration from fps; the first layer starts playing at 0). Logs; publishes LayerAdded.
    bool AddLayer(const std::filesystem::path& rtm);
    // An animation already in memory (a glTF clip) as a new last layer, duration in seconds per cycle.
    bool AddLayer(std::shared_ptr<RtmAnimation> anim, float duration);
    // CfgMovesMaleSdr >> States >> name: its RTM as a layer with the config speed and looped flag.
    bool LoadState(const std::string& name);

private:
    Session& s_;
    int sub_ = 0;
};
