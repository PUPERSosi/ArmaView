#pragma once
// Scene overlay for points: named-point labels (Memory LOD etc.), Hitpoints spheres from config.cpp, click-to-inspect.
// Labels are drawn only for points inside the scene rectangle, greedily placed without overlapping each other.
#include "ArmaConfig.h"
#include "P3D.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <functional>
#include <string>
#include <vector>

// One class of CfgVehicles >> <vehicle> >> HitPoints (or of a turret's HitPoints).
struct HitPointDef
{
    std::string cls;       // HitHull
    std::string turret;    // "" = hull, else turret class path (MainTurret/CommanderOptics)
    std::string selection; // name= : selection in the Hit-points LOD
    float radius = -1.0f, armor = -1.0f; // -1 = not set (or inherited from a game class that is not on disk)
    std::string where;     // file(line)
    bool unresolved = false; // "class HitX;" whose definition is in game configs that are not on disk
    bool unlinked = false;   // a Hit-points LOD selection no HitPoints class refers to (cls is empty)
};

// Finds the CfgVehicles class using modelPath (own model= entry preferred) and collects its hitpoints and the
// hitpoints of all its turrets (inherited classes merged). vehicleClass receives the class name ("" if none).
std::vector<HitPointDef> FindHitPoints(const ArmaConfig& cfg, const std::string& modelPath, std::string& vehicleClass);

class PointOverlay
{
public:
    struct View
    {
        glm::mat4 viewProj;
        glm::vec3 cameraRight;
        float x = 0, y = 0, w = 1, h = 1; // scene rectangle, screen pixels
        float dpi = 1.0f;
        std::function<float(float, float)> depthAt; // scene depth at a screen position (null = no occlusion test)
    };
    bool WantsDepth() const { return occlude_ && (labels_ || hitView_); }

    void Invalidate() { cacheLod_ = nullptr; picked_ = -1; pickedHit_ = -1; }
    // Hitpoint definitions for the loaded model; logs hitpoints whose selection is missing in the Hit-points LOD.
    void SetHitPoints(std::vector<HitPointDef> defs, const std::string& vehicleClass, const P3DModel& model);

    void DrawControls(const P3DLod* lod); // View panel section
    // pointMats: posed matrices for the points of `lod` (empty = rest pose).
    void Draw(ImDrawList* dl, const View& v, const P3DModel& model, int lod, const std::vector<glm::mat4>& pointMats);
    // Left click without drag on the scene: pick the nearest drawn point / hit sphere. True if something was picked.
    bool Click(float x, float y, const P3DModel& model, int lod);
    bool HasPick() const { return picked_ >= 0 || pickedHit_ >= 0; }
    std::string PickInfo(const P3DModel& model, int lod) const { return Info(model, lod); } // Inspector
    // Screen position of a named point in the last drawn frame (exact name first, then substring).
    bool PointScreenPos(const std::string& name, float& x, float& y) const;

private:
    struct Drawn
    {
        int point = -1;   // point index in its LOD
        int hit = -1;     // hitpoint definition index (Hitpoints view)
        ImVec2 s;         // screen position
        float r = 0;      // hit sphere screen radius
    };
    void EnsureNames(const P3DLod& lod);
    std::string Info(const P3DModel& model, int lod) const;

    bool labels_ = true;
    bool onMeshLods_ = false; // labels also on LODs with faces (resolution LODs: many selections per point)
    bool unnamedDots_ = true;
    bool hitView_ = false;
    bool occlude_ = false;      // points behind the mesh (depth buffer of the previous frame)
    bool occludedHide_ = false; // hide them instead of dimming
    char filter_[96] = {};

    // per-point selection names of the current LOD
    const P3DLod* cacheLod_ = nullptr;
    size_t cachePoints_ = 0;
    std::vector<std::string> names_;            // joined, for display
    std::vector<std::vector<int>> pointSel_;    // selection indices per point

    std::vector<HitPointDef> hit_;
    std::string vehicleClass_;
    int hitLod_ = -1;
    std::vector<std::vector<int>> hitPoints_;   // per definition: point indices in the Hit-points LOD

    std::vector<Drawn> drawn_; // last frame
    int labelsShown_ = 0, labelsHidden_ = 0;
    int picked_ = -1, pickedHit_ = -1, pickedLod_ = -1;
    glm::vec3 pickedPos_{ 0 };
};
