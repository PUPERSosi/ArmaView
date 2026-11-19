#pragma once
// RTM skeletal animation (source format RTM_0101) and the layered animation player.
// Optional prefix "RTM_MDAT", uint32 0, uint32 eventCount, events... (seen only with 0 events; events are skipped
// by locating the following "RTM_0101").
// Layout (verified by exact file sizes): "RTM_0101", vec3 move, uint32 frames, uint32 bones,
// bones x char[32] names, then per frame: float time (phase 0..1), bones x (char[32] name, float[12]).
// The 12 floats are a model-space deformation matrix: 3 columns (aside, up, dir) + translation,
// p' = c0*x + c1*y + c2*z + t. Verified: every child pivot maps to the same point with the child's and
// the parent's matrix (error 0.0000 m over 292+ joints), i.e. skinning is p' = sum(w * M * p).
#include "Skeleton.h"

#include <atomic>
#include "P3D.h" // kBinarizedNotSupported

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct RtmInfo
{
    std::string signature;
    bool binarized = false; // not a source RTM (a binarized game file): not supported, see kBinarizedNotSupported
    glm::vec3 move{ 0.0f };
    uint32_t frames = 0;
    std::vector<std::string> bones;
    uint64_t dataOffset = 0;  // offset of "RTM_0101"
    uint32_t mdatEvents = 0;  // events in the RTM_MDAT block (not parsed)
    uint32_t headerSize = 28; // RTM_0101: 28, RTM_0100 (OFP, Z step only): 20
};

// Reads the header and bone names. Returns false with a message in `error` (not logged here).
bool ReadRtmHeader(const std::filesystem::path& path, RtmInfo& out, std::string& error);

struct BonePose
{
    glm::quat rot{ 1, 0, 0, 0 };
    glm::vec3 pos{ 0.0f };
    glm::vec3 scale{ 1.0f };

    glm::mat4 Matrix() const;
    static BonePose FromMatrix(const glm::mat4& m);
    static BonePose Mix(const BonePose& a, const BonePose& b, float t);
};

struct RtmAnimation
{
    std::filesystem::path path;
    RtmInfo info;
    std::vector<float> times;     // per frame, phase 0..1
    std::vector<BonePose> poses;  // frames x bones
    std::string Name() const;
    size_t Frames() const { return times.size(); }
    size_t Bones() const { return info.bones.size(); }
    BonePose Sample(size_t bone, float phase) const; // interpolated between neighbouring keyframes
};

// Loads the whole animation (RTM_0100 / RTM_0101 / RTM_MDAT; a binarized file -> kBinarizedNotSupported). Problems that still allow playback go to `warnings`.
bool LoadRtm(const std::filesystem::path& path, RtmAnimation& out, std::string& error, std::vector<std::string>& warnings);

struct RtmBoneCheck
{
    std::filesystem::path file;
    std::string error;                // non-empty: file not checked
    RtmInfo info;
    std::vector<std::string> missing; // skeleton bones absent in the RTM
    std::vector<std::string> extra;   // RTM bones not in the skeleton (warning, not an error)
};

// Loop seam: pose difference between the first and the last frame per bone (model space).
struct SeamJump
{
    std::string bone;
    float distance = 0; // m
    float angle = 0;    // degrees
};
// Bones whose first and last frame differ by more than the limits, worst first.
std::vector<SeamJump> CheckLoopSeam(const RtmAnimation& anim, float maxDistance = 0.01f, float maxAngleDeg = 3.0f);

// Duration of a CfgMoves state from its speed: speed > 0 = cycles per second, speed < 0 = -seconds per cycle.
float DurationFromMovesSpeed(float speed);

// Every .rtm of a folder against the skeleton (headers only). done / total: progress for another thread; cancel: stop early
// (the files checked so far come back).
std::vector<RtmBoneCheck> CheckRtmFolder(const std::filesystem::path& dir, const Skeleton& skel, std::atomic<int>* done = nullptr,
                                         std::atomic<int>* total = nullptr, const std::atomic<bool>* cancel = nullptr);
RtmBoneCheck CheckRtmFile(const std::filesystem::path& file, const Skeleton& skel); // one file of it

enum class LayerMode { Blend, Override };

// Skeleton bones a layer has no data for:
//  FollowParent - move rigidly with the parent bone (default);
//  Engine       - Blend layers give identity (bind pose in model space), as Poseidon does (CWR RtAnimation.cpp:236);
//                 not confirmed for Arma 3 (stretches fingers / face bones). Override layers never use identity.
enum class MissingBones { Engine, FollowParent };

struct AnimLayer
{
    std::shared_ptr<RtmAnimation> anim;
    bool enabled = true;
    LayerMode mode = LayerMode::Blend;
    float weight = 1.0f;
    float speed = 1.0f;     // multiplier
    float duration = 1.0f;  // seconds per cycle at speed 1
    float offset = 0.0f;    // phase offset 0..1
    bool loop = true;
    bool pingPong = false;  // 0..1..0 instead of wrapping
    std::vector<int> boneMap; // skeleton bone -> RTM bone (-1 = not animated by this layer)
    std::vector<float> boneWeight; // Override layers: per skeleton bone (a gesture mask); empty = 1 for every bone
    int tag = 0;                   // 1 = gesture layer managed by the viewer
    int matched = 0;
};

// Stack of layers evaluated top to bottom:
//  - Blend layers: weighted average (quaternion nlerp, linear translation) of the layers that animate a bone;
//  - Override layers: then mix the result towards the layer's pose by its weight (e.g. a gesture on top).
class AnimStack
{
public:
    std::vector<AnimLayer> layers;
    MissingBones missing = MissingBones::FollowParent; // bones without data follow their parent (Engine: identity = bind)
    bool overrideRelative = false; // Override layers (gestures, handAnim) mixed in parent space instead of model space
    bool subtractStep = false; // OFP IntroduceStep: pos -= move * phase (CWR RtAnimation.cpp:771); Arma 3 RTMs are already in place

    std::vector<bool> Covered(size_t bones) const; // per skeleton bone: some enabled layer (weight > 0) has data
    std::vector<bool> Overridden(size_t bones) const; // per skeleton bone: an active Override layer moves it (data, mask > 0)

    void Bind(const Skeleton& skel); // (re)builds bone maps and the parent-first order
    bool Active() const;
    float Phase(const AnimLayer& l, double time) const;
    void Evaluate(const Skeleton& skel, double time, std::vector<glm::mat4>& boneMatrices) const;

private:
    std::vector<int> order_; // parent before child
};
