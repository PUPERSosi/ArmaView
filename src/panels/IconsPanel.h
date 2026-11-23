#pragma once
// Icons window: UI pictures of the open model's classes by BI's standards (IconStandards): a preset per kind (editor
// preview, weapon, gear, headgear, attachment, vehicle silhouette, map icon, custom), the fitting (exposure, saturation,
// contrast, outline, border, camera) with a live preview beside BI's own picture of the kind, PNG / JPG and PAA
// (ImageToPAA of the Arma 3 Tools), the file name and config line by BI's rules, a batch over the mod's classes and the
// game call that makes the editor previews of the mod's classes. The pictures come from view/IconRenderer.
#include "IconRenderer.h"
#include "Visibility.h"
#include "Material.h"
#include "Session.h"
#include "ViewHost.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

class IconsPanel
{
public:
    struct Hooks
    {
        std::function<bool(const std::filesystem::path& model)> loadModel; // open now (batch)
        std::function<void(const std::string& cls)> assembleUnit;          // a character class dressed (ArmaMan + its gear)
        std::function<bool()> ready;                                       // nothing loads or streams
        ModelVisibility* visibility = nullptr;                             // the model's hiding (swapped for the icon's)
    };
    IconsPanel(Session& session, IconRenderer& renderer, TextureCache& textures, IViewHost& host, Hooks hooks)
        : s_(session), r_(renderer), tex_(textures), host_(host), hooks_(std::move(hooks)) {}

    std::string imageToPaa; // ImageToPAA.exe (settings "ui": ImageToPaa)
    void Draw(const std::string& title);
    void AfterSceneDraw();  // a requested preview rendered, a batch step
    bool BatchRunning() const { return batchRunning_; }

    // ui-script / tests: the open model's picture with the preset's BI numbers into a file (.png / .jpg / .paa)
    bool RenderToFile(IconPreset preset, const std::filesystem::path& file, std::string& message, RgbaImage* picture = nullptr);
    bool RenderToFile(const IconSettings& settings, const std::filesystem::path& file, std::string& message, RgbaImage* picture = nullptr);

    bool SelectClass(const std::string& cls);
    // BI's pictures of a category from the game configs on P: (read once per category while those configs stay)
    const std::vector<IconExample>& Examples(IconCategory c); // ui-script: that class of the mod (its model and skin); false: not listed
    const std::string& Message() const { return msg_; }
    IconSettings& Current() { return set_; }       // ui-script: the window's settings
    void Changed() { dirty_ = true; SaveClass(); } // ... after changing them
    // ui-script: a selection hidden / shown for the icon only ("reset": none); false: not in the icon's hiding now
    bool HideForIcon(const std::string& name);
    struct ClassItem
    {
        std::string root, cls;  // CfgWeapons, arifle_MyRifle_F
        IconPreset preset = IconPreset::Custom;
        IconCategory category = IconCategory::Custom; // by inheritance
        std::string reason;                           // the base class it was decided by
        std::filesystem::path model;  // the model the picture shows (IconSubjectOf: worn model, uniformClass)
        ConfigSkin skin;              // its re-texture
        bool man = false, vehicle = false, selected = true;
        bool binarized = false;       // its model is not a source file (MLOD / glTF): no picture, the standard message
    };
    // the classes of the config (the mod's own ones, scope 2) with a model on disk; modelOnly: only those of the open model
    std::vector<ClassItem> ConfigClasses(bool modelOnly) const;
    static std::string SqfForClasses(const std::vector<ClassItem>& items); // CfgVehicles classes only

private:
    void PickClass(int i);
    void SaveClass();          // the class's category / camera / fitting into `saved`
    void LoadClass();          // ... and back (after its category's preset)
    void ResetCamera();
    void DrawStandards();      // the "BI standards" window: every category's rules
    void DrawConfigPictures(); // the class's pictures already in the config, checked
    bool showStandards_ = false;
    std::vector<IconPictureIssue> issues_;
    std::string issuesKey_;    // the class / config they were checked for
public:
    void RecheckPictures() { issuesKey_.clear(); }
    // the icon's own hiding of model parts (the outliner, H / Shift+H / Alt+H work on it while the Icons mode is on);
    // the model's own hiding comes back on leaving the mode
    void EnterHiding();
    void LeaveHiding();
    void ResetHiding();
    bool showParts = false;    // the "Model parts" window (App draws the outliner in it)
private:
    std::map<std::string, ModelVisibility::Snapshot> iconHiding_; // per class
    ModelVisibility::Snapshot modelHiding_;                       // the model's, while the icon's is in place
    bool hidingActive_ = false;
    std::string hidingCls_;
    std::filesystem::path hidingModel_;
    void ShowClass(const ClassItem& it); // its model open, its skin active
    void ApplyPreset(IconPreset p);
    void ApplyCategory(IconCategory c, bool manual); // its preset; manual: remembered for the class
    IconCategory Category() const;                    // the class's: the user's choice, else by inheritance
    std::filesystem::path OutputFile() const;
    bool Save(std::string& message);

    Session& s_;
    IconRenderer& r_;
    TextureCache& tex_;
    IViewHost& host_;
    Hooks hooks_;

    std::vector<ClassItem> classes_;     // of the open model
    std::filesystem::path classesOf_;    // the model they were listed for
    int cls_ = -1;
    char name_[128] = {};                // class name used in the file name
    IconSettings set_;
    IconCategory cat_ = IconCategory::Custom;
    int example_ = 0;                                 // which BI example of the category is shown
    std::map<int, std::vector<IconExample>> examples_; // per category (Examples)
    const ArmaConfig* examplesOf_ = nullptr;
public:
    std::map<std::string, std::string> saved;         // per class: the user's category / camera / fitting (settings "icons")
private:
    bool dirty_ = true, paa_ = true, previewStale_ = false;
    RgbaImage picture_;
    ID3D11ShaderResourceView* srv_ = nullptr;
    std::string msg_, sqf_;
    // batch
    std::vector<ClassItem> batch_;
    bool batchListed_ = false, batchRunning_ = false, batchPreviews_ = true, batchMapIcons_ = true;
    size_t batchNext_ = 0;
    int batchPhase_ = 0, batchWait_ = 0, batchDone_ = 0, batchFailed_ = 0;
};
