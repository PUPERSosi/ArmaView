#include "Unit.h"
#include "Log.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>

#include "CoreGuard.h"

namespace fs = std::filesystem;

namespace {

// root classes of that name (the merged game configs keep one per source file)
std::vector<const CfgClass*> RootClasses(const ArmaConfig& cfg, std::string_view root)
{
    std::vector<const CfgClass*> out;
    for (const auto& c : cfg.Root().classes)
        if (!c->external && IEquals(c->name, root)) out.push_back(c.get());
    return out;
}

// classes of a root in the config and its fallback (addon first), by lower-case name
template <class Fn> void ForEachClass(const ArmaConfig& cfg, std::string_view root, Fn&& fn)
{
    std::set<std::string> seen;
    for (const ArmaConfig* c = &cfg; c; c = c->Fallback())
        for (const CfgClass* r : RootClasses(*c, root))
            for (const auto& k : r->classes)
                if (!k->external && seen.insert(ToLower(k->name)).second) fn(*k);
}

std::string WithP3d(std::string p)
{
    while (!p.empty() && (p.front() == '\\' || p.front() == '/')) p.erase(p.begin());
    const std::string l = ToLower(p);
    if (!l.ends_with(".p3d")) p += ".p3d";
    return p;
}

int ItemType(const CfgClass& c)
{
    const CfgClass* info = c.FindClass("ItemInfo");
    return info ? int(info->GetNumber("type", -1)) : -1;
}

bool Intersects(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    for (const auto& x : a)
        for (const auto& y : b)
            if (!x.empty() && IEquals(x, y)) return true;
    return false;
}

// CfgFaces >> <faceType> >> <face>; also finds a face under another face type (faceType naming a face is a config
// mistake reported by the caller)
const CfgClass* FindFace(const ArmaConfig& cfg, const std::string& faceType, const std::string& face, std::string* foundUnder)
{
    if (const CfgClass* t = FindConfigClass(cfg, "CfgFaces", faceType))
        if (const CfgClass* f = t->FindClass(face)) return f;
    const CfgClass* hit = nullptr;
    ForEachClass(cfg, "CfgFaces", [&](const CfgClass& t) {
        if (hit) return;
        for (const auto& f : t.classes)
            if (!f->external && IEquals(f->name, face)) { hit = f.get(); if (foundUnder) *foundUnder = t.name; return; }
    });
    return hit;
}

} // namespace

const CfgClass* FindConfigClass(const ArmaConfig& cfg, std::string_view root, std::string_view cls)
{
    for (const ArmaConfig* c = &cfg; c; c = c->Fallback())
        for (const CfgClass* r : RootClasses(*c, root))
            if (const CfgClass* k = r->FindClass(cls, false))
            {
                if (!k->external) return k;
                if (k->base) return k->base; // "class X;" declared here, defined elsewhere
            }
    return nullptr;
}

const char* UnitSlotName(UnitSlot s)
{
    static const char* names[] = { "head", "headgear", "goggles", "vest", "nvg", "backpack", "other" };
    return s < UnitSlot::Count ? names[int(s)] : "?";
}

bool ParseUnitSlot(std::string_view name, UnitSlot& out)
{
    for (int i = 0; i < int(UnitSlot::Count); ++i)
        if (IEquals(name, UnitSlotName(UnitSlot(i)))) { out = UnitSlot(i); return true; }
    return false;
}

void Unit::Clear()
{
    cls_.clear();
    model_.clear();
    outfit_ = false;
    outfitBody_.clear();
    faceType_.clear();
    identityTypes_.clear();
    uniformSkin_ = {};
    parts_.clear();
}

