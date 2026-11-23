#include "IconStandards.h"
#include "ArmaConfig.h"
#include "Text.h"
#include "Texture.h"

#include <algorithm>
#include <format>

#include "CoreGuard.h"

namespace
{
// medians of BI's pictures of the kind (fill and border of the opaque pixels, scope=2 classes of P:\a3); the border is
// the smallest free edge: the item keeps its proportions, the other axis follows from its shape
const IconStandard kStandards[] = {
    { "editorpreview", "editorPreview (Eden)", 455, 256, IconBackground::VrGround, false, 45.0f, 15.0f, 20.0f, 0, 0, 0, 0, IconAlign::Center,
      "editorPreview", "{}.jpg", "data\\editorpreviews" },
    { "weapon", "Weapon picture", 1024, 512, IconBackground::Transparent, false, 0.0f, 0.0f, 6.0f, 0.05f, 0.03f, 0.05f, 0.05f, IconAlign::Center,
      "picture", "gear_{}_X_ca.paa", "data\\ui" },
    { "gear", "Gear picture (uniform, vest, backpack, glasses)", 256, 256, IconBackground::Transparent, false, 0.0f, 0.0f, 8.0f, 0.05f, 0.05f, 0.04f, 0.02f,
      IconAlign::Center, "picture", "icon_{}_ca.paa", "data\\ui" },
    { "headgear", "Headgear picture (helmet, hat, NVG)", 256, 256, IconBackground::Transparent, false, -45.0f, 15.0f, 8.0f, 0.10f, 0.10f, 0.10f, 0.10f,
      IconAlign::Center, "picture", "icon_{}_ca.paa", "data\\ui" },
    { "attachment", "Attachment / magazine / item picture", 256, 256, IconBackground::Transparent, false, 0.0f, 0.0f, 8.0f, 0.07f, 0.07f, 0.07f, 0.07f,
      IconAlign::Center, "picture", "icon_{}_ca.paa", "data\\ui" },
    { "vehicle", "Vehicle picture (white silhouette)", 128, 64, IconBackground::Transparent, true, -90.0f, 0.0f, 6.0f, 0.01f, 0.01f, 0.02f, 0.01f,
      IconAlign::Bottom, "picture", "{}_ca.paa", "data\\ui" },
    { "mapicon", "Vehicle map icon (white, from above)", 512, 512, IconBackground::Transparent, true, 180.0f, 89.5f, 6.0f, 0.03f, 0.03f, 0.02f, 0.02f,
      IconAlign::Center, "icon", "map_{}_ca.paa", "data\\ui" },
    { "custom", "Custom", 512, 512, IconBackground::Transparent, false, 45.0f, 15.0f, 10.0f, 0.05f, 0.05f, 0.05f, 0.05f, IconAlign::Center,
      "picture", "{}_ca.paa", "data\\ui" },
};
static_assert(std::size(kStandards) == size_t(IconPreset::Count));

bool Inherits(const CfgClass* c, std::initializer_list<std::string_view> bases)
{
    for (const CfgClass* k : c->Chain())
        for (std::string_view b : bases)
            if (IEquals(k->name, b) || IEquals(k->baseName, b)) return true; // a base only declared ("class X;") ends the chain
    return false;
}
} // namespace

const IconStandard& Standard(IconPreset p) { return kStandards[std::clamp(int(p), 0, int(IconPreset::Count) - 1)]; }

IconPreset IconPresetById(std::string_view id)
{
    for (int i = 0; i < int(IconPreset::Count); ++i)
        if (IEquals(kStandards[i].id, id)) return IconPreset(i);
    return IconPreset::Count;
}

IconPreset SuggestIconPreset(const ArmaConfig& cfg, const std::string& cfgRoot, const std::string& cls)
{
    if (const IconCategory k = SuggestIconCategory(cfg, cfgRoot, cls); k != IconCategory::Count) return CategoryInfo(k).preset;
    return IconPreset::Count;
}

