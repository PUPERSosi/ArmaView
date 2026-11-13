#pragma once
// Vehicle & Crew: the crew window (seats of the vehicle class, their actions / states / RTMs, playback, display options)
// and the seat Inspector (manual RTM, speed, get in / out chains).
#include "Session.h"
#include "ViewHost.h"

#include <string>

class CrewPanel
{
public:
    CrewPanel(Session& session, IViewHost& host) : session_(session), host_(host) {}
    void Draw(const std::string& windowName);
    void DrawSeatInspector();
    // after a change: resolve the crew again (actions, states, RTMs) and / or rebuild the geometry
    void Apply(bool resolve, bool rebuild);

    int selected = -1;        // seat shown in the Inspector
    bool hideVehicle = false; // crew check: draw only proxies / crew

private:
    Session& session_;
    IViewHost& host_;
    char modelEdit_[260] = {};
    char rtmEdit_[260] = {};
    int editFor_ = -1; // seat rtmEdit_ was filled for
};
