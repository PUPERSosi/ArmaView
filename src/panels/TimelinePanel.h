#pragma once
// Timeline window: transport, the timeline bar of the primary layer (scrub, frame ticks, events), onion skin / bone
// trail / A/B compare, a CfgMovesMaleSdr state as a layer, the layer list, adding RTMs, missing-bone and skinning
// options. The player is the core Playback; this window only shows and sets it.
#include "Playback.h"
#include "RTM.h"
#include "Session.h"
#include "ViewHost.h"

#include <string>
#include <vector>

// What the Timeline asks of the viewer (implemented by App).
class ITimelineView
{
public:
    virtual ~ITimelineView() = default;
    virtual const std::vector<RtmBoneCheck>& RtmChecks() const = 0; // the RTM folder checked against the skeleton
    virtual void ShowOpenDialog() = 0;                              // open a .p3d / .rtm
};

class TimelinePanel
{
public:
    TimelinePanel(Session& session, Playback& playback, AnimViewOptions& options, IViewHost& host, ITimelineView& view)
        : s_(session), pb_(playback), opt_(options), host_(host), view_(view) {}
    void Draw(const std::string& windowName);

private:
    void DrawTimelineBar();
    void DrawTools();
    void DrawState();
    void DrawLayers();
    void DrawAdvanced();

    Session& s_;
    Playback& pb_;
    AnimViewOptions& opt_;
    IViewHost& host_;
    ITimelineView& view_;
    char stateEdit_[128] = {};
    int addRtmIndex_ = 0;
};