IconSubject IconSubjectOf(const ArmaConfig& cfg, const std::string& cfgRoot, const std::string& cls, const std::filesystem::path& projectRoot)
{
    IconSubject s;
    const CfgClass* root = cfg.Root().FindClass(cfgRoot, false);
    const CfgClass* c = root ? root->FindClass(cls, false) : nullptr;
    if (!c) return s;
    const CfgClass* skinClass = c; // whose hiddenSelections re-texture the model
    std::string skinRoot = cfgRoot;
    const CfgClass* ii = IEquals(cfgRoot, "CfgWeapons") ? c->FindClass("ItemInfo") : nullptr;
    const std::string uniformClass = ii ? ii->GetString("uniformClass") : std::string();
    const std::string uniformModel = ii ? ii->GetString("uniformModel") : std::string();
    if (!uniformClass.empty())
    {
        // a uniform: worn as its soldier class (model, camo), like the Unit window dresses it
        const CfgClass* veh = cfg.Root().FindClass("CfgVehicles", false);
        if (const CfgClass* soldier = veh ? veh->FindClass(uniformClass) : nullptr)
        {
            s.modelArma = soldier->GetString("model");
            skinClass = soldier;
            skinRoot = "CfgVehicles";
            s.why = "uniformClass " + soldier->name;
        }
    }
    if (s.modelArma.empty() && !uniformModel.empty() && uniformModel != "-")
    {
        s.modelArma = uniformModel;
        s.why = "ItemInfo uniformModel";
    }
    if (s.modelArma.empty())
    {
        s.modelArma = c->GetString("model");
        s.why = "model";
    }
    if (!s.modelArma.empty())
    {
        std::string m = s.modelArma;
        if (std::filesystem::path(m).extension().empty()) m += ".p3d";
        std::error_code ec;
        const std::filesystem::path p = ResolveArmaPath(m, projectRoot);
        if (std::filesystem::exists(p, ec)) s.model = p;
    }
    s.skin.cfgRoot = skinRoot;
    s.skin.className = skinClass->name;
    s.skin.selections = skinClass->GetStrings("hiddenSelections");
    if (s.skin.selections.empty() && ii && skinClass == c) s.skin.selections = ii->GetStrings("hiddenSelections");
    s.skin.textures = skinClass->GetStrings("hiddenSelectionsTextures");
    s.skin.materials = skinClass->GetStrings("hiddenSelectionsMaterials");
    s.skin.where = cfg.Where(skinClass->file, skinClass->line);
    s.skin.file = cfg.FileOf(skinClass->file);
    s.skin.line = skinClass->line;
    return s;
}

namespace
{
const IconCategoryInfo kCategories[] = {
    { "rifle", "Rifle / MG / DMR", IconPreset::Weapon },
    { "pistol", "Pistol", IconPreset::Weapon },
    { "launcher", "Launcher", IconPreset::Weapon },
    { "uniform", "Uniform", IconPreset::Gear },
    { "vest", "Vest", IconPreset::Gear },
    { "headgear", "Headgear (helmet, hat, NVG)", IconPreset::Headgear },
    { "backpack", "Backpack", IconPreset::Gear },
    { "glasses", "Glasses", IconPreset::Gear },
    { "optic", "Optic", IconPreset::Attachment },
    { "attachment", "Muzzle / pointer / bipod / item", IconPreset::Attachment },
    { "magazine", "Magazine", IconPreset::Attachment },
    { "vehicle", "Vehicle (white silhouette)", IconPreset::VehiclePicture },
    { "mapicon", "Map icon (vehicle from above)", IconPreset::VehicleMapIcon },
    { "editorpreview", "editorPreview (Eden)", IconPreset::EditorPreview },
    { "custom", "Custom", IconPreset::Custom },
};
static_assert(std::size(kCategories) == size_t(IconCategory::Count));

// picture sizes BI ships besides the standard's (measured once on the scope=2 classes of the game data); a mod's picture
// of one of these sizes is not reported
struct OtherSize { IconCategory category; int width, height; };
const OtherSize kOtherSizes[] = {
    { IconCategory::Rifle, 512, 256 }, { IconCategory::Pistol, 512, 256 }, { IconCategory::Launcher, 512, 256 },
    { IconCategory::Magazine, 512, 512 }, { IconCategory::EditorPreview, 410, 256 },
};

// the first base of the chain among `bases` (its name), "" if none
std::string BaseAmong(const CfgClass* c, std::initializer_list<std::string_view> bases)
{
    for (const CfgClass* k : c->Chain())
        for (std::string_view b : bases)
            if (IEquals(k->name, b) || IEquals(k->baseName, b)) return std::string(b);
    return {};
}
} // namespace

