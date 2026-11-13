#pragma once
// The open document without GPU or UI: the model, its model.cfg / skeleton / model.cfg animations, the animation stack.
// The UI shows it and sends it commands; what changed is announced on the EventBus.
//
// Migration (CLAUDE.md, Architecture): the data is public while App still reads it through reference
// aliases (model_, skeleton_ ...); accessors come when the panels are split out (step 11).
#include "ArmaConfig.h"
#include "Attachments.h"
#include "EventBus.h"
#include "FileSystem.h"
#include "CommandStack.h"
#include "Moves.h"
#include "MovesEdit.h"
#include "RvMaterial.h"
#include "Weapon.h"
#include "P3D.h"
#include "RTM.h"
#include "Skeleton.h"
#include "Unit.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

// The game's configs from P: (bin/config.bin + a3/**/config.cpp, the classes the viewer reads), merged; a binary snapshot
// in cacheFile makes the next start fast. Null when there are none. Slow on a cold cache: run it on a worker thread.
std::shared_ptr<ArmaConfig> LoadGameConfigs(const std::filesystem::path& root, const std::filesystem::path& cacheFile);

class Session
{
public:
    Session(const IFileSystem& files, EventBus& events);
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // ---- data
    P3DModel model;
    bool hasModel = false;
    int lod = 0;
    std::filesystem::path projectRoot;     // P:\ (or the drive root) of the model
    std::filesystem::path modelCfg;        // model.cfg used (empty: none)
    ArmaConfig cfg;                        // model.cfg (+ includes)
    ModelCfgInfo modelInfo;
    Skeleton skeleton;
    P3DModel pivots;                       // skeleton pivotsModel
    bool hasPivots = false;
    std::vector<CfgAnimation> cfgAnims;    // model.cfg animations of the model
    std::vector<CfgSource> cfgSources;     // their sources (sliders)
    AnimStack anim;                        // RTM layers on the skeleton
    // the addon's config.cpp above the model (over the game configs when loaded)
    std::filesystem::path configCpp;
    std::shared_ptr<ArmaConfig> modCfg;
    std::shared_ptr<const ArmaConfig> game; // game configs from P: (bin/config.bin + a3/**/config.cpp), merged
    std::vector<ConfigSkin> configSkins;    // classes that re-texture this model (hiddenSelectionsTextures)
    std::vector<std::filesystem::path> configCppFiles; // config.cpp and its includes (watched)
    int activeSkin = -1;
    Attachments attach;                     // proxies + crew
    Unit unit;                              // a soldier class assembled on the open model (Unit window)
    MovesGraph moves;                       // CfgMoves of the model's skeleton
    MovesGraph gestures;                    // CfgGestures of the move config (MovesLoader)
    std::map<std::string, BoneMask> masks;  // BlendAnims masks of the moves + gesture config (lower-case names)
    int blendMode = 1;                      // gestures / handAnim / CfgMoves layers: 1 parent-relative (default), 0 model space
    WeaponCfg weapon;                       // the open model as a weapon (CfgWeapons class, slots, optics, reload)
    ConfigDoc movesDoc;                     // the user's own config with those CfgMoves (text editing, .bak on save)
    CommandStack commands;                  // undo / redo of every document edit (Ctrl+Z / Ctrl+Y)

    // ---- operations
    // Reads a model file (MLOD); false + logged error if unreadable, or incomplete while requireComplete
    // (an exporter still writing). Does not replace the open model.
    static bool ReadModel(const std::filesystem::path& path, bool requireComplete, P3DModel& out); // the disk
    static bool ReadModel(const IFileSystem& files, const std::filesystem::path& path, bool requireComplete, P3DModel& out);
    // Makes `m` the open model (keepLod: stay on the same LOD index when it exists), logs its LODs, loads its
    // model.cfg / skeleton / animations and binds the animation layers to the new skeleton. Publishes ModelLoaded.
    void SetModel(P3DModel&& m, bool keepLod);
    // model.cfg next to the model, then up the folders to the project root (as binarize looks for it)
    std::filesystem::path FindModelCfg(const std::filesystem::path& p3d) const;
    // (Re)loads model.cfg, the skeleton, pivots and model.cfg animations of the open model
    void LoadModelConfig();
    // Finds and loads the addon's config.cpp (mounting the mod's own prefix when it is not on P:), its re-texturing classes
    // and checks their hidden selections against the model.cfg sections. False when there is no config.cpp.
    bool LoadAddonConfig();
    // The config the crew / proxies / hitpoints are looked up in: the addon's, else the game's
    const ArmaConfig* CrewConfig() const { return modCfg ? modCfg.get() : game.get(); }
    // Lowest point of the first LOD in the rest pose (0 without points)
    float RestFloor() const;

    EventBus& Events() { return events_; }
    const IFileSystem& Files() const { return files_; }

private:
    void LogModel() const;

    const IFileSystem& files_;
    EventBus& events_;
};
