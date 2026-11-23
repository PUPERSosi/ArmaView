#pragma once
// UI pictures of a mod's classes as Bohemia makes them (measured on the scope=2 classes of P:\a3, see docs/icons.md):
// per kind the size, background, view, the free border around the item, the file name and the config entry.
// The Icons mode renders by these; the numbers are the medians of BI's own pictures of the category.
#include "RvMaterial.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class ArmaConfig;

enum class IconPreset
{
    EditorPreview,  // editorPreview: JPG 455 x 256, VR ground with shadow, the camera of BIS_fnc_exportEditorPreviews
    Weapon,         // picture of a rifle / MG / pistol / launcher: 1024 x 512, transparent, side view, muzzle left
    Gear,           // picture of a uniform / vest / backpack / glasses: 256 x 256, transparent, front view
    Headgear,       // picture of a helmet / hat / NVG: 256 x 256, transparent, 3/4 from the front-left, a little above
    Attachment,     // picture of an optic / muzzle / pointer / magazine / item: 256 x 256, transparent, side view
    VehiclePicture, // picture of a vehicle: 128 x 64 white silhouette, side view, nose left, standing on the bottom edge
    VehicleMapIcon, // icon of a vehicle: 512 x 512 white silhouette, from above, nose up
    Custom,
    Count
};

enum class IconBackground { Transparent, VrGround };
enum class IconAlign { Center, Bottom };

struct IconStandard
{
    const char* id;          // "weapon", for the ui-script and the settings
    const char* name;        // shown in the UI
    int width, height;       // the picture as BI ships it
    IconBackground background;
    bool whiteSilhouette;    // vehicles: the shape in white, no shading
    float yawDeg;            // camera around the item from its front, towards its right (90: the right side);
                             // weapons / attachments: from the side view with the muzzle left (the renderer finds it)
    float pitchDeg;          // camera above the item
    float fovDeg;            // narrow: a near-orthographic look like BI's renders
    float marginL, marginR, marginT, marginB; // free border, fraction of the picture (the item is fitted inside)
    IconAlign align;
    const char* configKey;   // picture / icon / editorPreview
    const char* fileFormat;  // std::format pattern with the class name: "gear_{}_X_ca.paa"
    const char* folder;      // inside the addon: "data\\ui"
};

const IconStandard& Standard(IconPreset p);
IconPreset IconPresetById(std::string_view id); // Count if unknown

// The picture kind for a class of the config (CfgWeapons / CfgVehicles / CfgGlasses / CfgMagazines) by its
// inheritance (Rifle_Base_F, Uniform_Base, HelmetBase, Bag_Base, Car, ...; attachments by their ItemInfo).
// Vehicles: VehiclePicture (their map icon is the second picture). Count: the class is not found.
IconPreset SuggestIconPreset(const ArmaConfig& cfg, const std::string& cfgRoot, const std::string& cls);

// What a class's picture shows: the model as worn (ItemInfo uniformModel; a uniform: the model of its uniformClass
// soldier, as the Unit window dresses it) or its own model, and its re-texture (hiddenSelections / ...Textures /
// ...Materials of the class, of the uniformClass for a uniform) as a config skin ("" hides a selection).
struct IconSubject
{
    std::string modelArma;          // Arma path as in the config
    std::filesystem::path model;    // on disk ("" when not found)
    ConfigSkin skin;                // empty selections: the model's own textures
    std::string why;                // "ItemInfo uniformModel", "uniformClass Test_Soldier_base_F", "model"
};
IconSubject IconSubjectOf(const ArmaConfig& cfg, const std::string& cfgRoot, const std::string& cls, const std::filesystem::path& projectRoot);