bool Unit::Read(const ArmaConfig& cfg, const std::string& cls)
{
    auto& log = Log::Get();
    const CfgClass* c = FindConfigClass(cfg, "CfgVehicles", cls);
    if (!c)
    {
        log.Error("Unit: CfgVehicles::{} not found (the addon's config.cpp + the game configs are searched)", cls);
        return false;
    }
    const bool sameClass = IEquals(cls_, c->name);
    std::vector<UnitPart> keep;
    if (sameClass) keep = std::move(parts_); // hot reload: models already loaded are kept by Load
    Clear();
    cls_ = c->name;
    model_ = c->GetString("model");
    faceType_ = c->GetString("faceType");
    identityTypes_ = c->GetStrings("identityTypes");
    uniformSkin_.cfgRoot = "CfgVehicles";
    uniformSkin_.className = c->name;
    uniformSkin_.selections = c->GetStrings("hiddenSelections");
    uniformSkin_.textures = c->GetStrings("hiddenSelectionsTextures");
    uniformSkin_.materials = c->GetStrings("hiddenSelectionsMaterials");
    uniformSkin_.where = cfg.Where(c->file, c->line);
    log.Info("Unit: CfgVehicles::{} - model {}, uniformClass {}, {} hidden selection(s)", cls_, model_.empty() ? "(none)" : model_,
             c->GetString("uniformClass", "(none)"), uniformSkin_.selections.size());
    if (model_.empty()) log.Warn("Unit: CfgVehicles::{} has no model=", cls_);

    // head: the face of the identity
    {
        std::string under;
        const CfgClass* type = FindConfigClass(cfg, "CfgFaces", faceType_);
        const CfgClass* face = nullptr;
        std::string why;
        if (!type && !faceType_.empty())
        {
            face = FindFace(cfg, faceType_, faceType_, &under);
            if (face)
                log.Warn("Unit: faceType=\"{}\" is a face (CfgFaces >> {} >> {}), not a face type (a class of CfgFaces) - the game "
                         "looks for CfgFaces >> {} and will not find it; the viewer shows that face",
                         faceType_, under, faceType_, faceType_);
            else
                why = std::format("faceType \"{}\" is not a class of CfgFaces", faceType_);
        }
        else if (faceType_.empty())
            why = "no faceType=";
        if (type)
        {
            const CfgClass* fallback = nullptr;
            const CfgClass* disabledMatch = nullptr;
            for (const CfgClass* f : MergedChildClasses(*type))
            {
                if (f->GetString("head").empty()) continue;
                const bool disabled = f->GetNumber("disabled", 0) != 0;
                if (Intersects(f->GetStrings("identityTypes"), identityTypes_))
                {
                    if (!disabled) { face = f; break; }
                    if (!disabledMatch) disabledMatch = f;
                }
                if (!disabled && !fallback) fallback = f;
            }
            if (!face) face = disabledMatch ? disabledMatch : fallback;
            if (!face) why = std::format("CfgFaces >> {} has no face with a head", faceType_);
        }
        if (face)
        {
            UnitPart p = ReadPart(cfg, UnitSlot::Head, face->name, "face (identityTypes; the game picks one at random)");
            parts_.push_back(std::move(p));
        }
        else
        {
            UnitPart p;
            p.slot = UnitSlot::Head;
            p.from = "face";
            p.problem = why;
            parts_.push_back(std::move(p));
            log.Warn("Unit: {}: no head - {}", cls_, why);
        }
    }
    // worn items
    for (const auto& item : c->GetStrings("linkedItems"))
    {
        const CfgClass* w = FindConfigClass(cfg, "CfgWeapons", item);
        if (!w)
        {
            log.Warn("Unit: linkedItems \"{}\": not in CfgWeapons (its addon is not loaded?) - skipped", item);
            continue;
        }
        const int type = ItemType(*w);
        UnitSlot slot;
        if (type == 605) slot = UnitSlot::Headgear;
        else if (type == 701) slot = UnitSlot::Vest;
        else if (type == 616) slot = UnitSlot::NVG;
        else
        {
            log.Debug("Unit: linkedItems \"{}\" (ItemInfo type {}) is not worn visibly", item, type);
            continue;
        }
        parts_.push_back(ReadPart(cfg, slot, w->name, "linkedItems[]"));
    }
    if (const std::string pack = c->GetString("backpack"); !pack.empty()) parts_.push_back(ReadPart(cfg, UnitSlot::Backpack, pack, "backpack="));
    // parts added from files stay (hot reload of the class)
    for (auto& k : keep)
        if (k.fromFile) parts_.push_back(std::move(k));
    // same parts as before (hot reload): the loaded models stay
    for (auto& p : parts_)
        for (auto& k : keep)
            if (k.slot == p.slot && IEquals(k.cls, p.cls) && k.file == p.file && k.model)
            {
                p.model = k.model;
                p.skin = std::move(k.skin);
                p.enabled = k.enabled;
            }
    return true;
}

