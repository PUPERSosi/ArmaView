#pragma once
// Retarget window: source rig, source animation (file or CfgMoves state), options, preview, export, bone mapping.
#include "RetargetJob.h"
#include "Session.h"
#include "ViewHost.h"

#include <functional>
#include <string>

class RetargetPanel
{
public:
    // run / stop: the viewer's side of the preview (game mode off, time, playing; the source skeleton lines)
    RetargetPanel(Session& session, RetargetJob& job, IViewHost& host, std::function<bool()> run, std::function<void()> stop)
        : session_(session), job_(job), host_(host), run_(std::move(run)), stop_(std::move(stop)) {}
    void Draw(const std::string& windowName, const std::string& timelineName);

private:
    Session& session_;
    RetargetJob& job_;
    IViewHost& host_;
    std::function<bool()> run_;
    std::function<void()> stop_;
};
