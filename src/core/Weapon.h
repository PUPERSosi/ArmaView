#pragma once
// Weapon checks without the game: what the config says about a weapon model.
//   CfgWeapons class whose model is this P3D -> reloadAction (+ the muzzles' own reloadAction) -> CfgGesturesMale >>
//   States >> <action>: RTM, speed, mask; handAnim for the character's skeleton; magazineReloadTime,
//   magazineReloadSwitchPhase; muzzlePos / muzzleEnd; OpticsModes; WeaponSlotsInfo slots (linkProxy, compatibleItems).
// Items (optics, muzzle, pointer, bipod): CfgWeapons >> <item> >> model and ItemInfo >> OpticsModes.
#include "ArmaConfig.h"
#include "Attachments.h"

#include <string>
#include <vector>

struct WeaponOptics
{
    std::string name;             // OpticsModes class (or "default" for entries on the weapon / ItemInfo itself)
    std::string cameraPoint;      // memoryPointCamera
    std::string cameraDir;        // cameraDir (memory point the view looks at; empty = along the weapon's z)
    float zoomMin = 0.75f, zoomMax = 0.75f, zoomInit = 0.75f;
    bool modelOptics = false;     // useModelOptics
    ConfigRef ref;
};

struct WeaponSlotCfg
{
    std::string name;                // MuzzleSlot / CowsSlot / PointerSlot / UnderBarrelSlot ...
    std::string linkProxy;           // \A3\data_f\proxies\weapon_slots\TOP
    std::vector<std::string> items;  // compatibleItems (array or class form)
    ConfigRef ref;
};

struct WeaponCfg
{
    bool found = false;
    std::string cls;                     // CfgWeapons class
    ConfigRef ref;
    std::vector<std::string> candidates; // every class with this model (first = chosen)
    bool pistol = false;                 // inherits Pistol / Pistol_Base_F (stance and character proxy)
    std::string reloadAction;
    ConfigRef reloadRef;
    std::vector<std::pair<std::string, std::string>> muzzleReloads; // other muzzles: muzzle class -> reloadAction
    std::string handAnim;                // RTM (Arma path) for the character skeleton
    std::string magazine, magazineModel; // first of magazines[] and its CfgMagazines modelSpecial (on the magazineslot proxy)
    ConfigRef handAnimRef;
    float magazineReloadTime = -1;       // -1 = not found
    ConfigRef magazineReloadTimeRef;
    float switchPhase = -1;              // magazineReloadSwitchPhase
    ConfigRef switchPhaseRef;
    std::string muzzlePos = "usti hlavne", muzzleEnd = "konec hlavne";
    std::vector<WeaponOptics> optics;    // ironsights (OpticsModes of the weapon, or its memoryPointCamera)
    std::vector<WeaponSlotCfg> slots;
};

// Model path as in config (\mymod\weapons\rifle.p3d); the character skeleton picks handAnim.
WeaponCfg ReadWeaponConfig(const ArmaConfig* cfg, const std::string& armaModelPath, const std::string& characterSkeleton);

struct ItemCfg
{
    bool found = false;
    std::string cls, model, displayName;
    std::vector<WeaponOptics> optics; // ItemInfo >> OpticsModes (optic items)
    ConfigRef ref;
};
ItemCfg ReadItemConfig(const ArmaConfig* cfg, const std::string& cls);

// Lower case, forward slashes to backslashes, no leading backslash, no .p3d: for comparing model paths.
std::string NormalizeModelPath(std::string p);