UnitPart Unit::ReadPart(const ArmaConfig& cfg, UnitSlot slot, const std::string& cls, const std::string& from) const
{
    UnitPart p;
    p.slot = slot;
    p.cls = cls;
    p.from = from;
    if (slot == UnitSlot::Head)
    {
        const CfgClass* face = FindFace(cfg, faceType_, cls, nullptr);
        if (!face) { p.problem = std::format("face {} not in CfgFaces", cls); return p; }
        p.cls = face->name;
        const std::string head = face->GetString("head");
        const CfgClass* h = FindConfigClass(cfg, "CfgHeads", head);
        if (!h) { p.problem = std::format("CfgHeads::{} (head of face {}) not found", head, face->name); return p; }
        p.armaModel = h->GetString("model");
        if (p.armaModel.empty()) { p.problem = std::format("CfgHeads::{} has no model=", head); return p; }
        p.note = std::format("CfgHeads::{}", head);
        return p;
    }
    if (slot == UnitSlot::Backpack)
    {
        const CfgClass* b = FindConfigClass(cfg, "CfgVehicles", cls);
        if (!b) { p.problem = std::format("CfgVehicles::{} not found", cls); return p; }
        p.cls = b->name;
        p.armaModel = b->GetString("model");
        if (p.armaModel.empty()) p.problem = std::format("CfgVehicles::{} has no model=", cls);
        return p;
    }
    const CfgClass* w = FindConfigClass(cfg, slot == UnitSlot::Goggles ? "CfgGlasses" : "CfgWeapons", cls);
    if (!w) { p.problem = std::format("{}::{} not found", slot == UnitSlot::Goggles ? "CfgGlasses" : "CfgWeapons", cls); return p; }
    p.cls = w->name;
    if (slot == UnitSlot::Goggles) { p.armaModel = w->GetString("model"); return p; }
    const CfgClass* info = w->FindClass("ItemInfo");
    std::string m = info ? info->GetString("uniformModel") : "";
    if (slot == UnitSlot::NVG && info)
        if (const std::string off = info->GetString("modelOff"); !off.empty())
        {
            p.note = "NVG up (ItemInfo modelOff; lowered: uniformModel)";
            m = off;
        }
    if (m.empty())
    {
        m = w->GetString("model");
        if (!m.empty()) p.note = "no ItemInfo uniformModel - the ground model= is shown";
    }
    if (m.empty() || m == "-") p.problem = std::format("CfgWeapons::{}: no worn model (ItemInfo uniformModel \"{}\")", cls, m);
    p.armaModel = m;
    return p;
}

void Unit::Replace(const ArmaConfig& cfg, UnitSlot slot, const std::string& cls)
{
    std::erase_if(parts_, [&](const UnitPart& p) { return p.slot == slot; });
    if (cls.empty() || IEquals(cls, "none")) return;
    UnitPart p = ReadPart(cfg, slot, cls, "replaced in the viewer");
    auto at = std::find_if(parts_.begin(), parts_.end(), [&](const UnitPart& q) { return q.slot > slot; });
    parts_.insert(at, std::move(p));
}

void Unit::StartOutfit(const fs::path& body)
{
    Clear();
    outfit_ = true;
    outfitBody_ = body;
    Log::Get().Info("Unit: outfit on {}", PathToUtf8(body.filename()));
}

