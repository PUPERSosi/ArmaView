#pragma once
// Non-destructive animation project: a base clip (RTM, or a retarget result) + correction layers with masks +
// clip operations, stored as JSON next to the source and baked into a NEW RTM_0101.
//  - Offset layer: rotation (deg) / position offset of the masked bones, constant or 2-5 keys (smooth in between)
//  - Additive layer: another RTM's motion relative to its first frame, added
//  - Replace layer: another RTM replaces the pose (masked)
//  Mask: a BlendAnims mask of the move config or own bones with weights and a fall-off down the hierarchy.
//  Layer weight: constant or a curve over the phase.
//  Clip: trim, speed (cycle length), mirror L/R, loop seam fix (last N frames to the first), root alignment, foot IK locks.
#include "Moves.h"
#include "RTM.h"
#include "Skeleton.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct LayerKey
{
    float phase = 0.0f;
    glm::vec3 rotDeg{ 0.0f }, pos{ 0.0f };
};

struct CorrectionLayer
{
    enum class Type { Offset, Additive, Replace };
    Type type = Type::Offset;
    std::string name = "layer";
    bool enabled = true;
    std::vector<LayerKey> keys{ LayerKey{} };          // Offset
    std::string rtm;                                    // Additive / Replace
    std::string armaMask;                               // BlendAnims mask name (empty: own bones)
    std::vector<std::pair<std::string, float>> bones;   // own mask: bone, weight
    float falloff = 0.0f;                               // children of an own-mask bone get weight x falloff per level
    std::vector<std::pair<float, float>> weightCurve;   // (phase, weight); empty = 1
    float weight = 1.0f;
};

struct FootLock
{
    std::string foot = "LeftFoot";
    float from = 0.0f, to = 0.2f; // phases
};

struct ClipOps
{
    float trimStart = 0.0f, trimEnd = 1.0f;
    float speed = 1.0f;   // cycle length = source duration / speed (written as the suggested config speed)
    bool mirror = false;  // Left* <-> Right*, reflected across X
    int seamFrames = 0;   // last N frames blended into the first
    bool alignRoot = false; // pelvis keeps its frame-0 X / Z
    std::vector<FootLock> footLocks;
};

struct AnimProject
{
    std::string source;          // RTM path; "#retarget" = the current retarget result
    float duration = 1.0f;       // source cycle length (s)
    std::vector<CorrectionLayer> layers;
    ClipOps ops;
    std::string output;          // baked RTM
};

std::string AnimProjectText(const AnimProject& p); // the project file text (what SaveAnimProject writes)
bool SaveAnimProject(const std::filesystem::path& file, const AnimProject& p, std::string& error);
bool LoadAnimProject(const std::filesystem::path& file, AnimProject& p, std::string& error);

// Per skeleton bone weights of a layer's mask (BlendAnims masks from the move / gesture config).
std::vector<float> LayerMaskWeights(const CorrectionLayer& l, const Skeleton& skel, const std::map<std::string, BoneMask>& armaMasks);

using RtmLoader = std::function<std::shared_ptr<RtmAnimation>(const std::string& path)>; // model space over the skeleton
// base: model space over skel bones (AnimStack-bound or a retarget result). Returns the baked clip (model space).
std::shared_ptr<RtmAnimation> BakeAnimProject(const AnimProject& p, const RtmAnimation& base, const Skeleton& skel,
                                              const std::map<std::string, BoneMask>& armaMasks, const RtmLoader& load, std::string& report);
