#pragma once
// Quick retargeting for checks: an RTM of one skeleton onto another (model.cfg skeletons, pivots as bind pose).
// Per target bone: the world rotation of the mapped source bone times a bind-pose correction (rotation taking the
// target bone's bind direction to the source bone's, i.e. compensation of different rest poses such as A / T pose);
// a root (pelvis) fits all its branches at once. Name matches count only with a matching hierarchy; unmapped bones keep
// their bind pose under the parent. Pelvis translation scaled by the leg length ratio; optional two-bone foot IK, with the
// pelvis lowered where a foot goal is out of the leg's reach.
// Export: RTM_0101 into a NEW file only, re-read and compared with the preview.
#include "RTM.h"
#include "Skeleton.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct RetargetRig
{
    Skeleton skel;
    std::filesystem::path model, modelCfg;
    bool ok = false;
    std::string info; // or the error
};
// Skeleton of a character model: model.cfg next to it / above it, pivotsModel P3D for exact joints (else centroids).
bool LoadRetargetRig(const std::filesystem::path& p3d, const std::filesystem::path& projectRoot, RetargetRig& out);

struct BoneMapping
{
    std::vector<int> srcOf;      // per target bone: source bone or -1
    std::vector<uint8_t> manual; // per target bone: set by hand
};
BoneMapping AutoMapBones(const Skeleton& src, const Skeleton& tgt); // same name, case-insensitive
// Preset file of a skeleton pair: "target = source" per line ("target = -" unmapped). Missing names are skipped.
bool SaveMappingPreset(const std::filesystem::path& file, const Skeleton& src, const Skeleton& tgt, const BoneMapping& m);
bool LoadMappingPreset(const std::filesystem::path& file, const Skeleton& src, const Skeleton& tgt, BoneMapping& m);

struct RetargetOptions
{
    bool scalePelvis = true; // pelvis translation x (target leg length / source leg length)
    bool footIK = false;     // feet reach the (scaled) source foot positions
};

struct RetargetResult
{
    std::shared_ptr<RtmAnimation> anim;   // model space over the target skeleton bones
    std::vector<std::vector<glm::vec3>> sourceJoints; // per frame: posed source pivots (preview)
    float scale = 1.0f;
    int mapped = 0;
    std::string legInfo;
    std::string debug; // frame 0 joints (diagnostics)
    std::string report;
    std::string notes;  // "; ..." remarks appended to the report
};
// src: model-space animation (RTM_0101) whose bone names are source bones.
RetargetResult RetargetAnimation(const RtmAnimation& src, const Skeleton& srcSkel, const Skeleton& tgtSkel, const BoneMapping& map,
                                 const RetargetOptions& opt);

// RTM_0101 writer. Refuses to overwrite an existing file.
bool WriteRtm0101(const RtmAnimation& a, const std::filesystem::path& file, std::string& error);
// Max difference between two animations over all frames / bones: translation (m) and matrix element.
void CompareAnimations(const RtmAnimation& a, const RtmAnimation& b, float& maxPos, float& maxElem, std::string& note);