const IconCategoryInfo& CategoryInfo(IconCategory c) { return kCategories[std::clamp(int(c), 0, int(IconCategory::Count) - 1)]; }

IconCategory IconCategoryById(std::string_view id)
{
    for (int i = 0; i < int(IconCategory::Count); ++i)
        if (IEquals(kCategories[i].id, id)) return IconCategory(i);
    return IconCategory::Count;
}

IconCategory SuggestIconCategory(const ArmaConfig& cfg, const std::string& cfgRoot, const std::string& cls, std::string* reason)
{
    std::string why;
    auto done = [&](IconCategory c, std::string r) {
        if (reason) *reason = std::move(r);
        return c;
    };
    const CfgClass* root = cfg.Root().FindClass(cfgRoot, false);
    const CfgClass* c = root ? root->FindClass(cls, false) : nullptr;
    if (!c) return done(IconCategory::Count, {});
    if (IEquals(cfgRoot, "CfgGlasses")) return done(IconCategory::Glasses, "CfgGlasses");
    if (IEquals(cfgRoot, "CfgMagazines")) return done(IconCategory::Magazine, "CfgMagazines");
    if (IEquals(cfgRoot, "CfgWeapons"))
    {
        if (!(why = BaseAmong(c, { "Pistol_Base_F" })).empty()) return done(IconCategory::Pistol, why);
        if (!(why = BaseAmong(c, { "Launcher_Base_F" })).empty()) return done(IconCategory::Launcher, why);
        if (!(why = BaseAmong(c, { "Rifle_Long_Base_F", "Rifle_Base_F" })).empty()) return done(IconCategory::Rifle, why);
        if (!(why = BaseAmong(c, { "Uniform_Base" })).empty()) return done(IconCategory::Uniform, why);
        if (!(why = BaseAmong(c, { "Vest_Camo_Base", "Vest_NoCamo_Base" })).empty()) return done(IconCategory::Vest, why);
        if (!(why = BaseAmong(c, { "HelmetBase", "H_HelmetB", "NVGoggles" })).empty()) return done(IconCategory::Headgear, why);
        if (const CfgClass* ii = c->FindClass("ItemInfo"))
        {
            const int type = int(ii->GetNumber("type", 0));
            const std::string t = std::format("ItemInfo type {}", type);
            if (type == 801 || !ii->GetString("uniformClass").empty()) return done(IconCategory::Uniform, t);
            if (type == 701) return done(IconCategory::Vest, t);
            if (type == 605 || type == 616) return done(IconCategory::Headgear, t);
            if (type == 201) return done(IconCategory::Optic, t);
            if (!(why = BaseAmong(ii, { "InventoryOpticsItem_Base_F" })).empty()) return done(IconCategory::Optic, "ItemInfo: " + why);
        }
        return done(IconCategory::Attachment, "CfgWeapons (other)");
    }
    if (IEquals(cfgRoot, "CfgVehicles"))
    {
        if (!(why = BaseAmong(c, { "Bag_Base" })).empty()) return done(IconCategory::Backpack, why);
        if (!(why = BaseAmong(c, { "CAManBase", "Animal" })).empty()) return done(IconCategory::EditorPreview, why);
        if (!(why = BaseAmong(c, { "Car", "Tank", "Helicopter", "Plane", "Ship", "StaticWeapon", "AllVehicles" })).empty())
            return done(IconCategory::VehicleSilhouette, why);
        return done(IconCategory::EditorPreview, "CfgVehicles (object)");
    }
    return done(IconCategory::Custom, cfgRoot);
}