UnitPart Unit::FilePart(UnitSlot slot, const fs::path& file) const
{
    UnitPart p;
    p.slot = slot;
    p.cls = PathToUtf8(file.filename());
    p.from = "file";
    p.file = file;
    p.fromFile = true;
    return p;
}

void Unit::AddFile(UnitSlot slot, const fs::path& file)
{
    if (slot != UnitSlot::Other) std::erase_if(parts_, [&](const UnitPart& p) { return p.slot == slot; });
    auto at = std::find_if(parts_.begin(), parts_.end(), [&](const UnitPart& q) { return q.slot > slot; });
    parts_.insert(at, FilePart(slot, file));
}

void Unit::RemovePart(size_t index)
{
    if (index < parts_.size()) parts_.erase(parts_.begin() + std::ptrdiff_t(index));
}

std::string UnitPart::Checks() const
{
    std::string s;
    auto add = [&](const std::string& t) { s += (s.empty() ? "" : "; ") + t; };
    if (!unpairedBones.empty())
    {
        std::string b;
        for (size_t i = 0; i < unpairedBones.size() && i < 6; ++i) b += (b.empty() ? "" : ", ") + unpairedBones[i];
        if (unpairedBones.size() > 6) b += ", ...";
        add(std::format("{} bone(s) without a pair in the body: {}", unpairedBones.size(), b));
    }
    if (overBones) add(std::format("{} point(s) on more than 4 bones", overBones));
    if (badWeightSum) add(std::format("{} point(s) with weights not summing to 1", badWeightSum));
    return s;
}

Unit::Preset Unit::MakePreset() const
{
    Preset p;
    if (outfit_) p.bodyFile = outfitBody_;
    else p.bodyClass = cls_;
    for (const auto& q : parts_)
    {
        if (q.cls.empty() && !q.fromFile) continue; // a slot without a part (no head found)
        p.parts.push_back({ q.slot, q.fromFile, q.fromFile ? PathToUtf8(q.file) : q.cls, q.enabled });
    }
    return p;
}

std::string Unit::SavePreset(const Preset& p)
{
    std::string s = "# ArmaView outfit preset\n";
    s += p.bodyClass.empty() ? "body=file:" + PathToUtf8(p.bodyFile) + "\n" : "body=class:" + p.bodyClass + "\n";
    for (const auto& q : p.parts) s += std::format("part={}|{}|{}|{}\n", UnitSlotName(q.slot), q.file ? "file" : "class", q.what, q.enabled ? 1 : 0);
    return s;
}

bool Unit::ParsePreset(const std::string& text, Preset& out, std::string& error)
{
    out = {};
    size_t a = 0;
    int ln = 0;
    while (a < text.size())
    {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string line = text.substr(a, b - a);
        a = b + 1;
        ++ln;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.starts_with("body=class:")) out.bodyClass = line.substr(11);
        else if (line.starts_with("body=file:")) out.bodyFile = fs::path(Utf8ToWide(line.substr(10)));
        else if (line.starts_with("part="))
        {
            std::vector<std::string> f;
            size_t c = 5;
            for (;;)
            {
                const size_t d = line.find('|', c);
                f.push_back(line.substr(c, d == std::string::npos ? std::string::npos : d - c));
                if (d == std::string::npos) break;
                c = d + 1;
            }
            Preset::Part q;
            if (f.size() < 3 || !ParseUnitSlot(f[0], q.slot) || (f[1] != "file" && f[1] != "class") || f[2].empty())
            {
                error = std::format("line {}: part=<slot>|class|file|<what>|<0/1> expected", ln);
                return false;
            }
            q.file = f[1] == "file";
            q.what = f[2];
            q.enabled = f.size() < 4 || f[3] != "0";
            out.parts.push_back(q);
        }
        else
        {
            error = std::format("line {}: unknown entry", ln);
            return false;
        }
    }
    if (out.bodyClass.empty() && out.bodyFile.empty()) { error = "no body= line"; return false; }
    return true;
}

