#pragma once
// A soldier assembled from CfgVehicles the way the game dresses it: the class's model is the uniform (body), the
// head comes from its face (CfgFaces >> faceType >> face >> head -> CfgHeads >> head >> model), the worn gear from
// linkedItems[] (CfgWeapons >> item >> ItemInfo: type 605 headgear, 701 vest, 616 NVG; uniformModel / modelOff) and
// backpack= (CfgVehicles >> pack >> model). Every part is skinned with the unit's skeleton (the open model's): gear
// is rigged to the same bones, so one set of bone matrices poses them all. Textures: hiddenSelections[] +
// hiddenSelectionsTextures[] / Materials[] of the class that wears the part (face: texture / material of the face).
// A part that cannot be shown keeps the reason (class not found, model not on disk, binarized model, points on bones
// the skeleton does not have ...), logged and shown in the Unit window.
// Outfit (try-on): the body is any open P3D (the Samples' ArmaMan, a file) instead of a class's model, and parts can
// be P3D files besides config classes; every part is checked against the body's skeleton (bones without a pair,
// more than 4 bones on a point, weights that do not sum to 1). An outfit is saved as a preset (text, see SavePreset).
#include "ArmaConfig.h"
#include "Attachments.h"
#include "RvMaterial.h"
#include "Skeleton.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

enum class UnitSlot { Head, Headgear, Goggles, Vest, NVG, Backpack, Other, Count };
const char* UnitSlotName(UnitSlot s);
bool ParseUnitSlot(std::string_view name, UnitSlot& out);

struct UnitPart
{
    UnitSlot slot = UnitSlot::Head;
    std::string cls;          // CfgWeapons / CfgVehicles / CfgFaces class
    std::string from;         // where the class came from: linkedItems[], backpack=, face (identityTypes ...), replaced
    std::string armaModel;    // \a3\characters_f\blufor\headgear_b_helmet_plain
    std::filesystem::path file;
    std::shared_ptr<AttachedModel> model; // own copy (re-textured), skinned with the unit's skeleton
    SkinData skin;
    std::vector<MeshVertex> posed;
    bool enabled = true;
    std::string problem;      // empty: shown
    std::string note;         // e.g. "NVG lowered: modelOff", "2 hidden selection texture(s)"
    int unboundPoints = 0;    // drawn points without a bone of the unit's skeleton (they stay in the bind pose)
    std::vector<std::string> unboundSelections; // selections holding them (probably bones the skeleton lacks)
    // skinning checks against the body's skeleton
    std::vector<std::string> unpairedBones; // bones of the part's own skeleton (its model.cfg) with weighted points, not in the body
    int overBones = 0;                      // points weighted to more than 4 bones of the body (the engine keeps 4)
    int badWeightSum = 0;                   // points whose bone weights do not sum to 1 (+-0.01; points on no bone not counted)
    bool fromFile = false;                  // a P3D file, not a config class
    std::string Checks() const;             // one line: the problems above, or empty
    bool Shown() const { return enabled && problem.empty() && model && model->status == AttachedModel::Status::Ok; }
};

class Unit
{
public:
    // Reads the class (no models loaded): its model, uniform skin and the part classes. False (logged) if the class
    // is not in the config.
    bool Read(const ArmaConfig& cfg, const std::string& cls);
    // Loads the parts' models and skins them with skel (the open model's skeleton). Parts already loaded for the same
    // class / model are kept.
    void Load(const ArmaConfig& cfg, const Skeleton& skel, const std::filesystem::path& projectRoot);
    // Replaces (or with an empty class removes) the part of a slot; Load must follow.
    void Replace(const ArmaConfig& cfg, UnitSlot slot, const std::string& cls);
    // Outfit: the open P3D is the body (no class); parts are added from the config or from files. Load must follow.
    void StartOutfit(const std::filesystem::path& body);
    // A P3D file as a part (slot Other: added; another slot: replaces the part of that slot). Load must follow.
    void AddFile(UnitSlot slot, const std::filesystem::path& file);
    void RemovePart(size_t index);
    void Clear();
    bool Active() const { return !cls_.empty() || outfit_; }
    bool Outfit() const { return outfit_; }
    const std::filesystem::path& OutfitBody() const { return outfitBody_; }
    const std::string& Class() const { return cls_; }
    const std::string& ModelPath() const { return model_; } // the class's model (Arma path)
    const ConfigSkin& UniformSkin() const { return uniformSkin_; } // hiddenSelections of the class for the body
    std::vector<UnitPart>& Parts() { return parts_; }
    const std::vector<UnitPart>& Parts() const { return parts_; }
    // Posed vertices of the shown parts (bones empty = bind pose) into attach.unitParts (same order every call
    // until the parts change; Publish then rebuilds the list).
    void Publish(Attachments& attach) const;
    void Pose(const std::vector<glm::mat4>& bones, Attachments& attach);
    std::vector<std::filesystem::path> Files() const; // part models (hot reload)
    std::string Report() const;                        // one line per part (ui-script `unit report`)

    // Preset (text): "body=class:<cls>" or "body=file:<path>", then "part=<slot>|class|<cls>|<shown>" or
    // "part=<slot>|file|<path>|<shown>" per part, in order.
    struct Preset
    {
        struct Part { UnitSlot slot = UnitSlot::Other; bool file = false; std::string what; bool enabled = true; };
        std::string bodyClass;           // a CfgVehicles class (its model is the body) ...
        std::filesystem::path bodyFile;  // ... or a P3D
        std::vector<Part> parts;
    };
    Preset MakePreset() const;
    static std::string SavePreset(const Preset& p);
    static bool ParsePreset(const std::string& text, Preset& out, std::string& error);
    // Sets the parts of the preset (the body must be set up already: Read or StartOutfit). Load must follow.
    void ApplyPresetParts(const ArmaConfig* cfg, const Preset& p);

    // Soldier classes (CfgVehicles with uniformClass) of the config and its fallback, sorted.
    static std::vector<std::string> SoldierClasses(const ArmaConfig& cfg);
    // Item classes that fit a slot (CfgWeapons by ItemInfo type, CfgVehicles backpacks, CfgFaces faces).
    static std::vector<std::string> Candidates(const ArmaConfig& cfg, UnitSlot slot);

private:
    void LoadPart(UnitPart& p, const ArmaConfig& cfg, const Skeleton& skel, const std::filesystem::path& projectRoot);
    UnitPart ReadPart(const ArmaConfig& cfg, UnitSlot slot, const std::string& cls, const std::string& from) const;
    UnitPart FilePart(UnitSlot slot, const std::filesystem::path& file) const;
    std::string cls_, model_;
    bool outfit_ = false;
    std::filesystem::path outfitBody_;
    std::vector<std::string> identityTypes_;
    std::string faceType_;
    ConfigSkin uniformSkin_;
    std::vector<UnitPart> parts_;
    ModelCache cache_; // part models as read from disk (copies are re-textured)
};

// A class of a root (CfgVehicles ...) in the config or its fallback (game) configs.
const CfgClass* FindConfigClass(const ArmaConfig& cfg, std::string_view root, std::string_view cls);
