#pragma once
// The View and Light popups of the toolbar: shading, wireframe, face diagnostics, overlays, points, normals, culling,
// the camera numbers; sun / headlight, presets, CfgWorlds lighting, time of day, exposure, shadows, background.
#include "Camera.h"
#include "DisplaySettings.h"
#include "Lighting.h"
#include "PointOverlay.h"
#include "Session.h"
#include "ViewHost.h"
#include "WorldLight.h"

#include <string>
#include <vector>

class DisplayPanel
{
public:
    DisplayPanel(Session& session, DisplaySettings& display, Lighting& light, const LightingResult& lit, WorldLighting& world,
                 const std::vector<std::string>& worlds, const OrbitCamera& camera, PointOverlay& points, IViewHost& host)
        : s_(session), d_(display), light_(light), lit_(lit), world_(world), worlds_(worlds), camera_(camera), points_(points), host_(host) {}
    void DrawViewOptions();
    void DrawLightOptions();
    void SetWorld(const std::string& name); // CfgWorlds >> name lighting ("" / "procedural": the built-in sky)

private:
    Session& s_;
    DisplaySettings& d_;
    Lighting& light_;
    const LightingResult& lit_;
    WorldLighting& world_;
    const std::vector<std::string>& worlds_;
    const OrbitCamera& camera_;
    PointOverlay& points_;
    IViewHost& host_;
};
