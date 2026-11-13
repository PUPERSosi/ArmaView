#pragma once
// Reload sync window (weapon mode A): the config chain of the weapon's reload, the timing model, the reload transport,
// one timeline for the gesture and the model.cfg windows of the reload source (edges drag), a comparison weapon, saving
// the windows, the touch helper. The logic is core/ReloadSync; this window shows it and sends commands.
#include "ReloadSync.h"
#include "Session.h"
#include "ViewHost.h"
#include "WeaponView.h"

#include <string>

class ReloadPanel
{
public:
    ReloadPanel(Session& session, ReloadSync& reload, AnimViewOptions& options, IViewHost& host, IWeaponView& view)
        : s_(session), rs_(reload), opt_(options), host_(host), view_(view) {}
    void Draw(const std::string& windowName);

private:
    void DrawConfigChain();
    void DrawTiming();
    void DrawTransport();
    void DrawTimeline();
    void DrawSave();
    void DrawTouchHelper();
    void SetWindow(int anim, float mn, float mx);

    Session& s_;
    ReloadSync& rs_;
    AnimViewOptions& opt_;
    IViewHost& host_;
    IWeaponView& view_;
    char cmpInput_[160] = {};
    bool showWindowSave_ = false;
};
