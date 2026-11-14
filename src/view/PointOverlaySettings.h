#pragma once
// What the point overlay shows (named-point labels, Hitpoints spheres) - set by any UI without ImGui widgets, and stored
// in the settings file through the same structure. No ImGui.
#include <string>
#include <utility>
#include <vector>

struct PointOverlaySettings
{
    bool labels = true;         // named-point labels
    bool onMeshLods = false;    // labels also on LODs with faces (resolution LODs: many selections per point)
    bool unnamedDots = true;    // points in no named selection as dots
    bool hitpointsView = false; // CfgVehicles >> HitPoints spheres
    bool occludeByMesh = false; // points behind the mesh (depth buffer of the previous frame) dimmed ...
    bool occludedHide = false;  // ... or hidden
    std::string filter;         // only labels whose name contains this (case-insensitive); "" all

    bool operator==(const PointOverlaySettings&) const = default;
    // the "points" block of the settings file
    std::vector<std::pair<std::string, std::string>> ToPairs() const;
    void FromPairs(const std::vector<std::pair<std::string, std::string>>& pairs); // unknown keys ignored, missing ones kept
};
