#pragma once
// The scene of the weapon modes: the weapon is the main model; the character holding it (Reload: always; Optics: in
// the aiming pose) and a comparison weapon are extra geometry in weapon space, slot items sit on their proxies with a
// temporary fit, the camera can look through the sight. Also: hot reload of the gesture RTMs, the reload timeline
// driving the model.cfg reload source, the touch line, the onion ghosts, the sight overlay, the "weapon" ui-script
// command. The logic is core/ReloadSync + OpticsFit; this class is their view.
#include "Camera.h"
#include "MovesEdit.h"
#include "ReloadSync.h"
#include "Renderer.h"
#include "Session.h"
#include "AppMode.h"
#include "ViewHost.h"
#include "WeaponView.h"

#include <string>
#include <vector>

// What the weapon scene needs of the viewer (implemented by App).
class IWeaponSceneHost : public IViewport
{
public:
    virtual ~IWeaponSceneHost() = default;
    virtual AppMode Mode() const = 0;
    virtual void SetMode(AppMode m) = 0;
    virtual Renderer& SceneRenderer() = 0;
    virtual OrbitCamera& SceneCamera() = 0;
    virtual const std::vector<MeshVertex>& BaseVerts() const = 0; // rest-pose geometry of the current LOD
    virtual const std::vector<uint32_t>& VertPoint() const = 0;   // vertex -> point
    virtual size_t AttachFirst() const = 0;                       // first vertex of the attached geometry
    virtual const SkinData& Skin() const = 0;
    virtual void RebuildAttachments() = 0;
    virtual void ReloadModel() = 0;
    virtual std::string ValidationSummary(const std::string& label) = 0;
};

class WeaponScene : public IWeaponView
{
public:
    WeaponScene(Session& session, ReloadSync& reload, OpticsFit& optics, AnimViewOptions& options, IViewHost& host, IWeaponSceneHost& scene)
        : s_(session), reload_(reload), optics_(optics), animView_(options), host_(host), h_(scene) {}

    bool Active() const { return h_.Mode() == AppMode::Reload || h_.Mode() == AppMode::Optics; }
    void Update(float dt);    // per frame: config, character, comparison weapon, reload source, fit, sight camera, lines
    void Ghosts(std::vector<MeshVertex>& gv, std::vector<Renderer::Ghost>& ghosts); // Reload onion skin
    void DrawOverlay();       // crosshair of the sight view
    void ApplySlotItems() { optics_.ApplySlotItems(Active()); }
    void SetWindow(int anim, float mn, float mx);
    std::string Command(const std::string& line); // ui-script "weapon ..."
    bool ConfigDirty() const { return weaponCfgDoc_.Dirty(); } // "weapon cfg" edits not saved

    // IWeaponView
    bool SaveWindows(bool write) override;
    bool CompareLoad(const std::string& clsOrPath) override;
    void ComputeTouch() override;
    void CharacterPoseChanged() override { weaponCharT_ = -1; }
    void SlotItemsChanged() override;

private:
    void WeaponEnsure();
    void WeaponCharLoad();
    void ComparePose(double t);
    void WeaponLines();

    Session& s_;
    ReloadSync& reload_;
    OpticsFit& optics_;
    AnimViewOptions& animView_;
    IViewHost& host_;
    IWeaponSceneHost& h_;
    double weaponPoll_ = 0.0, weaponCharT_ = -1.0;
    bool weaponCharShown_ = false, weaponLinesShown_ = false, cmpShown_ = false;
    OrbitCamera savedCamera_;
    bool cameraSaved_ = false;
    int weaponBlendShown_ = -1;
    bool weaponCfgModel_ = false; // "weapon cfg": model.cfg instead of config.cpp
    ConfigDoc weaponCfgDoc_;      // "weapon cfg": the weapon addon's config.cpp
};