std::vector<IconExample> BiExamples(const ArmaConfig& game, IconCategory c, const std::filesystem::path& projectRoot, size_t max)
{
    std::vector<IconExample> out;
    if (c == IconCategory::Custom || c == IconCategory::Count) return out;
    const IconPreset preset = CategoryInfo(c).preset;
    const IconStandard& st = Standard(preset);
    const char* key = c == IconCategory::EditorPreview ? "editorPreview" : c == IconCategory::MapIcon ? "icon" : "picture";
    static const char* const kRoots[] = { "CfgWeapons", "CfgVehicles", "CfgGlasses", "CfgMagazines" };
    for (const char* rootName : kRoots)
    {
        const CfgClass* root = game.Root().FindClass(rootName, false);
        if (!root) continue;
        for (const auto& k : root->classes)
        {
            if (out.size() >= max * 4) break; // enough candidates
            if (k->external || k->GetNumber("scope", 0) != 2) continue;
            const CfgEntry* e = k->Find(key, false); // the class's own picture (not one inherited from its base)
            if (!e) continue;
            std::string v = k->GetString(key);
            if (v.size() < 4 || (v.find('\\') == std::string::npos && v.find('/') == std::string::npos)) continue; // CfgVehicleIcons names
            if (SuggestIconCategory(game, rootName, k->name) != c) continue;
            std::filesystem::path file = ResolveArmaPath(v, projectRoot);
            if (!file.has_extension()) file += ".paa";
            std::error_code ec;
            if (!std::filesystem::exists(file, ec)) continue;
            const PictureFileInfo info = ReadPictureInfo(file);
            if (!info.ok) continue;
            IconExample x;
            x.category = c;
            x.cls = k->name;
            x.file = file;
            x.config = game.FileOf(k->file);
            x.line = k->line;
            x.width = info.width;
            x.height = info.height;
            out.push_back(std::move(x));
        }
    }
    // the standard's size first, then by class name (stable)
    std::stable_sort(out.begin(), out.end(), [&](const IconExample& a, const IconExample& b) {
        const bool sa = a.width == st.width && a.height == st.height, sb = b.width == st.width && b.height == st.height;
        return sa != sb ? sa : ToLower(a.cls) < ToLower(b.cls);
    });
    if (out.size() > max) out.resize(max);
    return out;
}

std::string IconFileName(IconPreset p, const std::string& cls) { return std::vformat(Standard(p).fileFormat, std::make_format_args(cls)); }

std::string IconConfigLine(IconPreset p, const std::string& armaPath)
{
    std::string path = armaPath;
    std::replace(path.begin(), path.end(), '/', '\\');
    if (!path.starts_with("\\")) path = "\\" + path;
    return std::format("{} = \"{}\";", Standard(p).configKey, path);
}

std::string EditorPreviewSqf(const std::vector<std::string>& classes, float delaySeconds)
{
    std::string list;
    for (const auto& c : classes) list += (list.empty() ? "\"" : ", \"") + c + "\"";
    // parameters of BIS_fnc_exportEditorPreviews: delay, objects, sides, mods, addons, classes; it sleeps: spawn
    return std::format("[{:g}, \"all\", [], [], [], [{}]] spawn BIS_fnc_exportEditorPreviews;", delaySeconds, list);
}

bool PaaSizeOk(int width, int height)
{
    auto pot = [](int v) { return v >= 2 && v <= 4096 && (v & (v - 1)) == 0; };
    return pot(width) && pot(height);
}

const char* IconViewText(IconPreset p)
{
    switch (p)
    {
    case IconPreset::EditorPreview: return "3/4 from the front-right, a little above (BIS_fnc_exportEditorPreviews)";
    case IconPreset::Weapon: return "side view, muzzle left";
    case IconPreset::Gear: return "front view";
    case IconPreset::Headgear: return "3/4 from the front-left, a little above";
    case IconPreset::Attachment: return "side view";
    case IconPreset::VehiclePicture: return "side view, nose left, on the bottom edge";
    case IconPreset::VehicleMapIcon: return "from above, nose up";
    default: return "free";
    }
}

const char* IconBackgroundText(IconPreset p)
{
    if (p == IconPreset::EditorPreview) return "VR ground with shadow, no alpha";
    if (Standard(p).whiteSilhouette) return "white silhouette, transparent (alpha)";
    return "transparent (alpha)";
}

