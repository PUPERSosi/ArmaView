#pragma once
// Model checks without UI: per-LOD skinning validation in the log, the status of each model.cfg animation, and the
// summary the selftest / CLI read ("RESULT ...: n error(s)").
#include "Skeleton.h"

#include <string>
#include <vector>

class Session;

enum class Severity { Ok, Inactive, Warning, Error };

struct AnimStatus
{
    Severity severity = Severity::Ok;
    std::string message;
};

// "a, b, c ... (+n more)"
std::string JoinList(const std::vector<std::string>& v, size_t max = 12);

// One LOD's skinning report into the log (info line + warnings)
void LogLodValidation(const Skeleton& skel, int lodIndex, const P3DLod& lod, const ValidationReport& r);
// Every visual / view LOD of the open model against its skeleton, into the log (once per load)
void LogAllLodValidation(const Session& s);

// A model.cfg animation: its own problem, axis problems, or inactive when its bone has no points in the current LOD.
// boneSelection: per bone, the selection index in the current LOD (-1 none).
AnimStatus AnimStatusOf(const CfgAnimation& a, const std::vector<int>& boneSelection, int lod);

struct ModelChecks
{
    int errors = 0, warnings = 0;
    std::string text; // one line per part, "ERROR" lines for the problems
};
// Skeleton / current LOD weights / model.cfg animations / crew seats of the open model.
ModelChecks CheckModel(const Session& s, const ValidationReport& currentLod, const std::vector<int>& boneSelection);

// Per-face shading diagnostics of a LOD (orientation = majority of the LOD, minority = suspicious).
struct FaceDiagInfo
{
    std::vector<uint8_t> flippedCorners; // corners whose normal points against the face (count 0..4)
    std::vector<uint8_t> mirrored;       // UV handedness opposite to the LOD majority (mirrored UV island)
    std::vector<uint8_t> degenerateUV;   // zero UV area: the tangent frame is undefined
    int windingSign = 1;                 // +1: normals follow cross(p1-p0, p2-p0), -1: opposite
};
FaceDiagInfo AnalyzeFaces(const P3DLod& lod);