// The kind of item finer than the preset (which BI pictures to compare with, which standard row applies); a class gets
// one by its inheritance and the user may pick another.
enum class IconCategory
{
    Rifle, Pistol, Launcher, Uniform, Vest, Headgear, Backpack, Glasses, Optic, Attachment, Magazine,
    VehicleSilhouette, MapIcon, EditorPreview, Custom, Count
};
struct IconCategoryInfo
{
    const char* id;   // "vest"
    const char* name; // shown
    IconPreset preset;
};
const IconCategoryInfo& CategoryInfo(IconCategory c);
IconCategory IconCategoryById(std::string_view id); // Count if unknown
// the category of a class by its inheritance; reason: the base class (or ItemInfo type) it was decided by
IconCategory SuggestIconCategory(const ArmaConfig& cfg, const std::string& cfgRoot, const std::string& cls, std::string* reason = nullptr);

// BI's own pictures of a category, found in the game configs on the user's P: drive while the viewer runs (nothing of
// them is shipped): scope=2 classes of the category with their own picture / icon / editorPreview file on disk, those
// of the standard's size first. Empty without the game data.
struct IconExample
{
    IconCategory category = IconCategory::Custom;
    std::string cls;
    std::filesystem::path file;   // the picture on disk
    std::filesystem::path config; // the config file of the class, and its line
    int line = 0;
    int width = 0, height = 0;
};
std::vector<IconExample> BiExamples(const ArmaConfig& game, IconCategory c, const std::filesystem::path& projectRoot, size_t max = 8);

// "gear_MyRifle_F_X_ca.paa", "MyTank_F.jpg"
std::string IconFileName(IconPreset p, const std::string& cls);
// the config line: picture = "\mymod\data\ui\gear_myrifle_x_ca.paa"; (armaPath: the file's Arma path)
std::string IconConfigLine(IconPreset p, const std::string& armaPath);
// the game call that makes editor previews of these classes only (BIS_fnc_exportEditorPreviews: Screenshots\EditorPreviews)
std::string EditorPreviewSqf(const std::vector<std::string>& classes, float delaySeconds = 1.0f);
// whether the size can go to PAA (powers of two, 2 .. 4096)
bool PaaSizeOk(int width, int height);

// how BI looks at the item for the kind ("side view, muzzle left") and the background / alpha in words
const char* IconViewText(IconPreset p);
const char* IconBackgroundText(IconPreset p);

// the file name by BI's rule in parts: prefix + class + ending + extension, and why each part is so
struct IconNameParts
{
    std::string prefix;  // "gear_", "icon_", "map_", ""
    std::string stem;    // the class name
    std::string suffix;  // "_X_ca", "_ca", ""
    std::string ext;     // ".paa", ".jpg"
    std::string file;    // all of it
    std::string why;     // where the prefix / ending come from (BI's files of the kind, alpha)
};
IconNameParts IconNameOf(IconPreset p, const std::string& cls);

// size and alpha of a picture file (PAA: DXT1 has no alpha, DXT5 / ARGB has; PNG: colour type; JPG: none)
struct PictureFileInfo
{
    bool ok = false;
    int width = 0, height = 0;
    bool alpha = false;
    std::string format; // "DXT5", "JPG", "PNG"
    std::string error;
};
PictureFileInfo ReadPictureInfo(const std::filesystem::path& file);

// the pictures a mod's config already names (picture / editorPreview, icon of CfgVehicles) checked against BI's
// standard of the class's category: the file is there, its size, its ending (_ca with alpha, not _co), the folder
struct IconPictureIssue
{
    std::string root, cls, key, value; // CfgWeapons, my_vest, picture, "\mymod\data\ui\icon_my_vest_co.paa"
    std::string problem;
    bool error = false;                // the file is missing / unreadable / cannot be a PAA; else a warning
    std::filesystem::path config;      // the line of the entry
    int line = 0;
};
// cls: only that class ("" all scope=2 classes of the config's own roots); categoryOf: the user's category of a class
// (Count: by inheritance)
std::vector<IconPictureIssue> CheckConfigPictures(const ArmaConfig& cfg, const std::filesystem::path& projectRoot, const std::string& cls = {},
                                                  const std::function<IconCategory(const std::string& cls)>& categoryOf = {});