void Unit::ApplyPresetParts(const ArmaConfig* cfg, const Preset& p)
{
    parts_.clear();
    for (const auto& q : p.parts)
    {
        UnitPart part;
        if (q.file) part = FilePart(q.slot, fs::path(Utf8ToWide(q.what)));
        else if (cfg) part = ReadPart(*cfg, q.slot, q.what, "preset");
        else { part.slot = q.slot; part.cls = q.what; part.problem = "no config loaded"; }
        part.enabled = q.enabled;
        parts_.push_back(std::move(part));
    }
}

void Unit::Load(const ArmaConfig& cfg, const Skeleton& skel, const fs::path& projectRoot)
{
    auto& log = Log::Get();
    for (auto& p : parts_)
    {
        if (p.model && p.problem.empty()) continue; // kept from the previous read
        LoadPart(p, cfg, skel, projectRoot);
        if (!p.problem.empty())
            log.Warn("Unit: {} {} ({}): not shown - {}", UnitSlotName(p.slot), p.cls.empty() ? "-" : p.cls, p.from, p.problem);
        else
            log.Info("Unit: {} {} ({}): {}{}{}", UnitSlotName(p.slot), p.cls, p.from, p.armaModel, p.note.empty() ? "" : " - ", p.note);
        if (p.problem.empty() && p.unboundPoints > 0)
        {
            std::string sels;
            for (const auto& s : p.unboundSelections) sels += (sels.empty() ? "" : ", ") + s;
            log.Warn("Unit: {} {}: {} point(s) on no bone of the unit's skeleton stay in the bind pose (selections: {})",
                     UnitSlotName(p.slot), p.cls, p.unboundPoints, sels.empty() ? "none" : sels);
        }
        if (p.problem.empty())
            if (const std::string c = p.Checks(); !c.empty()) log.Warn("Unit: {} {}: {}", UnitSlotName(p.slot), p.cls, c);
    }
}

