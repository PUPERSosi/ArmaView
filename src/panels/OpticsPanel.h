#pragma once
// Optics window (weapon mode B): items on the weapon's slots, the sight modes and zoom, the view through the sight or
// from the character's eye, the sight numbers, a temporary fit with the correction to make in the P3D.
// The logic is core/OpticsFit; this window shows it and sends commands.
#include "ReloadSync.h"
#include "Session.h"
#include "ViewHost.h"
#include "WeaponView.h"

#include <string>

class OpticsPanel
{
public:
    OpticsPanel(Session& session, OpticsFit& optics, IViewHost& host, IWeaponView& view) : s_(session), fit_(optics), host_(host), view_(view) {}
    void Draw(const std::string& windowName);

private:
    void DrawSlots();
    void DrawSight();
    void DrawFit();

    Session& s_;
    OpticsFit& fit_;
    IViewHost& host_;
    IWeaponView& view_;
};
