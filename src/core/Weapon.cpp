#include "Weapon.h"

#include "Log.h"

#include <algorithm>

#include "CoreGuard.h"

namespace {

ConfigRef RefOf(const ArmaConfig* cfg, int file, int line)
{
    ConfigRef r;
    if (cfg)
    {
        r.file = cfg->FileOf(file);
        r.line = line;
    }
    return r;
}

ConfigRef EntryRef(const ArmaConfig* cfg, const CfgClass* c, std::string_view name)
{
    const CfgEntry* e = c ? c->Find(name) : nullptr;
    return e ? RefOf(cfg, e->file, e->line) : ConfigRef{};
}

bool Inherits(const CfgClass* c, std::string_view base)
{
    for (int guard = 0; c && guard < 64; c = c->base, ++guard)
        if (IEquals(c->name, base)) return true;
    return false;
}

WeaponOptics ReadOptics(const ArmaConfig* cfg, const CfgClass& c, const std::string& name)
{
    WeaponOptics o;
    o.name = name;
    o.cameraPoint = c.GetString("memoryPointCamera");
    o.cameraDir = c.GetString("cameraDir");
    o.zoomInit = float(c.GetNumber("opticsZoomInit", 0.75));
    o.zoomMin = float(c.GetNumber("opticsZoomMin", o.zoomInit));
    o.zoomMax = float(c.GetNumber("opticsZoomMax", o.zoomInit));
    o.modelOptics = c.GetNumber("useModelOptics", 0) != 0;
    o.ref = RefOf(cfg, c.file, c.line);
    return o;
}

// OpticsModes children, else the entries on the class itself (old style: memoryPointCamera on the weapon / ItemInfo)
std::vector<WeaponOptics> ReadOpticsModes(const ArmaConfig* cfg, const CfgClass& c)
{
    std::vector<WeaponOptics> out;
    if (const CfgClass* modes = c.FindClass("OpticsModes"))
        for (const CfgClass* m : MergedChildClasses(*modes)) out.push_back(ReadOptics(cfg, *m, m->name));
    if (out.empty() && !c.GetString("memoryPointCamera").empty()) out.push_back(ReadOptics(cfg, c, "default"));
    return out;
}

} // namespace

std::string NormalizeModelPath(std::string p)
{
    p = ToLower(p);
    std::replace(p.begin(), p.end(), '/', '\\');
    while (!p.empty() && p.front() == '\\') p.erase(p.begin());
    if (p.ends_with(".p3d")) p.resize(p.size() - 4);
    return p;
}