void Unit::LoadPart(UnitPart& p, const ArmaConfig& cfg, const Skeleton& skel, const fs::path& projectRoot)
{
    p.model.reset();
    p.unboundPoints = 0;
    p.unboundSelections.clear();
    p.unpairedBones.clear();
    p.overBones = p.badWeightSum = 0;
    if (!p.problem.empty()) return;
    if (!p.fromFile) p.file = ResolveArmaPath(WithP3d(p.armaModel), projectRoot);
    auto src = cache_.Get(p.file, true, projectRoot); // its own skeleton (model.cfg), if any: bones without a pair
    if (!src || src->status != AttachedModel::Status::Ok)
    {
        p.problem = !src ? "proxy cycle" : std::format("{} ({})", PathToUtf8(p.file), src->message.empty() ? "not loaded" : src->message);
        return;
    }
    auto m = std::make_shared<AttachedModel>(*src);
    const P3DLod& lod = m->model.lods.front();
    // textures: hidden selections of the class that wears it (the face: its texture on the head's main texture)
    std::vector<std::string> sels, texs, mats;
    if (p.slot == UnitSlot::Head)
    {
        const CfgClass* face = FindFace(cfg, faceType_, p.cls, nullptr);
        std::map<std::string, int> count;
        for (const auto& f : lod.faces) ++count[ToLower(f.texture)];
        std::string main;
        int best = -1;
        for (const auto& [t, n] : count)
            if (n > best) { best = n; main = t; }
        const std::string tex = face ? face->GetString("texture") : "", mat = face ? face->GetString("material") : "";
        int changed = 0;
        RebuildModelGeometry(*m, [&](size_t, std::string& t, std::string& mt) {
            if (ToLower(t) != main) return true;
            if (!tex.empty()) t = tex;
            if (!mat.empty()) mt = mat;
            ++changed;
            return true;
        });
        if (!tex.empty() || !mat.empty())
            p.note += std::format("{}face texture / material on {} face(s)", p.note.empty() ? "" : "; ", changed);
    }
    else
    {
        const std::string root = p.slot == UnitSlot::Backpack ? "CfgVehicles" : p.slot == UnitSlot::Goggles ? "CfgGlasses" : "CfgWeapons";
        if (const CfgClass* c = FindConfigClass(cfg, root, p.cls))
        {
            const CfgClass* info = c->FindClass("ItemInfo");
            sels = info && info->Find("hiddenSelections") ? info->GetStrings("hiddenSelections") : c->GetStrings("hiddenSelections");
            texs = c->GetStrings("hiddenSelectionsTextures");
            mats = c->GetStrings("hiddenSelectionsMaterials");
        }
        if (!sels.empty() && (!texs.empty() || !mats.empty()))
        {
            int found = 0;
            const int hidden = RebuildWithHiddenSelections(*m, sels, texs, mats, &found);
            p.note += std::format("{}{} of {} hidden selection(s) in the model", p.note.empty() ? "" : "; ", found, sels.size());
            if (hidden) p.note += std::format("; {} face(s) hidden by config (\"\")", hidden);
        }
    }
    // skinning with the unit's skeleton
    if (skel.Empty())
        p.note += std::string(p.note.empty() ? "" : "; ") + "the open model has no skeleton: bind pose";
    else
    {
        p.skin = BuildSkin(skel, lod, true, 4);
        std::vector<uint8_t> unbound(lod.points.size(), 0);
        for (uint32_t v : m->vertPoint)
            if (v + 1 < p.skin.offset.size() && p.skin.offset[v] == p.skin.offset[v + 1] && !unbound[v]) { unbound[v] = 1; ++p.unboundPoints; }
        if (p.unboundPoints)
        {
            // the selections holding them, most first (a bone the skeleton lacks holds all of its points)
            std::vector<std::pair<int, std::string>> held;
            for (const auto& s : lod.selections)
            {
                if (s.name.starts_with("proxy:")) continue;
                int n = 0;
                for (size_t i = 0; i < s.pointBytes.size() && i < unbound.size(); ++i) n += unbound[i] && s.pointBytes[i] != 0;
                if (n) held.push_back({ n, s.name });
            }
            std::stable_sort(held.begin(), held.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (size_t i = 0; i < held.size() && i < 8; ++i) p.unboundSelections.push_back(std::format("{} ({})", held[i].second, held[i].first));
        }
        // raw weights per point on the body's bones (before the 4-bone limit and normalising)
        const std::vector<int> map = MapBonesToSelections(skel, lod);
        std::vector<int> count(lod.points.size(), 0);
        std::vector<float> sum(lod.points.size(), 0.0f);
        for (int b = 0; b < int(map.size()); ++b)
        {
            if (map[b] < 0) continue;
            const P3DSelection& sel = lod.selections[size_t(map[b])];
            for (size_t q = 0; q < lod.points.size(); ++q)
                if (const float w = sel.Weight(q); w > 0.0f) { ++count[q]; sum[q] += w; }
        }
        for (size_t q = 0; q < lod.points.size(); ++q)
        {
            p.overBones += count[q] > 4;
            p.badWeightSum += count[q] > 0 && std::abs(sum[q] - 1.0f) > 0.01f;
        }
        // bones of the part's own skeleton that hold points and that the body does not have
        if (src->rigged)
        {
            const std::vector<int> own = MapBonesToSelections(src->skeleton, lod);
            for (int b = 0; b < int(own.size()); ++b)
            {
                if (own[b] < 0 || skel.Find(src->skeleton.bones[size_t(b)].name) >= 0) continue;
                const P3DSelection& sel = lod.selections[size_t(own[b])];
                bool used = false;
                for (size_t q = 0; q < lod.points.size() && !used; ++q) used = sel.Weight(q) > 0.0f;
                if (used) p.unpairedBones.push_back(src->skeleton.bones[size_t(b)].name);
            }
        }
        else
            for (const auto& s : p.unboundSelections) p.unpairedBones.push_back(s.substr(0, s.find(' ')) + "?"); // a guess: no model.cfg
    }
    p.model = std::move(m);
}

void Unit::Publish(Attachments& attach) const
{
    attach.unitParts.clear();
    for (const auto& p : parts_)
        if (p.Shown()) attach.unitParts.push_back({ p.model, p.posed.empty() ? p.model->verts : p.posed });
}

void Unit::Pose(const std::vector<glm::mat4>& bones, Attachments& attach)
{
    size_t k = 0;
    std::vector<glm::mat4> pm;
    for (auto& p : parts_)
    {
        if (!p.Shown()) continue;
        if (bones.empty() || p.skin.offset.empty()) p.posed = p.model->verts;
        else
        {
            SkinMatrices(p.skin, bones, pm);
            SkinModelVerts(*p.model, pm, p.posed);
        }
        if (k < attach.unitParts.size()) attach.unitParts[k].posed = p.posed;
        ++k;
    }
}

std::vector<fs::path> Unit::Files() const
{
    std::vector<fs::path> out;
    for (const auto& p : parts_)
        if (!p.file.empty()) out.push_back(p.file);
    return out;
}

std::string Unit::Report() const
{
    std::string r = outfit_ ? std::format("outfit on {} ({} part(s))", PathToUtf8(outfitBody_.filename()), parts_.size())
                            : std::format("unit {} ({} part(s))", cls_.empty() ? "(none)" : cls_, parts_.size());
    for (const auto& p : parts_)
    {
        r += std::format("\n  {} {}: {}{}", UnitSlotName(p.slot), p.cls.empty() ? "-" : p.cls,
                         !p.problem.empty() ? "NOT SHOWN - " + p.problem : !p.enabled ? "hidden" : "shown",
                         p.problem.empty() && p.unboundPoints ? std::format(" ({} point(s) on no bone)", p.unboundPoints) : "");
        if (p.problem.empty())
            if (const std::string c = p.Checks(); !c.empty()) r += " - check: " + c;
    }
    return r;
}

std::vector<std::string> Unit::SoldierClasses(const ArmaConfig& cfg)
{
    std::vector<std::string> out;
    ForEachClass(cfg, "CfgVehicles", [&](const CfgClass& c) {
        if (!c.GetString("uniformClass").empty() && !c.GetString("model").empty()) out.push_back(c.name);
    });
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return ToLower(a) < ToLower(b); });
    return out;
}