IconNameParts IconNameOf(IconPreset p, const std::string& cls)
{
    IconNameParts n;
    const std::string fmt = Standard(p).fileFormat;
    const size_t at = fmt.find("{}");
    n.prefix = fmt.substr(0, at);
    std::string rest = at == std::string::npos ? std::string() : fmt.substr(at + 2);
    const size_t dot = rest.rfind('.');
    n.ext = dot == std::string::npos ? std::string() : rest.substr(dot);
    n.suffix = rest.substr(0, dot);
    n.stem = cls;
    n.file = n.prefix + n.stem + n.suffix + n.ext;
    switch (p)
    {
    case IconPreset::Weapon: n.why = "gear_ ... _X_ca: BI's weapon pictures (gear_mx_rifle_x_ca.paa)"; break;
    case IconPreset::Gear:
    case IconPreset::Headgear:
    case IconPreset::Attachment: n.why = "icon_ ... _ca: BI's gear / item pictures (icon_u_b_combatuniform_mcam_ca.paa)"; break;
    case IconPreset::VehiclePicture: n.why = "<class>_ca: BI's vehicle pictures (apc_tracked_01_base_ca.paa)"; break;
    case IconPreset::VehicleMapIcon: n.why = "map_ ... _ca: BI's map icons (map_apc_tracked_01_ca.paa)"; break;
    case IconPreset::EditorPreview: n.why = "<Class>.jpg: the file name is the CfgVehicles class (BIS_fnc_exportEditorPreviews)"; break;
    default: n.why = "_ca: a picture with transparency"; break;
    }
    if (n.suffix.ends_with("_ca"))
        n.why += "; _ca = colour + alpha: TexConvert keeps the transparent background (DXT5), _co would drop it (DXT1)";
    else if (n.ext == ".jpg")
        n.why += "; JPG: no alpha, the editor shows it as is";
    return n;
}

namespace
{
int Be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
int Be32(const uint8_t* p) { return int((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]); }
} // namespace

PictureFileInfo ReadPictureInfo(const std::filesystem::path& file)
{
    PictureFileInfo info;
    std::vector<uint8_t> b;
    if (!ReadFileBytes(file, b)) { info.error = "cannot read the file"; return info; }
    const std::string ext = ToLower(PathToUtf8(file.extension()));
    if (b.size() >= 26 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')
    {
        info.width = Be32(&b[16]);
        info.height = Be32(&b[20]);
        info.alpha = b[25] == 4 || b[25] == 6;
        info.format = "PNG";
        info.ok = true;
        return info;
    }
    if (b.size() >= 4 && b[0] == 0xFF && b[1] == 0xD8)
    {
        // segments up to the frame header (SOF0..15 but DHT C4, JPG C8, DAC CC)
        for (size_t i = 2; i + 9 < b.size();)
        {
            if (b[i] != 0xFF) { ++i; continue; }
            const uint8_t m = b[i + 1];
            if (m == 0xFF) { ++i; continue; }
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC)
            {
                info.height = Be16(&b[i + 5]);
                info.width = Be16(&b[i + 7]);
                info.format = "JPG";
                info.ok = true;
                return info;
            }
            if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            i += 2 + size_t(Be16(&b[i + 2]));
        }
        info.error = "no JPG frame header";
        return info;
    }
    if (ext == ".paa")
    {
        Image img;
        std::string err;
        if (!DecodePaa(b, img, err)) { info.error = err.empty() ? "not a PAA" : err; return info; }
        info.width = int(img.width);
        info.height = int(img.height);
        info.alpha = img.format != Image::Format::BC1;
        info.format = img.format == Image::Format::BC1 ? "DXT1" : img.format == Image::Format::BC2 ? "DXT3" : img.format == Image::Format::BC3 ? "DXT5" : "ARGB";
        info.ok = true;
        return info;
    }
    info.error = "unknown picture format";
    return info;
}

