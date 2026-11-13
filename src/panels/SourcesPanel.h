#pragma once
// Sources: the model.cfg animation sources as sliders (with oscillation) and the list of animations with their status.
#include "Session.h"
#include "ViewHost.h"

#include <string>

class SourcesPanel
{
public:
    SourcesPanel(Session& session, IViewHost& host) : session_(session), host_(host) {}
    void Draw(const std::string& windowName);

    float oscillatePeriod = 3.0f;    // seconds per min -> max -> min of an oscillating source
    bool memoryAxisFallback = true;  // memory=0 axes missing in the LOD: take them from the Memory LOD (flagged)

private:
    Session& session_;
    IViewHost& host_;
};