std::vector<std::string> Unit::Candidates(const ArmaConfig& cfg, UnitSlot slot)
{
    std::vector<std::string> out;
    if (slot == UnitSlot::Head)
        ForEachClass(cfg, "CfgFaces", [&](const CfgClass& t) {
            for (const auto& f : t.classes)
                if (!f->external && !f->GetString("head").empty()) out.push_back(f->name);
        });
    else if (slot == UnitSlot::Backpack)
        ForEachClass(cfg, "CfgVehicles", [&](const CfgClass& c) {
            if (c.GetNumber("isbackpack", 0) != 0 && c.GetNumber("scope", 0) >= 1) out.push_back(c.name);
        });
    else if (slot == UnitSlot::Goggles)
        ForEachClass(cfg, "CfgGlasses", [&](const CfgClass& c) {
            if (!c.GetString("model").empty() && c.GetNumber("scope", 0) >= 1) out.push_back(c.name);
        });
    else
    {
        const int want = slot == UnitSlot::Headgear ? 605 : slot == UnitSlot::Vest ? 701 : 616;
        ForEachClass(cfg, "CfgWeapons", [&](const CfgClass& c) {
            if (c.GetNumber("scope", 0) >= 1 && ItemType(c) == want) out.push_back(c.name);
        });
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return ToLower(a) < ToLower(b); });
    out.erase(std::unique(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return IEquals(a, b); }), out.end());
    return out;
}
