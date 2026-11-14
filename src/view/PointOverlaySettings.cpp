#include "PointOverlaySettings.h"

#include "ViewCoreGuard.h"

std::vector<std::pair<std::string, std::string>> PointOverlaySettings::ToPairs() const
{
    auto b = [](bool v) { return std::string(v ? "1" : "0"); };
    return { { "Labels", b(labels) },           { "OnMeshLods", b(onMeshLods) },       { "UnnamedDots", b(unnamedDots) },
             { "HitpointsView", b(hitpointsView) }, { "OccludeByMesh", b(occludeByMesh) }, { "OccludedHide", b(occludedHide) },
             { "Filter", filter } };
}

void PointOverlaySettings::FromPairs(const std::vector<std::pair<std::string, std::string>>& pairs)
{
    for (const auto& [k, v] : pairs)
    {
        const bool on = v == "1";
        if (k == "Labels") labels = on;
        else if (k == "OnMeshLods") onMeshLods = on;
        else if (k == "UnnamedDots") unnamedDots = on;
        else if (k == "HitpointsView") hitpointsView = on;
        else if (k == "OccludeByMesh") occludeByMesh = on;
        else if (k == "OccludedHide") occludedHide = on;
        else if (k == "Filter") filter = v;
    }
}