std::vector<IconPictureIssue> CheckConfigPictures(const ArmaConfig& cfg, const std::filesystem::path& projectRoot, const std::string& only,
                                                  const std::function<IconCategory(const std::string& cls)>& categoryOf)
{
    std::vector<IconPictureIssue> out;
    for (const char* rootName : { "CfgWeapons", "CfgVehicles", "CfgGlasses", "CfgMagazines" })
    {
        const CfgClass* root = cfg.Root().FindClass(rootName, false);
        if (!root) continue;
        const bool vehicles = IEquals(rootName, "CfgVehicles");
        for (const auto& k : root->classes)
        {
            if (k->external || (!only.empty() && !IEquals(k->name, only))) continue;
            if (only.empty() && k->GetNumber("scope", 0) != 2) continue;
            IconCategory cat = categoryOf ? categoryOf(k->name) : IconCategory::Count;
            if (cat == IconCategory::Count) cat = SuggestIconCategory(cfg, rootName, k->name);
            for (const char* key : { "picture", "editorPreview", "icon" })
            {
                if (IEquals(key, "icon") && !vehicles) continue;
                const CfgEntry* e = k->Find(key, false); // written in the class itself
                if (!e || e->isArray || e->value.str.empty()) continue;
                std::string value = e->value.str;
                // a name of CfgVehicleIcons / a UI class ("iconMan"), not a file
                if (value.find('\\') == std::string::npos && value.find('/') == std::string::npos && value.find('.') == std::string::npos) continue;
                auto add = [&](std::string problem, bool error) {
                    out.push_back({ rootName, k->name, key, value, std::move(problem), error, cfg.FileOf(e->file), e->line });
                };
                std::string arma = value;
                if (std::filesystem::path(arma).extension().empty()) arma += ".paa"; // the game adds .paa
                const std::string norm = NormalizeArmaPath(arma);
                std::error_code ec;
                const std::filesystem::path file = ResolveArmaPath(arma, projectRoot);
                if (!std::filesystem::exists(file, ec))
                {
                    if (!norm.starts_with("a3\\")) add("the file is not found", true); // the game's own files are packed
                    continue;
                }
                IconPreset preset = IconPreset::Custom;
                if (IEquals(key, "editorPreview")) preset = IconPreset::EditorPreview;
                else if (IEquals(key, "icon")) preset = IconPreset::VehicleMapIcon;
                else if (cat != IconCategory::Count && cat != IconCategory::EditorPreview) preset = CategoryInfo(cat).preset;
                const IconStandard& st = Standard(preset);
                const PictureFileInfo info = ReadPictureInfo(file);
                if (!info.ok) { add(info.error, true); continue; }
                const std::string ext = ToLower(PathToUtf8(file.extension()));
                const std::string stem = ToLower(PathToUtf8(file.stem()));
                if (ext == ".paa" && !PaaSizeOk(info.width, info.height))
                    add(std::format("{} x {}: not a power of two (a PAA must be)", info.width, info.height), true);
                if (preset == IconPreset::Custom) continue; // no standard to compare with
                // the size: the standard's or one of BI's own pictures of the category
                bool sizeOk = info.width == st.width && info.height == st.height;
                if (cat != IconCategory::Count && preset != IconPreset::EditorPreview && preset != IconPreset::VehicleMapIcon)
                    for (const OtherSize& o : kOtherSizes) sizeOk |= o.category == cat && o.width == info.width && o.height == info.height;
                if (preset == IconPreset::VehicleMapIcon) sizeOk |= info.width == info.height && info.width >= 64; // BI: 64 .. 512 square
                if (!sizeOk) add(std::format("size {} x {}, BI's {}: {} x {}", info.width, info.height, st.name, st.width, st.height), false);
                if (preset == IconPreset::EditorPreview)
                {
                    if (ext != ".jpg") add(std::format("{}: BI's editor previews are JPG ({})", ext, st.fileFormat), false);
                    if (norm.find("editorpreviews") == std::string::npos) add("not in an EditorPreviews folder (BI: \\a3\\editorpreviews_f\\data\\cfgvehicles\\, a mod: data\\editorpreviews)", false);
                    continue;
                }
                if (stem.ends_with("_co"))
                    add("ending _co on a picture with a transparent background: BI names them _ca (TexConvert keeps the alpha)", false);
                else if (!stem.ends_with("_ca"))
                    add("no _ca ending: TexConvert picks the format by it (BI: " + std::string(st.fileFormat) + ")", false);
                else if (!info.alpha)
                    add(std::format("_ca but no alpha in the file ({}): the background is not transparent", info.format), false);
                if (const size_t slash = norm.rfind('\\'); slash == std::string::npos || !norm.substr(0, slash).ends_with("ui"))
                    add("not in a UI folder (BI: data\\ui)", false);
            }
        }
    }
    return out;
}
