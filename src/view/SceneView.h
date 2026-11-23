#pragma once
// What the renderer draws of the open document, prepared each time it changes: the rest-pose geometry of the current
// LOD (vertices, normals, highlight / heat / diagnostic colours, hidden faces, proxies and crew as extra geometry), the
// drawn sections with their materials and textures, the pose (RTM + model.cfg animations -> bone and point matrices,
// bounds, attachments), skeleton / normal / proxy lines, onion-skin ghosts, the bone trail, the floor grid, other LODs
// as overlays. No decisions of the document here: the core says what plays and what is loaded; this is the render data.
#include "Camera.h"
#include "DisplaySettings.h"
#include "Material.h"
#include "MaterialSet.h"
#include "MaterialsPanel.h"
#include "MovesLoader.h"
#include "Playback.h"
#include "Renderer.h"
#include "Session.h"
#include "Validation.h"
#include "ValidationPanel.h"
#include "ViewHost.h"
#include "Visibility.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// What the scene needs of the viewer around it (implemented by App).
class ISceneHost
{
public:
    virtual ~ISceneHost() = default;
    virtual int DamageState() const = 0;      // 0 intact, 1 damaged, 2 destroyed (Materials)
    virtual bool HideVehicle() const = 0;     // crew only (Crew window)
    virtual void ApplyRootMotion(std::vector<glm::mat4>& bones) = 0; // control mode walks the character
    virtual void ExtraGhosts(std::vector<MeshVertex>& gv, std::vector<Renderer::Ghost>& ghosts) = 0; // weapon modes
    virtual void SectionsRebuilt() = 0;       // the drawn sections changed (selection indices may be out of range)
};

class SceneView
{
public:
    enum class Highlight { None, Selection, TooManyBones, BadWeightSum, NoBone };
    struct NormalLine
    {
        uint32_t point;
        glm::vec3 normal;
        glm::vec4 color;
    };

    SceneView(Session& session, Playback& playback, MaterialSet& mats, TextureCache& textures, Renderer& renderer, const OrbitCamera& camera,
              DisplaySettings& display, AnimViewOptions& animView, GesturePlayer& gestures, ModelVisibility& vis, IViewHost& host, ISceneHost& sceneHost)
        : s_(session), playback_(playback), mats_(mats), texCache_(textures), renderer_(renderer), camera_(camera), display_(display),
          animView_(animView), gesturePlayer_(gestures), vis_(vis), host_(host), h_(sceneHost) {}

    void PrepareLod(bool memoryAxisFallback); // per LOD: pivots, axes, bone selections, validation, mesh / proxy points
    void RebuildLodGeometry();      // vertices, colours, hidden faces, attachments of the current LOD + sections + pose
    void BuildDrawSections();       // sections: materials (edited / original), textures, procedural, damage
    void UpdatePose();              // matrices, bounds, attachments, lines, ghosts, trail
    void RebuildSkeletonLines();
    void UpdateGhosts(bool animate);
    void UpdateTrail();
    void RebuildGrid();
    void RebuildLodOverlay();       // other LODs drawn over the current one (wireframe)
    void VisibilityChanged();        // saved (settings.txt) + geometry again
    void HideSelected();             // H: picked faces, or the highlighted selection
    void IsolateSelected();          // Shift+H (again: back)
    void ShowAllHidden();            // Alt+H
    void LoadDamageMaterials();     // CfgVehicles >> class >> Damage (mat / tex)
    glm::vec3 BonePosition(int bone) const;
    void ApplyRootMotion(MovesControl& control, std::vector<glm::mat4>& bones);
    bool PickFace(const glm::mat4& viewProj, float nx, float ny, uint32_t& face) const; // nearest face under the ray
    uint64_t PoseSignature() const; // what the pose depends on besides time
    IValidationView::BoneUsage UsageOfBone(int bone) const;
    void LogMissingBones(const std::string& rtmName, const std::vector<int>& bones, bool indent) const;
    void LogLayerCoverage(const AnimLayer& layer) const;
    const RvMaterial* EffectiveRvMaterial(const std::string& name);
    const FaceDiagInfo& FaceInfo();
    AnimStatus AnimStatusOf(const CfgAnimation& a) const; // the animation against the drawn LOD
    void LogMaterials() const;      // textures / RVMATs of the drawn sections
    void LogCfgAnimations() const;  // model.cfg animations with their problems
    float FloorY() const { return s_.anim.layers.empty() ? restFloor : 0.0f; } // RTMs put the feet on y = 0

    bool streamTextures = true;      // textures decode in the background (placeholders until they arrive)

    // highlight on the mesh
    Highlight highlight = Highlight::None;
    int selection = -1;    // highlighted selection (Highlight::Selection)
    int selectedBone = -1;
    bool maskHeat = false; // the mesh coloured by a mask (maskView, or customHeat of a correction layer)
    std::string maskView;
    std::vector<float> customHeat;
    std::vector<uint32_t> pickFaces; // Ctrl+click on the scene: picked faces of the current LOD
    std::vector<uint8_t> faceHidden; // current LOD
    std::vector<uint8_t> skinHidden; // current LOD: faces the config skin hides (hiddenSelectionsTextures "")
    std::vector<uint8_t> pointSkinOnly; // current LOD: points only on skin-hidden faces (left out of the bounds)
    size_t skinHiddenFaces = 0;      // count of skinHidden
    ValidationReport validation;     // of the current LOD
    FaceDiagInfo faceInfo;           // of the current LOD

    // geometry of the current LOD (rest pose; the posed copy is uploaded while animating)
    std::vector<MeshVertex> baseVerts;
    std::vector<uint32_t> vertPoint;
    std::vector<NormalLine> normalBase;
    std::vector<int> boneSelection;     // per bone: selection index in the current LOD
    std::vector<uint8_t> pointInMesh;   // per point: used by a non-proxy face
    std::vector<int> pointProxy;        // per point: index into proxyNames (-1 = none)
    std::vector<std::string> proxyNames;
    std::vector<MeshSectionInfo> meshSections;
    size_t vehicleSections = 0;         // meshSections before the attached ones
    size_t attachFirst = 0;             // first vertex of the attached geometry in baseVerts
    glm::vec3 restMin{ -1.0f }, restMax{ 1.0f };
    glm::vec3 boundsMin{ -1.0f }, boundsMax{ 1.0f };
    glm::vec3 shadowMin{ -1.0f }, shadowMax{ 1.0f }; // posed mesh + attachments: what casts a shadow
    float restFloor = 0.0f;             // lowest point of the first LOD in the rest pose
    float gridFloor = 0.0f;             // the height the grid was built at
    float shadowR = 0.0f;               // sun camera half size, kept while the pose stays inside
    glm::vec2 gridCell{ 0.0f };         // control mode: the grid travels with the character
    SkinData skin;
    std::vector<glm::mat4> boneMats, pointMats;
    std::vector<LineVertex> trail;
    std::string trailKey;
    double posedTime = -1.0;            // time of the last UpdatePose
    uint64_t posedSig = 0;
    std::map<std::string, std::pair<std::string, std::string>> damageMat; // rvmat (lower) -> damaged, destroyed
    std::map<std::string, std::string> damageTex;                       // texture (lower) -> damaged

private:
    Session& s_;
    Playback& playback_;
    MaterialSet& mats_;
    TextureCache& texCache_;
    Renderer& renderer_;
    const OrbitCamera& camera_;
    DisplaySettings& display_;
    AnimViewOptions& animView_;
    GesturePlayer& gesturePlayer_;
    ModelVisibility& vis_;
    IViewHost& host_;
    ISceneHost& h_;
};
