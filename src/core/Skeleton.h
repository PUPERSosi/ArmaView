#pragma once
// Skeleton from model.cfg (CfgSkeletons/CfgModels), bone pivots, rig validation and skinning data.
#include "ArmaConfig.h"
#include "P3D.h"

#include <glm/glm.hpp>
#include <filesystem>
#include <string>
#include <vector>

enum class PivotSource { None, PivotsModel, SelectionCentroid };

struct Bone
{
    std::string name, parentName;
    int parent = -1;
    std::vector<int> children;
    glm::vec3 pivot{ 0.0f };
    PivotSource pivotSource = PivotSource::None;
};

struct Skeleton
{
    std::string name;
    std::string inherit;     // skeletonInherit
    bool isDiscrete = false;
    std::string pivotsModel; // as written in model.cfg
    std::filesystem::path pivotsPath;
    std::vector<Bone> bones;

    int Find(std::string_view bone) const; // case-insensitive, -1 if absent
    bool Empty() const { return bones.empty(); }
    std::vector<int> ParentFirstOrder() const;
    int Depth(int bone) const;
    bool IsAncestor(int ancestor, int bone) const;
};

struct ModelCfgInfo
{
    std::string modelClass;   // CfgModels class used
    std::string skeletonName;
    std::vector<std::string> sections;
};

// Builds the skeleton for model `modelName` (P3D file stem). Problems are logged. Returns false if no skeleton.
bool BuildSkeleton(const ArmaConfig& cfg, const std::string& modelName, Skeleton& out, ModelCfgInfo& info);

// Pivot per bone: Memory LOD selection of the pivots model; else weight-averaged centroid of the bone's
// selection in `lod`; else none. pivots may be null.
void ResolvePivots(Skeleton& skel, const P3DModel* pivots, const P3DLod* lod);

// Index of the selection named like each bone in `lod` (-1 if absent or empty).
std::vector<int> MapBonesToSelections(const Skeleton& skel, const P3DLod& lod);

struct VertexIssue
{
    uint32_t point;
    int bones;
    float weightSum;
};

struct ValidationReport
{
    bool valid = false;                              // false = no skeleton/LOD
    std::vector<std::string> weightedNotInSkeleton;  // selections with partial weights not in the skeleton (likely bones)
    std::vector<std::string> otherNotInSkeleton;     // remaining non-bone selections (sections, proxies, helpers)
    std::vector<std::string> bonesWithoutSelection;  // skeleton bones with no (or empty) selection
    std::vector<VertexIssue> tooManyBones;           // > 4 bones
    std::vector<VertexIssue> badWeightSum;           // |sum - 1| > 0.01; discrete skeletons: points in unrelated bones
    bool discrete = false;                           // isDiscrete=1: >4/sum checks replaced by the unrelated-bones check
    std::vector<uint32_t> noBone;                    // points with no bone at all
    size_t points = 0;
};

ValidationReport Validate(const Skeleton& skel, const P3DLod& lod, const std::vector<std::string>& sections);

// ---------------------------------------------------------------- model.cfg animations (CfgModels::X::Animations)

enum class CfgAnimType { Rotation, RotationX, RotationY, RotationZ, Translation, TranslationX, TranslationY, TranslationZ, Hide, Direct, Unsupported };
enum class SourceAddress { Clamp, Mirror, Loop };

struct CfgAnimation
{
    std::string name, typeName, source, selection, axis, begin, end;
    CfgAnimType type = CfgAnimType::Unsupported;
    SourceAddress address = SourceAddress::Clamp;
    bool memory = true;            // axis points from the Memory LOD (else from the animated LOD)
    bool centerFirstVertex = false;
    float minValue = 0, maxValue = 1;
    float angle0 = 0, angle1 = 0;  // radians
    float offset0 = 0, offset1 = 0;
    float hideValue = 0, unHideValue = -1; // unHideValue < 0: not set
    // type "direct": rotation by angle around axisPos->axisDir (memory points) plus axisOffset along it, both scaled by phase
    std::string axisPosName, axisDirName;
    float directAngle = 0, directOffset = 0;
    int bone = -1;                 // skeleton bone animated (selection must be a bone)
    int sourceIndex = -1;
    int file = 0, line = 0;        // where the class is defined
    std::string problem;           // config-level problem (unknown type, not a bone, ...)
    bool problemIsWarning = false; // e.g. selection not a bone: Arma just ignores the animation
    // resolved from memory points
    bool axisOk = false;
    glm::vec3 axisPos{ 0.0f }, axisDir{ 0, 1, 0 };
    float axisLength = 1.0f;
    std::string axisProblem;   // axis unusable -> animation does nothing
    std::string axisWarning;   // works, but only thanks to a viewer fallback

    float Phase(float sourceValue) const;   // minValue..maxValue -> 0..1 with sourceAddress
    glm::mat4 Matrix(float sourceValue, const glm::vec3& bonePivot) const;
};

struct CfgSource
{
    std::string name;
    float minValue = 0, maxValue = 1, value = 0;
    bool animate = false; // oscillate over time in the viewer
};

// Reads the animations of CfgModels::<modelClass> (inherited classes included, definition order kept).
void BuildCfgAnimations(const ArmaConfig& cfg, const ModelCfgInfo& info, const Skeleton& skel,
                        std::vector<CfgAnimation>& anims, std::vector<CfgSource>& sources);

// Resolves axis points: memory=1 -> Memory LOD of `model`, else `lod`.
// memoryFallback: memory=0 but the axis exists only in the Memory LOD -> use it (flagged in axisWarning).
void ResolveAnimationAxes(std::vector<CfgAnimation>& anims, const P3DModel& model, const P3DLod& lod, bool memoryFallback);

// Per-bone matrices from model.cfg animations, hierarchical (child = parent * own animations in order).
void EvaluateCfgAnimations(const Skeleton& skel, const std::vector<CfgAnimation>& anims, const std::vector<CfgSource>& sources,
                           std::vector<glm::mat4>& boneMatrices);

// Per-point bone influences for CPU skinning (bones = skeleton indices).
struct SkinData
{
    struct Influence
    {
        int bone;
        float weight;
    };
    std::vector<uint32_t> offset;     // points + 1; influences of point p are [offset[p], offset[p+1])
    std::vector<Influence> influences;
    std::vector<float> restWeight;    // per point: 1 - sum(weights) (>0 keeps part of the point at rest)
};

// normalize: scale weights to sum 1 (binarize-like). maxBones > 0: keep only the strongest N bones.
// isDiscrete skeletons: each point goes to its deepest bone with weight 1 (nested selections = bone + ancestors).
SkinData BuildSkin(const Skeleton& skel, const P3DLod& lod, bool normalize, int maxBones);

// Per-point blended matrices from per-bone deformation matrices (linear blend skinning).
void SkinMatrices(const SkinData& skin, const std::vector<glm::mat4>& bones, std::vector<glm::mat4>& points);
