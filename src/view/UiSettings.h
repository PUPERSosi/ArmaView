#pragma once
// The viewer's own values in the "ui" block of the settings file: UI scale, language, first start seen, recording
// settings, the start state picked per model; the point overlay in the "points" block (PointOverlaySettings).
#include "MovesSim.h"
#include "PointOverlay.h"
#include "RecordPanel.h"
#include "Settings.h"
#include "UiShell.h"

class UiSettings
{
public:
    UiSettings(UiShell& shell, RecordSettings& record, MovesSim& moves, PointOverlay& points) : shell_(shell), record_(record), moves_(moves), points_(points) {}
    void Load(const Settings& settings);  // before the first frame
    void Store(Settings& settings) const;
    bool startedSeen = false;             // the first-run window was shown once
    bool startScreen = true;              // "What do you want to do?" at every start without a model

private:
    UiShell& shell_;
    RecordSettings& record_;
    MovesSim& moves_;
    PointOverlay& points_;
};
