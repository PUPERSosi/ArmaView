#pragma once
// 2D marks over the scene (ImGui background draw list): the axis gizmo with the sun / moon direction, named points and
// hit spheres, labels of proxy / crew stubs, the selected bone's name.
#include "Camera.h"
#include "DisplaySettings.h"
#include "Lighting.h"
#include "PointOverlay.h"
#include "Renderer.h"
#include "SceneView.h"
#include "Session.h"
#include "ViewHost.h"

class SceneOverlay
{
public:
    SceneOverlay(Session& session, SceneView& scene, const DisplaySettings& display, const Lighting& light, const LightingResult& lit,
                 const OrbitCamera& camera, PointOverlay& points, Renderer& renderer, const IViewport& viewport, IViewHost& host, const bool& crewWindow)
        : s_(session), scene_(scene), display_(display), light_(light), lit_(lit), camera_(camera), points_(points), renderer_(renderer),
          viewport_(viewport), host_(host), crewWindow_(crewWindow) {}
    void Draw();

private:
    Session& s_;
    SceneView& scene_;
    const DisplaySettings& display_;
    const Lighting& light_;
    const LightingResult& lit_;
    const OrbitCamera& camera_;
    PointOverlay& points_;
    Renderer& renderer_;
    const IViewport& viewport_;
    IViewHost& host_;
    const bool& crewWindow_; // stub names of binarized / missing proxies only while the Crew window is open
};