WeaponCfg ReadWeaponConfig(const ArmaConfig* cfg, const std::string& armaModelPath, const std::string& characterSkeleton)
{
    WeaponCfg w;
    const CfgClass* weapons = cfg ? cfg->Root().FindClass("CfgWeapons") : nullptr;
    if (!weapons) return w;
    const std::string want = NormalizeModelPath(armaModelPath);
    // classes with this model: own "model" entries first (variants inherit it), public ones (scope 2) first
    std::vector<std::pair<int, const CfgClass*>> found;
    for (const CfgClass* c : MergedChildClasses(*weapons))
    {
        const CfgEntry* m = c->Find("model");
        if (!m || m->value.type != CfgValue::Type::String || NormalizeModelPath(m->value.str) != want) continue;
        int rank = (c->Find("model", false) ? 0 : 2) + (c->GetNumber("scope", 0) >= 2 ? 0 : 1);
        if (c->GetString("reloadAction").empty()) rank += 4;
        found.emplace_back(rank, c);
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (found.empty()) return w;
    for (const auto& [r, c] : found) w.candidates.push_back(c->name);
    const CfgClass* c = found.front().second;
    w.found = true;
    w.cls = c->name;
    w.ref = RefOf(cfg, c->file, c->line);
    w.pistol = Inherits(c, "Pistol") || Inherits(c, "Pistol_Base_F") || c->GetNumber("type", 1) == 2;
    w.reloadAction = c->GetString("reloadAction");
    w.reloadRef = EntryRef(cfg, c, "reloadAction");
    if (c->HasNumber("magazineReloadTime"))
    {
        w.magazineReloadTime = float(c->GetNumber("magazineReloadTime"));
        w.magazineReloadTimeRef = EntryRef(cfg, c, "magazineReloadTime");
    }
    if (c->HasNumber("magazineReloadSwitchPhase"))
    {
        w.switchPhase = float(c->GetNumber("magazineReloadSwitchPhase"));
        w.switchPhaseRef = EntryRef(cfg, c, "magazineReloadSwitchPhase");
    }
    if (std::string s = c->GetString("muzzlePos"); !s.empty()) w.muzzlePos = s;
    if (std::string s = c->GetString("muzzleEnd"); !s.empty()) w.muzzleEnd = s;
    // handAnim[] = {"OFP2_ManSkeleton", "\path.rtm", "OFP2_ManSkeleton", ...}: pairs skeleton, RTM
    {
        const std::vector<std::string> h = c->GetStrings("handAnim");
        for (size_t i = 0; i + 1 < h.size(); i += 2)
            if (IEquals(h[i], characterSkeleton) || (w.handAnim.empty() && i == 0)) w.handAnim = h[i + 1];
        w.handAnimRef = EntryRef(cfg, c, "handAnim");
    }
    // other muzzles (UGL ...): muzzles[] = {"this", "EGLM"}
    for (const std::string& m : c->GetStrings("muzzles"))
    {
        if (IEquals(m, "this")) continue;
        if (const CfgClass* mc = c->FindClass(m))
            if (std::string a = mc->GetString("reloadAction"); !a.empty()) w.muzzleReloads.emplace_back(m, a);
    }
    // the magazine the game puts on the weapon's magazineslot proxy (binarized BI weapons have no magazine geometry)
    if (const std::vector<std::string> mags = c->GetStrings("magazines"); !mags.empty())
    {
        w.magazine = mags.front();
        if (const CfgClass* cm = cfg->Root().FindClass("CfgMagazines"))
            if (const CfgClass* m = cm->FindClass(w.magazine)) w.magazineModel = m->GetString("modelSpecial");
    }
    w.optics = ReadOpticsModes(cfg, *c);
    if (const CfgClass* slots = c->FindClass("WeaponSlotsInfo"))
        for (const CfgClass* s : MergedChildClasses(*slots))
        {
            WeaponSlotCfg sl;
            sl.name = s->name;
            sl.linkProxy = s->GetString("linkProxy");
            sl.ref = RefOf(cfg, s->file, s->line);
            if (const CfgEntry* e = s->Find("compatibleItems"); e && e->isArray)
            {
                for (const std::string& it : s->GetStrings("compatibleItems"))
                    if (!it.empty()) sl.items.push_back(it);
            }
            else if (const CfgClass* ci = s->FindClass("compatibleItems"))
            {
                // class form: item = 1; (the nearest definition of an item decides)
                std::vector<std::string> seen;
                for (const CfgClass* k : ci->Chain())
                    for (const auto& en : k->entries)
                    {
                        const std::string low = ToLower(en.name);
                        if (std::find(seen.begin(), seen.end(), low) != seen.end()) continue;
                        seen.push_back(low);
                        if (en.value.type == CfgValue::Type::Number && en.value.num > 0) sl.items.push_back(en.name);
                    }
            }
            if (sl.linkProxy.empty() && sl.items.empty()) continue; // not a slot (e.g. mass entries only)
            w.slots.push_back(std::move(sl));
        }
    return w;
}

ItemCfg ReadItemConfig(const ArmaConfig* cfg, const std::string& cls)
{
    ItemCfg it;
    const CfgClass* weapons = cfg ? cfg->Root().FindClass("CfgWeapons") : nullptr;
    const CfgClass* c = weapons ? weapons->FindClass(cls) : nullptr;
    if (!c) return it;
    it.found = true;
    it.cls = c->name;
    it.model = c->GetString("model");
    it.displayName = c->GetString("displayName");
    it.ref = RefOf(cfg, c->file, c->line);
    if (const CfgClass* info = c->FindClass("ItemInfo")) it.optics = ReadOpticsModes(cfg, *info);
    return it;
}
