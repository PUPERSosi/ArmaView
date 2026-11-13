#pragma once
// Closing with edits that exist only in the viewer (move config, RVMATs, model.cfg windows, weapon config.cpp, the
// layers project): the list, and a modal "close without saving / cancel".
#include "LayersJob.h"
#include "MaterialSet.h"
#include "MovesEditor.h"
#include "ReloadSync.h"
#include "Session.h"
#include "WeaponScene.h"

#include <functional>
#include <string>
#include <vector>

class ClosePrompt
{
public:
    ClosePrompt(Session& session, MaterialSet& mats, ReloadSync& reload, WeaponScene& weapon, LayersJob& layers, MovesEditor& moves,
                std::function<void()> closeWindow)
        : s_(session), mats_(mats), reload_(reload), weapon_(weapon), layers_(layers), moves_(moves), close_(std::move(closeWindow)) {}
    std::vector<std::string> UnsavedEdits() const;
    bool AllowClose(bool automation); // false: the prompt opens
    void Draw();
    void Show() { show_ = true; } // the dialog closing would show (ui-script screenshot)

private:
    Session& s_;
    MaterialSet& mats_;
    ReloadSync& reload_;
    WeaponScene& weapon_;
    LayersJob& layers_;
    MovesEditor& moves_;
    std::function<void()> close_;
    bool show_ = false, force_ = false;
};
