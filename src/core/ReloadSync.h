#pragma once
// Reload sync (weapon mode A) and optic fitting (B) without a view - see ReloadSync.cpp.
#include "Attachments.h"
#include "Moves.h"
#include "RTM.h"
#include "Session.h"
#include "Skeleton.h"
#include "Weapon.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

// a named point of the model's Memory LOD (centroid of its selection)
bool MemoryPointOf(const P3DModel& m, const std::string& name, glm::vec3& out);

class ReloadSync
{
public:
    ReloadSync(Session& s, SkinData& skin)
        : weapon_(s.weapon), s_(s), model_(s.model), hasModel_(s.hasModel), lod_(s.lod), skeleton_(s.skeleton), cfgAnims_(s.cfgAnims),
          cfgSources_(s.cfgSources), cfg_(s.cfg), attach_(s.attach), game_(s.game), projectRoot_(s.projectRoot), skin_(skin), blendMode_(s.blendMode) {}

    struct WeaponChar
    {
        std::shared_ptr<AttachedModel> model;   // the crew model holding the weapon
        uint32_t proxyPoints[3] = {};           // its weapon / pistol proxy triangle (first LOD)
        bool proxyOk = false;
        AnimStack stack;                        // stance + handAnim + reload gesture
        std::shared_ptr<RtmAnimation> base, hand, gesture;
        std::string baseState, gestureName, gestureMask, problem;
        std::filesystem::path baseFile, handFile, gestureFile;
        float baseDuration = 1.0f, gestureDuration = 1.0f, gestureSpeed = 0.0f, gestureInterp = 6.0f;
        bool gestureLooped = false;
        ConfigRef gestureRef;
        std::vector<float> leftIK, rightIK;
        std::vector<glm::mat4> bones;           // last pose (character space)
        glm::mat4 toWeapon{ 1.0f };             // character space -> weapon space
        std::filesystem::file_time_type stamp[3]{};
    };
    struct CompareWeapon
    {
        std::string cls, problem;
        std::shared_ptr<AttachedModel> model;
        Skeleton skel;
        std::vector<CfgAnimation> anims;
        std::vector<CfgSource> sources;
        SkinData skin;
        float duration = 1.0f;          // its reload gesture
        std::vector<int> reloadAnims;    // its animations of the reload source
        double shownT = -1;
    };
    struct WindowEdit { float min0, max0, min1, max1; }; // a min / max window: in the file / edited

    WeaponCfg& weapon_;
    std::string weaponKey_;
    std::filesystem::file_time_type weaponStamp_{};
    WeaponChar wchar_;
    CompareWeapon cmp_;
    double time = 0.0;                          // the reload timeline (s)
    bool playing = false, loop = true;
    float speed = 1.0f;
    float reloadK_ = 1.0f;                      // reload speed factor (game: reloadSpeed multiplier)
    std::string reloadSource_ = "reloadMagazine";
    std::vector<int> reloadAnims_;              // model.cfg animations driven by the reload source
    std::map<std::string, WindowEdit> windowEdits_; // animation (lower) -> window in the file / edited
    std::vector<std::string> windowDiff_;
    int touchBone_ = -1, touchSel_ = -1, touchAnim_ = -1;
    float touchThresholdCm_ = 4.0f, touchMin_ = 0.0f, touchMinT_ = 0.0f;
    std::vector<float> touchDist_;
    std::string weaponMsg_;
    bool gestureFade_ = true;                   // gesture fade in / out (approximation)
    bool handIK_ = true;                        // leftHandIKCurve / rightHandIKCurve of the gesture applied
    float handIKWeight_[2] = { 0, 0 };          // left / right IK weight at the last pose

    float ReloadDuration() const;
    float ReloadPhase(double t) const;
    bool WeaponEnsure();                        // the weapon config of the open model (true: read again)
    void WeaponCharLoad();
    bool WeaponCharPose(double t, bool withGesture, std::vector<glm::mat4>& bones, glm::mat4& toWeapon, std::vector<MeshVertex>* verts);
    bool CompareLoad(const std::string& clsOrPath);
    glm::vec3 SelectionCentroidAt(int sel, float phase);
    glm::vec3 CharJoint(int bone, const std::vector<glm::mat4>& bones, const glm::mat4& toWeapon) const;
    void ComputeTouch();
    bool SuggestWindow();
    void SetWindow(int anim, float mn, float mx);
    bool SaveWindows(bool write, std::string& saved); // diff (write: .bak + save; the caller reloads the model)
    bool TakePoseChanged() { const bool c = poseChanged_; poseChanged_ = false; return c; }
    const std::string& WeaponKey() const { return weaponKey_; }

private:
    const ArmaConfig* CrewConfig() const { return s_.CrewConfig(); }
    Session& s_;
    P3DModel& model_;
    bool& hasModel_;
    int& lod_;
    Skeleton& skeleton_;
    std::vector<CfgAnimation>& cfgAnims_;
    std::vector<CfgSource>& cfgSources_;
    ArmaConfig& cfg_;
    Attachments& attach_;
    std::shared_ptr<const ArmaConfig>& game_;
    std::filesystem::path& projectRoot_;
    SkinData& skin_;
    int& blendMode_;
    bool poseChanged_ = false;
};

class OpticsFit
{
public:
    OpticsFit(Session& s, ReloadSync& rs) : s_(s), rs_(rs) {}

    struct SightNumbers
    {
        bool ok = false, eyeOk = false;
        std::string from, problem, note;
        std::vector<WeaponOptics> modes;
        glm::vec3 barrelA{ 0.0f }, barrelB{ 0.0f }, camera{ 0.0f }, dir{ 0, 0, 1 }, eye{ 0.0f };
        glm::vec3 offsetMm{ 0.0f };
        float heightMm = 0, angleDeg = 0, elevDeg = 0, windDeg = 0, zeroM = -1, eyeDistMm = -1;
        float zoomInit = 0.75f, zoomMin = 0.75f, zoomMax = 0.75f;
    };

    std::map<std::string, std::string> slotItem_;      // slot class -> item class
    std::map<std::string, glm::vec3> fitPos_, fitRot_; // slot class -> temporary fit (mm, degrees; weapon axes)
    std::map<std::string, std::filesystem::file_time_type> itemStamp_;
    int opticsSlot_ = -1, opticsMode_ = 0;
    SightNumbers sight_;
    float zoom = -1.0f;                                 // opticsZoom picked in the window (-1: the sight's initial one)
    bool throughSight = false, eyeView = false;         // the camera at the sight / at the character's eye
    bool aimPose = true;                                // the character aiming with the weapon

    void ApplySlotItems(bool weaponMode);   // the magazine and the chosen items on their slot proxies
    void ComputeSight(bool charShown);      // barrel, camera, angles, zero, eye distance (charShown: the eye too)

private:
    Session& s_;
    ReloadSync& rs_;
};
