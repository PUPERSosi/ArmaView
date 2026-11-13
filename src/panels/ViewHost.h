#pragma once
// What a panel may ask of the viewer around it (implemented by App): the narrow interface panels get in their
// constructor instead of reaching into App. The document itself is the Session; this is only the view side.
#include <glm/glm.hpp>

#include <filesystem>
#include <vector>

// How the animation is shown (view settings shared by the Timeline, Layers and Inspector windows; owned by App).
struct AnimViewOptions
{
    bool onionSkin = false;     // previous / next frame of the primary layer as ghosts
    int onionStep = 1;          // frames between them
    bool showTrail = false;     // path of the selected bone over one cycle
    bool skinNormalize = true;  // skinning: weights scaled to sum 1
    bool skinLimit4 = true;     // skinning: strongest 4 bones per point
    bool reloadOnion = false;   // Reload sync: the weapon and the character before / after the current time
    float reloadOnionDt = 0.15f;
};

// The scene rectangle of the window and its camera transform (implemented by App).
class IViewport
{
public:
    virtual ~IViewport() = default;
    virtual void SceneRect(float& x, float& y, float& w, float& h) const = 0;
    virtual glm::mat4 ViewProj() const = 0;
};

class IViewHost
{
public:
    virtual ~IViewHost() = default;
    virtual float Dpi() const = 0;
    virtual void RebuildGeometry() = 0;      // LOD geometry + overlays (after visibility / proxies / crew changes)
    virtual void RebuildDrawSections() = 0;  // which sections are drawn (hidden parts, hide vehicle)
    virtual void PoseChanged() = 0;          // recompute the pose this frame
    virtual void WatchedFilesChanged() = 0;  // the hot reload list follows newly loaded files
    virtual void InspectSeat() = 0;          // the Inspector shows the selected crew seat
    virtual void OpenInEditor(const std::filesystem::path& file, int line) const = 0;
    virtual void PickBone(int bone, bool focusCamera = false) = 0; // select (and highlight) a skeleton bone
    virtual int SelectedBone() const = 0;      // -1: none
    virtual void LodChanged() = 0;             // everything that depends on the current LOD (axes, sections, crew)
    virtual void ShowMode(int mode, const char* panel) = 0; // switch the UI mode, bring a docked panel to the front
    virtual const std::vector<int>& BoneSelection() const = 0; // per bone: its selection in the current LOD (-1 none)
};
