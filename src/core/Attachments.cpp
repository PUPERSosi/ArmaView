#include "Attachments.h"
#include "Perf.h"
#include <thread>
#include <atomic>
#include "Log.h"
#include "RvMaterial.h" // FindVehicleClass

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstring>
#include <fstream>
#include <set>

#include "CoreGuard.h"

namespace fs = std::filesystem;

namespace {

std::wstring Key(const fs::path& p)
{
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    std::wstring s = (ec ? p : a).native();
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
    return s;
}

std::string Signature(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    char sig[4] = {};
    f.read(sig, 4);
    return std::string(sig, size_t(f.gcount()));
}

fs::path FindModelCfgFor(const fs::path& p3d, const fs::path& projectRoot)
{
    std::error_code ec;
    for (fs::path dir = p3d.parent_path(); !dir.empty(); dir = dir.parent_path())
    {
        if (fs::exists(dir / "model.cfg", ec)) return dir / "model.cfg";
        if (dir == dir.root_path() || (!projectRoot.empty() && fs::equivalent(dir, projectRoot, ec))) break;
    }
    return {};
}

// First LOD as sections grouped by (texture, material); proxy triangles skipped.
void BuildGeometry(AttachedModel& m, const FaceOverride& faceOverride = {})
{
    const P3DLod& l = m.model.lods.front();
    std::vector<uint8_t> proxyFace(l.faces.size(), 0);
    for (const auto& s : l.selections)
        if (s.name.starts_with("proxy:"))
            for (size_t i = 0; i < s.faceBytes.size() && i < proxyFace.size(); ++i)
                if (s.faceBytes[i]) proxyFace[i] = 1;
    bool normalsOk = !l.normals.empty();
    std::vector<glm::vec3> corner;
    if (!normalsOk) corner = ComputeCornerNormals(l, true);
    std::map<std::pair<std::string, std::string>, int> index;
    std::vector<std::vector<MeshVertex>> sv;
    std::vector<std::vector<uint32_t>> sp;
    static const int tri[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    for (size_t fi = 0; fi < l.faces.size(); ++fi)
    {
        const P3DFace& f = l.faces[fi];
        if (proxyFace[fi] || f.count < 3) continue;
        bool valid = true;
        for (uint32_t c = 0; c < f.count; ++c)
            valid &= f.v[c].point < l.points.size() && (!normalsOk || f.v[c].normal < l.normals.size());
        if (!valid) continue;
        std::string tex = f.texture, mat = f.material;
        if (faceOverride && !faceOverride(fi, tex, mat)) continue; // hidden by the class wearing it ("" texture)
        auto [it, added] = index.emplace(std::make_pair(ToLower(tex), ToLower(mat)), int(m.sections.size()));
        if (added)
        {
            m.sections.push_back({ tex, mat, 0, 0 });
            sv.emplace_back();
            sp.emplace_back();
        }
        for (int t = 0; t < int(f.count) - 2; ++t)
            for (int k : tri[t])
            {
                const P3DFaceVertex& fv = f.v[k];
                MeshVertex v;
                v.pos = l.points[fv.point].pos;
                v.normal = normalsOk ? l.normals[fv.normal] : corner[fi * 4 + k];
                v.uv = { fv.u, fv.v };
                v.uv1 = v.uv;
                v.color = glm::vec4(0.0f);
                sv[it->second].push_back(v);
                sp[it->second].push_back(fv.point);
            }
    }
    for (size_t s = 0; s < m.sections.size(); ++s)
    {
        m.sections[s].first = uint32_t(m.verts.size());
        m.sections[s].count = uint32_t(sv[s].size());
        m.verts.insert(m.verts.end(), sv[s].begin(), sv[s].end());
        m.vertPoint.insert(m.vertPoint.end(), sp[s].begin(), sp[s].end());
    }
}

const CfgEntry* FindEntryWithClass(const CfgClass* c, std::string_view name) { return c ? c->Find(name) : nullptr; }

} // namespace

MovesStateInfo LookupMovesState(const ArmaConfig* cfg, const std::string& name)
{
    MovesStateInfo s;
    s.name = name;
    const CfgClass* moves = cfg ? cfg->Root().FindClass("CfgMovesMaleSdr") : nullptr;
    const CfgClass* states = moves ? moves->FindClass("States") : nullptr;
    const CfgClass* st = states ? states->FindClass(name) : nullptr;
    if (!st) return s;
    s.found = true;
    s.file = st->GetString("file");
    s.speed = float(st->GetNumber("speed", 1.0));
    s.interpolationSpeed = float(st->GetNumber("interpolationSpeed", 6.0));
    s.looped = st->GetNumber("looped", 1.0) != 0.0;
    s.cfgFile = cfg->FileOf(st->file);
    s.cfgLine = st->line;
    auto names = [](const CfgClass* c, const char* entry) {
        std::vector<std::string> out;
        if (const CfgEntry* e = c->Find(entry))
            for (const auto& item : e->value.items)
                if (item.type == CfgValue::Type::String) out.push_back(item.str);
        return out;
    };
    s.connectTo = names(st, "ConnectTo");
    s.interpolateTo = names(st, "InterpolateTo");
    s.actions = st->GetString("actions");
    if (const CfgClass* acts = moves->FindClass("Actions"))
        if (const CfgClass* a = acts->FindClass(s.actions))
        {
            for (const CfgClass* c : a->Chain())
                for (const auto& e : c->entries)
                    if (e.value.type == CfgValue::Type::String && !e.value.str.empty() && states->FindClass(e.value.str) &&
                        std::none_of(s.actionEntries.begin(), s.actionEntries.end(), [&](auto& x) { return IEquals(x.first, e.name); }))
                        s.actionEntries.emplace_back(e.name, e.value.str);
        }
    return s;
}

std::string StateOfAction(const ArmaConfig* cfg, const std::string& action, const std::string& actionsClass)
{
    if (!cfg || action.empty()) return {};
    const CfgClass* moves = cfg->Root().FindClass("CfgMovesMaleSdr");
    if (!moves) return {};
    const CfgClass* states = moves->FindClass("States");
    auto isState = [&](const std::string& n) { return !n.empty() && states && states->FindClass(n); };
    if (const CfgClass* ma = moves->FindClass("ManActions"))
        if (const CfgEntry* e = ma->Find(action); e && isState(e->value.str)) return e->value.str;
    if (const CfgClass* acts = moves->FindClass("Actions"))
        if (const CfgClass* a = actionsClass.empty() ? nullptr : acts->FindClass(actionsClass))
            if (const CfgEntry* e = a->Find(action); e && isState(e->value.str)) return e->value.str;
    if (isState(action)) return action;
    return {};
}

const char* SeatRoleName(SeatRole r)
{
    switch (r)
    {
    case SeatRole::Driver: return "driver";
    case SeatRole::Gunner: return "gunner";
    case SeatRole::Commander: return "commander";
    case SeatRole::Cargo: return "cargo";
    default: return "-";
    }
}

std::string ConfigRef::Where() const { return file.empty() ? std::string() : std::format("{}({})", PathToUtf8(file), line); }

// ============================================================================================ model cache

bool IsPlaceholderProxy(const std::string& lp, const std::string& stem)
{
    return lp.find("muzzle_flash") != std::string::npos || lp.find("volumelight") != std::string::npos || stem.starts_with("mf_") ||
           stem.starts_with("muzzle_flash") || lp.find("\\characters_f\\proxies\\") != std::string::npos || lp.find("\\weapon_slots\\") != std::string::npos;
}

void RebuildModelGeometry(AttachedModel& m, const FaceOverride& faceOverride)
{
    m.verts.clear();
    m.vertPoint.clear();
    m.sections.clear();
    if (!m.model.lods.empty()) BuildGeometry(m, faceOverride);
}

int RebuildWithHiddenSelections(AttachedModel& m, const std::vector<std::string>& sels, const std::vector<std::string>& texs,
                                const std::vector<std::string>& mats, int* found)
{
    if (found) *found = 0;
    if (m.model.lods.empty()) return 0;
    const P3DLod& lod = m.model.lods.front();
    std::vector<int> faceSel(lod.faces.size(), -1);
    for (int i = 0; i < int(sels.size()); ++i)
        for (const auto& s : lod.selections)
            if (IEquals(s.name, sels[size_t(i)]))
            {
                if (found) ++*found;
                for (size_t f = 0; f < s.faceBytes.size() && f < faceSel.size(); ++f)
                    if (s.faceBytes[f]) faceSel[f] = i;
            }
    int hidden = 0;
    RebuildModelGeometry(m, [&](size_t f, std::string& t, std::string& mt) {
        const int i = f < faceSel.size() ? faceSel[f] : -1;
        if (SkinFace(texs, mats, i, t, mt)) return true;
        ++hidden;
        return false;
    });
    return hidden;
}

std::shared_ptr<AttachedModel> ModelCache::Get(const fs::path& file, bool rig, const fs::path& projectRoot) { return Load(file, rig, projectRoot, 0); }

void ModelCache::Merge(ModelCache&& other)
{
    for (auto& [k, m] : other.cache_) cache_.try_emplace(k, std::move(m));
    other.cache_.clear();
}

std::vector<fs::path> ModelCache::ProxyFiles(const P3DLod& lod, const fs::path& projectRoot)
{
    std::vector<fs::path> out;
    for (const auto& s : lod.selections)
    {
        if (!s.name.starts_with("proxy:")) continue;
        std::string p = s.name.substr(6);
        if (size_t dot = p.find_last_of('.'); dot != std::string::npos && dot + 1 < p.size() &&
                                              std::all_of(p.begin() + dot + 1, p.end(), [](char c) { return isdigit((unsigned char)c); }))
            p.resize(dot);
        fs::path f = ResolveArmaPath(p + ".p3d", projectRoot);
        if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(std::move(f));
    }
    return out;
}

// Proxies of the model's first LOD whose models exist are drawn as part of it (recursively, up to kMaxNesting levels):
// a character with a weapon, a weapon with its optic. Placeholders the game fills (gear slots, effects) are skipped.
// The nested vertices follow the proxy's first point (so they move with its bone on a rigged model).
void ModelCache::ExpandNested(AttachedModel& m, const fs::path& projectRoot, int depth)
{
    if (depth >= kMaxNesting || m.model.lods.empty()) return;
    const P3DLod& l = m.model.lods.front();
    const fs::path root = projectRoot.empty() ? fs::path("P:\\") : projectRoot;
    for (const auto& s : l.selections)
    {
        if (!s.name.starts_with("proxy:")) continue;
        std::string p = s.name.substr(6);
        if (size_t dot = p.find_last_of('.'); dot != std::string::npos && dot + 1 < p.size() &&
                                              std::all_of(p.begin() + dot + 1, p.end(), [](char c) { return isdigit((unsigned char)c); }))
            p.resize(dot);
        const std::string lp = ToLower(p);
        const size_t slash = lp.find_last_of("\\/");
        const std::string stem = slash == std::string::npos ? lp : lp.substr(slash + 1);
        if (IsPlaceholderProxy(lp, stem) || stem.find("wreck") != std::string::npos || lp.find("\\proxies\\") != std::string::npos) continue; // crew / gear proxies: the game's
        uint32_t pts[3] = {};
        int n = 0;
        for (size_t i = 0; i < s.pointBytes.size() && n < 3; ++i)
            if (s.pointBytes[i]) pts[n++] = uint32_t(i);
        if (n < 3) continue;
        fs::path file = ResolveArmaPath(p + ".p3d", root);
        auto child = Load(file, false, projectRoot, depth + 1);
        if (!child || child->status != AttachedModel::Status::Ok || child->verts.empty()) continue;
        glm::vec3 tri[3] = { l.points[pts[0]].pos, l.points[pts[1]].pos, l.points[pts[2]].pos };
        bool valid = false;
        const glm::mat4 xf = ProxyMatrix(tri, &valid);
        if (!valid) continue;
        const glm::mat3 r(xf);
        const uint32_t base = uint32_t(m.verts.size());
        for (const auto& sec : child->sections) m.sections.push_back({ sec.texture, sec.material, base + sec.first, sec.count });
        for (const auto& v : child->verts)
        {
            MeshVertex d = v;
            d.pos = glm::vec3(xf * glm::vec4(v.pos, 1.0f));
            d.normal = glm::normalize(r * v.normal);
            m.verts.push_back(d);
            m.vertPoint.push_back(pts[0]);
        }
        m.nested.push_back(child->file);
        m.nested.insert(m.nested.end(), child->nested.begin(), child->nested.end());
        m.nestedCount += 1 + child->nestedCount;
        Log::Get().Debug("Nested proxy {} in {} (level {}, {} vertices)", p, PathToUtf8(m.file.filename()), depth + 1, child->verts.size());
    }
}

std::shared_ptr<AttachedModel> ModelCache::Load(const fs::path& file, bool rig, const fs::path& projectRoot, int depth)
{
    const std::wstring key = Key(file) + (rig ? L"|rig" : L"");
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;
    if (std::find(loading_.begin(), loading_.end(), Key(file)) != loading_.end()) return nullptr; // a proxy cycle
    auto m = std::make_shared<AttachedModel>();
    m->file = file;
    cache_[key] = m;
    std::error_code ec;
    if (!fs::exists(file, ec))
    {
        m->status = AttachedModel::Status::Missing;
        m->message = "file not on disk";
        return m;
    }
    if (Signature(file) != "MLOD") // a binarized game model: shown as a frame with its name
    {
        m->status = AttachedModel::Status::Binarized;
        m->message = kBinarizedNotSupported;
        return m;
    }
    LoadP3D(file, m->model);
    if (m->model.lods.empty())
    {
        m->status = AttachedModel::Status::Error;
        m->message = "cannot read the P3D (see log)";
        return m;
    }
    m->status = AttachedModel::Status::Ok;
    BuildGeometry(*m);
    loading_.push_back(Key(file));
    ExpandNested(*m, projectRoot, depth);
    loading_.pop_back();
    if (rig)
    {
        m->modelCfg = FindModelCfgFor(file, projectRoot);
        if (!m->modelCfg.empty())
        {
            ArmaConfig cfg;
            cfg.Load(m->modelCfg, projectRoot);
            ModelCfgInfo info;
            if (BuildSkeleton(cfg, PathToUtf8(file.stem()), m->skeleton, info))
            {
                m->skin = BuildSkin(m->skeleton, m->model.lods.front(), true, 4);
                ResolvePivots(m->skeleton, nullptr, &m->model.lods.front()); // joint positions for IK (selection centroids)
                m->rigged = true;
            }
        }
        if (!m->rigged) m->message = "no skeleton (model.cfg) - shown in bind pose";
    }
    return m;
}

void ModelCache::Invalidate(const fs::path& file)
{
    const std::wstring k = Key(file);
    for (auto it = cache_.begin(); it != cache_.end();)
    {
        const auto& m = *it->second;
        bool hit = it->first == k || it->first == k + L"|rig" || (!m.modelCfg.empty() && Key(m.modelCfg) == k) ||
                   std::any_of(m.nested.begin(), m.nested.end(), [&](const fs::path& f) { return Key(f) == k; }); // a nested proxy changed
        it = hit ? cache_.erase(it) : std::next(it);
    }
}

std::vector<fs::path> ModelCache::Files() const
{
    std::vector<fs::path> out;
    for (const auto& [k, m] : cache_)
    {
        if (m->status == AttachedModel::Status::Ok || m->status == AttachedModel::Status::Error) out.push_back(m->file);
        if (!m->modelCfg.empty()) out.push_back(m->modelCfg);
    }
    return out;
}

// ============================================================================================ proxies

glm::mat4 ProxyMatrix(const glm::vec3 p[3], bool* validOut)
{
    // origin: the corner between the two shortest edges (opposite the longest one);
    // direction: the shortest edge from it, up: the other edge
    float d01 = glm::length(p[1] - p[0]), d02 = glm::length(p[2] - p[0]), d12 = glm::length(p[2] - p[1]);
    int o = d12 >= d01 && d12 >= d02 ? 0 : d02 >= d01 ? 1 : 2; // vertex opposite the longest edge
    int a = (o + 1) % 3, b = (o + 2) % 3;
    if (glm::length(p[a] - p[o]) > glm::length(p[b] - p[o])) std::swap(a, b);
    glm::vec3 dir = p[a] - p[o], up = p[b] - p[o];
    float ld = glm::length(dir);
    const bool valid = ld > 1e-6f && glm::length(glm::cross(dir, up)) > 1e-10f;
    if (validOut) *validOut = valid;
    glm::mat4 m;
    if (!valid)
    {
        m = glm::mat4(0.0f); // collapsed (e.g. hidden by an animation)
        m[3] = glm::vec4(p[o], 1.0f);
        return m;
    }
    dir /= ld;
    up = glm::normalize(up - dir * glm::dot(up, dir));
    glm::vec3 aside = glm::cross(up, dir);
    // proxy model space -> parent model space: x = aside, y = up, z = dir
    return glm::mat4(glm::vec4(aside, 0), glm::vec4(up, 0), glm::vec4(dir, 0), glm::vec4(p[o], 1));
}

void SkinModelVerts(const AttachedModel& m, const std::vector<glm::mat4>& points, std::vector<MeshVertex>& out)
{
    out = m.verts;
    for (size_t i = 0; i < out.size() && i < m.vertPoint.size(); ++i)
    {
        const uint32_t p = m.vertPoint[i];
        if (p >= points.size()) continue;
        out[i].pos = glm::vec3(points[p] * glm::vec4(m.verts[i].pos, 1.0f));
        out[i].normal = glm::normalize(glm::mat3(points[p]) * m.verts[i].normal);
    }
}

void Attachments::BuildSlots(const P3DLod& lod, const ArmaConfig* cfg, const fs::path& projectRoot)
{
    auto& log = Log::Get();
    slots_.clear();
    const CfgClass* nonAi = cfg ? cfg->Root().FindClass("CfgNonAIVehicles") : nullptr;
    for (const auto& s : lod.selections)
    {
        if (!s.name.starts_with("proxy:")) continue;
        ProxySlot slot;
        slot.selection = s.name;
        std::string p = s.name.substr(6);
        if (size_t dot = p.find_last_of('.'); dot != std::string::npos && dot + 1 < p.size() &&
                                              std::all_of(p.begin() + dot + 1, p.end(), [](char c) { return isdigit((unsigned char)c); }))
        {
            slot.index = atoi(p.c_str() + dot + 1);
            p.resize(dot);
        }
        slot.armaPath = p;
        size_t slash = p.find_last_of("\\/");
        slot.stem = ToLower(slash == std::string::npos ? p : p.substr(slash + 1));
        // the wreck model of DestructWreck vehicles (e.g. \...\pf_hydra_wreck.001, Test_Heli_01_wreck.001)
        slot.destroyed = slot.stem.find("wreck") != std::string::npos;
        slot.effect = IsPlaceholderProxy(ToLower(slot.armaPath), slot.stem); // effects and gear-slot placeholders (the game fills them)
        int n = 0;
        for (size_t i = 0; i < s.pointBytes.size() && n < 3; ++i)
            if (s.pointBytes[i]) slot.points[n++] = uint32_t(i);
        if (n < 3)
        {
            log.Warn("Proxy {}: {} point(s) instead of 3 - skipped", s.name, n);
            continue;
        }
        // role: CfgNonAIVehicles >> Proxy<file name> and its bases (ProxyDriver / ProxyGunner / ...), else the name
        if (const CfgClass* k = nonAi ? nonAi->FindClass("Proxy" + slot.stem) : nullptr)
            for (std::pair<const CfgClass*, int> it{ k, 0 }; it.first && slot.role == SeatRole::None && it.second < 64; it = { it.first->base, it.second + 1 })
            {
                std::string b = ToLower(it.first->name);
                if (b == "proxydriver" || b == "proxypilot") slot.role = SeatRole::Driver;
                else if (b == "proxycommander") slot.role = SeatRole::Commander;
                else if (b == "proxygunner") slot.role = SeatRole::Gunner;
                else if (b == "proxycargo") slot.role = SeatRole::Cargo;
            }
        if (slot.role == SeatRole::None && slot.armaPath.find("\\proxies\\") != std::string::npos)
        {
            if (slot.stem.starts_with("driver") || slot.stem.starts_with("pilot")) slot.role = SeatRole::Driver;
            else if (slot.stem.starts_with("commander")) slot.role = SeatRole::Commander;
            else if (slot.stem.starts_with("gunner")) slot.role = SeatRole::Gunner;
            else if (slot.stem.starts_with("cargo")) slot.role = SeatRole::Cargo;
        }
        {
            // crew proxies too: the game's mannequin (shown when the crew preview is off)
            fs::path file = ResolveArmaPath(slot.armaPath + ".p3d", projectRoot);
            slot.model = cache_.Get(file, false, projectRoot);
            if (slot.role == SeatRole::None && slot.model->status != AttachedModel::Status::Ok &&
                std::find(loggedStubs_.begin(), loggedStubs_.end(), ToLower(slot.armaPath)) == loggedStubs_.end())
            {
                loggedStubs_.push_back(ToLower(slot.armaPath));
                if (slot.model->status == AttachedModel::Status::Binarized)
                    log.Info("Proxy {}: binarized model - shown as a frame", slot.armaPath);
                else if (slot.model->status == AttachedModel::Status::Missing)
                    log.Warn("Proxy {}: file not on P: ({}) - shown as a frame", slot.armaPath, PathToUtf8(file));
                else
                    log.Warn("Proxy {}: {} - shown as a frame", slot.armaPath, slot.model->message);
            }
        }
        slots_.push_back(std::move(slot));
    }
    std::stable_sort(slots_.begin(), slots_.end(), [](const ProxySlot& a, const ProxySlot& b) {
        return a.role != b.role ? a.role < b.role : a.index < b.index;
    });
    int models = 0, crew = 0;
    for (const auto& s : slots_) (s.role != SeatRole::None ? crew : models)++;
    log.Info("Proxies: {} ({} crew place(s), {} other), {} model file(s) cached", slots_.size(), crew, models, cache_.Files().size());
    for (auto& seat : seats_) seat.slot = -1;
}

void Attachments::UpdateTransforms(const P3DLod& lod, const std::vector<glm::mat4>& pointMats)
{
    const bool posed = pointMats.size() == lod.points.size();
    for (auto& s : slots_)
    {
        for (int pass = posed ? 0 : 1; pass < 2; ++pass) // 0: rest pose (IK reference), 1: current pose
        {
        glm::vec3 p[3];
        for (int i = 0; i < 3; ++i)
        {
            uint32_t pi = s.points[i];
            glm::vec3 x = pi < lod.points.size() ? lod.points[pi].pos : glm::vec3(0.0f);
            p[i] = posed && pass == 1 ? glm::vec3(pointMats[pi] * glm::vec4(x, 1.0f)) : x;
        }
        bool valid = false;
        const glm::mat4 m = ProxyMatrix(p, &valid);
        if (pass == 0 || !posed) s.xfRest = m;
        if (pass == 1) { s.xf = m; s.valid = valid; }
        }
    }
}

// ============================================================================================ crew

void Attachments::BuildCrew(const ArmaConfig* cfg, const std::string& modelPath, const fs::path& projectRoot)
{
    auto& log = Log::Get();
    std::map<std::string, std::string> overrides; // manual RTMs survive a rebuild (hot reload, LOD switch)
    for (const auto& s : seats_)
        if (!s.overrideRtm.empty()) overrides[s.title] = s.overrideRtm;
    seats_.clear();
    vehicleClass_.clear();
    const CfgClass* veh = nullptr;
    {
        PerfNoteScope ps("load:       crew: CfgVehicles class of the model");
        veh = cfg ? FindVehicleClass(*cfg, modelPath) : nullptr;
    }
    if (veh && IEquals(veh->GetString("simulation"), "soldier")) // a character: its class has no crew seats
    {
        log.Info("Crew: CfgVehicles::{} is a soldier (simulation=soldier) - no seats", veh->name);
        veh = nullptr;
    }
    auto ref =[&](const CfgClass* c, std::string_view entry) {
        ConfigRef r;
        if (const CfgEntry* e = FindEntryWithClass(c, entry)) { r.file = cfg->FileOf(e->file); r.line = e->line; }
        return r;
    };
    std::set<int> cargoTaken;
    if (veh)
    {
        vehicleClass_ = veh->name;
        if (veh->GetNumber("hasDriver", 1) != 0)
        {
            CrewSeat s;
            s.title = "driver";
            s.role = SeatRole::Driver;
            s.actionOut = veh->GetString("driverAction");
            s.actionIn = veh->GetString("driverInAction");
            s.actionRef = ref(veh, "driverAction");
            const char* ikd[4] = { "driverLeftHandAnimName", "driverRightHandAnimName", "driverLeftLegAnimName", "driverRightLegAnimName" };
            for (int k = 0; k < 4; ++k) s.ikBone[k] = veh->GetString(ikd[k]);
            s.getInPoint = veh->GetString("memoryPointsGetInDriver");
            s.getInDir = veh->GetString("memoryPointsGetInDriverDir");
            for (const char* e : { "getInAction", "getOutAction" })
                if (std::string v = veh->GetString(e); !v.empty()) s.related.emplace_back(e, v);
            seats_.push_back(std::move(s));
        }
        std::function<void(const CfgClass&, const std::string&)> turrets = [&](const CfgClass& owner, const std::string& path) {
            const CfgClass* tu = owner.FindClass("Turrets");
            if (!tu) return;
            for (const CfgClass* t : MergedChildClasses(*tu))
            {
                std::string name = path.empty() ? t->name : path + "/" + t->name;
                if (t->GetNumber("hasGunner", 1) != 0)
                {
                    CrewSeat s;
                    s.title = "turret " + name;
                    std::string type = ToLower(t->GetString("proxyType", "CPGunner"));
                    s.role = type == "cpcommander" ? SeatRole::Commander : type == "cpdriver" ? SeatRole::Driver
                           : type == "cpcargo" ? SeatRole::Cargo : SeatRole::Gunner;
                    s.proxyIndex = int(t->GetNumber("proxyIndex", 1));
                    s.actionOut = t->GetString("gunnerAction");
                    s.actionIn = t->GetString("gunnerInAction");
                    s.actionRef = ref(t, "gunnerAction");
                    if (s.role == SeatRole::Cargo) cargoTaken.insert(s.proxyIndex);
                    const char* ikg[4] = { "gunnerLeftHandAnimName", "gunnerRightHandAnimName", "gunnerLeftLegAnimName", "gunnerRightLegAnimName" };
                    for (int k = 0; k < 4; ++k) s.ikBone[k] = t->GetString(ikg[k]);
                    s.getInPoint = t->GetString("memoryPointsGetInGunner");
                    s.getInDir = t->GetString("memoryPointsGetInGunnerDir");
                    for (const char* e : { "gunnerGetInAction", "gunnerGetOutAction" })
                        if (std::string v = t->GetString(e); !v.empty()) s.related.emplace_back(e, v);
                    seats_.push_back(std::move(s));
                }
                turrets(*t, name);
            }
        };
        turrets(*veh, "");
        // cargo: free cargo proxies in index order, cargoAction[index - 1] (shorter array: last element)
        int count = int(veh->GetNumber("transportSoldier", 0));
        std::vector<std::string> actions = veh->GetStrings("cargoAction");
        std::vector<int> free;
        std::vector<std::string> cpi = veh->GetStrings("cargoProxyIndexes"); // explicit cargo proxy order (e.g. helicopters)
        if (!cpi.empty())
            for (const auto& v : cpi) { int ix = atoi(v.c_str()); if (ix > 0 && !cargoTaken.count(ix)) free.push_back(ix); }
        else
            for (const auto& sl : slots_)
                if (sl.role == SeatRole::Cargo && !cargoTaken.count(sl.index)) free.push_back(sl.index);
        if (cpi.empty()) std::sort(free.begin(), free.end());
        free.erase(std::unique(free.begin(), free.end()), free.end());
        for (int i = 0; i < count; ++i)
        {
            CrewSeat s;
            s.role = SeatRole::Cargo;
            s.proxyIndex = i < int(free.size()) ? free[i] : (free.empty() ? i + 1 : free.back() + (i - int(free.size()) + 1));
            s.title = std::format("cargo {}", s.proxyIndex);
            if (!actions.empty()) s.actionOut = actions[std::min<size_t>(size_t(s.proxyIndex - 1), actions.size() - 1)];
            s.actionRef = ref(veh, "cargoAction");
            {
                auto pts = veh->GetStrings("memoryPointsGetInCargo"), dirs = veh->GetStrings("memoryPointsGetInCargoDir");
                if (!pts.empty()) s.getInPoint = pts[std::min<size_t>(size_t(i), pts.size() - 1)];
                if (!dirs.empty()) s.getInDir = dirs[std::min<size_t>(size_t(i), dirs.size() - 1)];
            }
            for (const char* e : { "cargoGetInAction", "cargoGetOutAction" })
            {
                std::vector<std::string> v = veh->GetStrings(e);
                if (!v.empty()) s.related.emplace_back(e, v[std::min<size_t>(size_t(s.proxyIndex - 1), v.size() - 1)]);
            }
            seats_.push_back(std::move(s));
        }
    }
    // match seats to proxies; crew proxies nobody uses become seats without an action
    std::vector<bool> used(slots_.size(), false);
    for (auto& s : seats_)
        for (int i = 0; i < int(slots_.size()); ++i)
            if (!used[i] && slots_[i].role == s.role && slots_[i].index == s.proxyIndex)
            {
                s.slot = i;
                used[i] = true;
                break;
            }
    for (int i = 0; i < int(slots_.size()); ++i)
        if (slots_[i].role != SeatRole::None && !used[i])
        {
            CrewSeat s;
            s.title = std::format("{}.{:03} (no seat)", slots_[i].stem, slots_[i].index);
            s.role = slots_[i].role;
            s.proxyIndex = slots_[i].index;
            s.slot = i;
            seats_.push_back(std::move(s));
        }
    {
        PerfNoteScope ps("load:       crew: get-in points");
        for (auto& s : seats_) ResolveGetIn(s);
    }
    for (auto& sl : slots_) sl.seat = -1;
    for (int i = 0; i < int(seats_.size()); ++i)
    {
        if (seats_[i].slot >= 0) slots_[seats_[i].slot].seat = i;
        if (auto it = overrides.find(seats_[i].title); it != overrides.end()) seats_[i].overrideRtm = it->second;
    }
    log.Info("Crew: CfgVehicles::{} - {} seat(s)", vehicleClass_.empty() ? "?" : vehicleClass_, seats_.size());
    PerfNoteScope ps("load:       crew: actions, states, RTMs, posing");
    ResolveCrew(cfg, projectRoot);
}

void Attachments::ResolveCrew(const ArmaConfig* cfg, const fs::path& projectRoot)
{
    auto& log = Log::Get();
    lastCfg_ = cfg;
    projectRoot_ = projectRoot;
    deferPose_ = true;
    posePending_.clear();
    // seats share actions and states (cargo): each config lookup once per rebuild
    std::map<std::string, MovesStateInfo> stateInfo;
    std::map<std::string, std::string> actionState;
    auto lookupState = [&](const std::string& st) -> const MovesStateInfo& {
        auto it = stateInfo.find(st);
        return it != stateInfo.end() ? it->second : stateInfo.emplace(st, LookupMovesState(cfg, st)).first->second;
    };
    auto stateOf = [&](const std::string& act, const std::string& actions = "RifleStandActions") -> std::string {
        const std::string key = act + "|" + actions;
        auto it = actionState.find(key);
        return it != actionState.end() ? it->second : actionState.emplace(key, StateOfAction(cfg, act, actions)).first->second;
    };
    const CfgClass* moves = cfg ? cfg->Root().FindClass("CfgMovesMaleSdr") : nullptr;
    const CfgClass* manActions = moves ? moves->FindClass("ManActions") : nullptr;
    const CfgClass* states = moves ? moves->FindClass("States") : nullptr;
    for (auto& s : seats_)
    {
        s.action = s.state = s.rtmArma = s.problem = {};
        s.problemIsWarning = false;
        s.manActionRef = s.stateRef = {};
        s.rtmFile.clear();
        s.binarized = false;
        s.rtm.reset();
        s.posed.clear();
        if (s.title.ends_with("(no seat)"))
        {
            s.problem = "crew proxy not used by any seat of the config (proxyIndex / proxyType)";
            continue;
        }
        if (s.slot < 0) s.problem = std::format("no proxy {}.{:03} in this LOD - the seat is not available in the game", SeatRoleName(s.role), s.proxyIndex);
        // turned in: *InAction - unless it is the inherited placeholder that names no state (e.g. ManActTestDriver)
        const bool useIn = !turnedOut && !s.actionIn.empty() && s.role != SeatRole::Cargo && !stateOf(s.actionIn).empty();
        s.action = useIn ? s.actionIn : s.actionOut;
        s.time = 0.0;
        s.chain.clear();
        s.chainIndex = -1;
        s.poseStack = AnimStack{}; // it points at the previous animations
        s.poseA = s.poseB = nullptr;
        // keep only the vehicle-level get in / out actions; state links are re-read below
        std::erase_if(s.related, [](auto& x) { return x.first.find("GetIn") == std::string::npos && x.first.find("GetOut") == std::string::npos &&
                                                 x.first.find("getIn") == std::string::npos && x.first.find("getOut") == std::string::npos; });
        auto fail = [&](std::string msg) { s.problem = s.problem.empty() ? msg : s.problem + "; " + msg; };
        if (!s.overrideRtm.empty())
            s.rtmFile = s.overrideRtm;
        else if (s.action.empty())
        {
            fail(s.role == SeatRole::Cargo ? "no cargoAction[]" : std::format("no {}", s.role == SeatRole::Driver && s.title == "driver" ? "driverAction" : "gunnerAction"));
            s.problemIsWarning = s.slot >= 0; // not set anywhere: the game falls back to its default
            continue;
        }
        else
        {
            if (!moves) { fail("no CfgMovesMaleSdr (the game configs are still loading, or not on P:)"); continue; }
            // action -> state: CfgMovesMaleSdr >> ManActions >> action; an action may also name a state directly
            if (const CfgEntry* e = manActions ? manActions->Find(s.action) : nullptr)
            {
                s.state = e->value.str;
                s.manActionRef = { cfg->FileOf(e->file), e->line };
            }
            else if (states && states->FindClass(s.action))
                s.state = s.action;
            if (s.state.empty()) { fail(std::format("action \"{}\" is not in CfgMovesMaleSdr >> ManActions", s.action)); continue; }
            const CfgClass* st = states ? states->FindClass(s.state) : nullptr;
            if (!st) { fail(std::format("state \"{}\" is not in CfgMovesMaleSdr >> States", s.state)); continue; }
            s.stateRef = { cfg->FileOf(st->file), st->line };
            const MovesStateInfo& info = lookupState(s.state);
            s.duration = DurationFromMovesSpeed(info.speed);
            s.interpolationSpeed = info.interpolationSpeed;
            s.looped = info.looped;
            // related states: the state's Actions (GetOut, TurnIn, TurnOut, ...), the other In/Out action, get in / out
            std::vector<std::pair<std::string, std::string>> rel;
            for (const auto& [k, act] : s.related) // vehicle level: getInAction / getOutAction (+ gunner / cargo variants)
            {
                // get in: from the standing man (RifleStandActions); get out: from the seat state's own actions
                bool out = ToLower(k).find("getout") != std::string::npos;
                std::string stn = stateOf(act, out ? info.actions : "RifleStandActions");
                log.Debug("  Crew {}: {}=\"{}\" -> state \"{}\" (actions {})", s.title, k, act, stn, out ? info.actions : "RifleStandActions");
                if (!stn.empty()) rel.emplace_back(k, stn);
            }
            for (const auto& [k, v] : info.actionEntries)
            {
                // the seat's own entries only (the Crew base class inherits dozens of others, e.g. ChopperHeavy_*_GetOut_H)
                const std::string lk = ToLower(k);
                bool seat = lk == "getin" || lk == "getout" || lk == "turnin" || lk == "turnout" || lk == "die" || lk == "eject";
                if (seat && std::find_if(rel.begin(), rel.end(), [&](auto& x) { return IEquals(x.second, v); }) == rel.end() && !IEquals(v, s.state))
                    rel.emplace_back(k, v);
            }
            if (const std::string& other = useIn ? s.actionOut : s.actionIn; !other.empty())
                if (std::string stn = stateOf(other); !stn.empty() && !IEquals(stn, s.state))
                    rel.emplace_back(useIn ? "turned out" : "turned in", stn);
            s.related = std::move(rel);
            s.rtmArma = st->GetString("file");
            if (s.rtmArma.empty()) { fail(std::format("state {} has no file", s.state)); continue; }
            s.rtmFile = ResolveArmaPath(s.rtmArma, projectRoot);
            if (!s.rtmFile.has_extension()) s.rtmFile += ".rtm";
        }
        PerfNoteScope pps("load:         crew: seat RTM + model load");
        PoseSeat(s, projectRoot);
    }
    deferPose_ = false;
    PerfNoteScope posing("load:         crew: posing (parallel)");
    {
        // the crew model and the RTMs are loaded (the caches are only read now): one seat per worker
        std::vector<CrewSeat*> todo = std::move(posePending_);
        posePending_.clear();
        std::atomic<size_t> next{ 0 };
        std::vector<std::jthread> pool;
        const unsigned n = std::min<unsigned>(std::max(1u, std::thread::hardware_concurrency()), unsigned(todo.size()));
        for (unsigned w = 0; w < n; ++w)
            pool.emplace_back([&] {
                for (size_t i; (i = next.fetch_add(1)) < todo.size();) PoseSeatAt(*todo[i], *todo[i]->rtm, 0.0f);
            });
    }
    // a config seat without a proxy in the model (e.g. an inherited MainTurret) does not exist in the game: warning
    // without the game's CfgMovesMaleSdr (no unpacked configs on P:) the seats cannot be resolved at all, and the
    // vehicle's inherited turrets are unknown: every crew problem is then a warning, not an error of the model
    for (auto& s : seats_)
        if ((s.slot < 0 && !s.problem.empty() && !s.title.ends_with("(no seat)")) || (!moves && !s.problem.empty())) s.problemIsWarning = true;
    int ok = 0, binarized = 0;
    for (const auto& s : seats_)
    {
        ok += !s.posed.empty();
        binarized += s.binarized;
        if (!s.problem.empty()) log.Warn("  Crew {}: {}{}", s.title, s.problem, s.problemIsWarning ? " (the game uses its default)" : "");
    }
    log.Info("Crew: {} posed, {} binarized pose(s) (frame), {} with problems", ok, binarized,
             std::count_if(seats_.begin(), seats_.end(), [](const CrewSeat& s) { return !s.problem.empty() && !s.problemIsWarning; }));
}

std::shared_ptr<AttachedModel> Attachments::CrewModel(const fs::path& projectRoot)
{
    return cache_.Get(fs::path(Utf8ToWide(crewModel)), true, projectRoot);
}

void Attachments::PoseSeat(CrewSeat& s, const fs::path& projectRoot)
{
    auto& log = Log::Get();
    auto fail = [&](std::string msg) { s.problem = s.problem.empty() ? msg : s.problem + "; " + msg; };
    std::error_code ec;
    if (!fs::exists(s.rtmFile, ec)) { fail(std::format("RTM not on disk: {}", PathToUtf8(s.rtmFile))); return; }
    std::string err;
    auto anim = LoadCachedRtm(s.rtmFile, err);
    if (!anim)
    {
        if (!Signature(s.rtmFile).starts_with("RTM_"))
        {
            s.binarized = true;
            log.Info("Crew {}: {} - binarized animation, the seat is shown as a frame", s.title, PathToUtf8(s.rtmFile.filename()));
        }
        else
            fail(std::format("RTM {}: {}", PathToUtf8(s.rtmFile.filename()), err));
        return;
    }
    s.rtm = anim;
    auto model = CrewModel(projectRoot);
    if (model->status != AttachedModel::Status::Ok) { fail(std::format("crew model: {}", model->message)); return; }
    s.posed = model->verts;
    if (deferPose_) posePending_.push_back(&s); // ResolveCrew poses the seats in parallel once all are loaded
    else PoseSeatAt(s, *anim, 0.0f);
}

void Attachments::PoseSeatAt(CrewSeat& s, const RtmAnimation& a, float phase, const RtmAnimation* b, float phaseB, float weightB)
{
    auto model = cache_.Get(fs::path(Utf8ToWide(crewModel)), true, projectRoot_);
    if (model->status != AttachedModel::Status::Ok) return;
    if (s.posed.size() != model->verts.size()) s.posed = model->verts;
    if (!model->rigged) return;
    AnimStack& stack = s.poseStack;
    if (s.poseA != &a || s.poseB != b || s.poseFollow != followParent || stack.layers.size() != (b ? 2u : 1u))
    {
        stack = AnimStack{};
        auto layer = [&](const RtmAnimation& anim) {
            AnimLayer l;
            l.anim = std::shared_ptr<RtmAnimation>(std::shared_ptr<RtmAnimation>{}, const_cast<RtmAnimation*>(&anim)); // non-owning
            l.loop = false;
            l.duration = 1.0f;
            stack.layers.push_back(l);
        };
        layer(a);
        if (b) layer(*b);
        stack.missing = followParent ? MissingBones::FollowParent : MissingBones::Engine;
        stack.Bind(model->skeleton);
        s.poseA = &a;
        s.poseB = b;
        s.poseFollow = followParent;
    }
    stack.layers[0].offset = phase; // time 0 -> phase = offset
    stack.layers[0].weight = b ? 1.0f - weightB : 1.0f;
    if (b)
    {
        stack.layers[1].offset = phaseB;
        stack.layers[1].weight = weightB;
    }
    std::vector<glm::mat4> bones;
    stack.Evaluate(model->skeleton, 0.0, bones);
    SkinSeat(s, *model, bones);
}

void Attachments::SkinSeat(CrewSeat& s, const AttachedModel& model, std::vector<glm::mat4>& bones)
{
    if (ik) ApplyIK(s, model.skeleton, bones);
    std::vector<glm::mat4> points;
    SkinMatrices(model.skin, bones, points);
    for (size_t i = 0; i < s.posed.size() && i < model.verts.size(); ++i)
    {
        const MeshVertex& src = model.verts[i];
        uint32_t p = model.vertPoint[i];
        if (p >= points.size()) continue;
        s.posed[i].pos = glm::vec3(points[p] * glm::vec4(src.pos, 1.0f));
        s.posed[i].normal = glm::normalize(glm::mat3(points[p]) * src.normal);
    }
}

namespace {

glm::quat RotationBetween(glm::vec3 u, glm::vec3 v)
{
    u = glm::normalize(u);
    v = glm::normalize(v);
    float d = glm::dot(u, v);
    if (d > 0.99999f) return glm::quat(1, 0, 0, 0);
    if (d < -0.99999f)
    {
        glm::vec3 axis = glm::cross(glm::vec3(1, 0, 0), u);
        if (glm::length(axis) < 1e-4f) axis = glm::cross(glm::vec3(0, 1, 0), u);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    glm::vec3 c = glm::cross(u, v);
    return glm::normalize(glm::quat(1.0f + d, c.x, c.y, c.z));
}

glm::mat4 RotateAbout(const glm::vec3& pivot, const glm::quat& q)
{
    return glm::translate(glm::mat4(1.0f), pivot) * glm::mat4_cast(q) * glm::translate(glm::mat4(1.0f), -pivot);
}

void Subtree(const Skeleton& sk, int b, std::vector<int>& out)
{
    out.push_back(b);
    for (int c : sk.bones[b].children) Subtree(sk, c, out);
}

} // namespace

glm::quat TwoBoneIK(const Skeleton& sk, std::vector<glm::mat4>& bones, int a, int b, int c, const glm::vec3& target)
{
    auto joint = [&](int j) { return glm::vec3(bones[size_t(j)] * glm::vec4(sk.bones[size_t(j)].pivot, 1.0f)); };
    const glm::vec3 A = joint(a), B = joint(b), C = joint(c);
    if (glm::length(target - C) < 1e-5f) return glm::quat(1, 0, 0, 0);
    const float l1 = glm::length(B - A), l2 = glm::length(C - B);
    if (l1 < 1e-4f || l2 < 1e-4f) return glm::quat(1, 0, 0, 0);
    const glm::vec3 toT = target - A;
    const float d = glm::clamp(glm::length(toT), std::abs(l1 - l2) + 1e-4f, l1 + l2 - 1e-4f);
    const glm::vec3 dir = glm::normalize(toT);
    glm::vec3 pole = (B - A) - dir * glm::dot(B - A, dir);
    if (glm::length(pole) < 1e-5f) pole = glm::cross(dir, glm::vec3(0, 1, 0));
    pole = glm::normalize(pole);
    const float cosA = glm::clamp((l1 * l1 + d * d - l2 * l2) / (2.0f * l1 * d), -1.0f, 1.0f);
    const glm::vec3 B2 = A + dir * (l1 * cosA) + pole * (l1 * std::sqrt(1.0f - cosA * cosA));
    const glm::vec3 T2 = A + dir * d;
    // upper bone: B -> B2 around A (whole subtree), then lower bone: C' -> T2 around B2
    const glm::quat q1 = RotationBetween(B - A, B2 - A);
    const glm::mat4 r1 = RotateAbout(A, q1);
    std::vector<int> upper, lower;
    Subtree(sk, a, upper);
    Subtree(sk, b, lower);
    for (int j : upper) bones[size_t(j)] = r1 * bones[size_t(j)];
    const glm::vec3 C1 = glm::vec3(r1 * glm::vec4(C, 1.0f));
    const glm::quat q2 = RotationBetween(C1 - B2, T2 - B2);
    const glm::mat4 r2 = RotateAbout(B2, q2);
    for (int j : lower) bones[size_t(j)] = r2 * bones[size_t(j)];
    return q2 * q1;
}

void RotateSubtree(const Skeleton& sk, std::vector<glm::mat4>& bones, int b, const glm::vec3& pivot, const glm::quat& q)
{
    std::vector<int> tree;
    Subtree(sk, b, tree);
    const glm::mat4 r = RotateAbout(pivot, q);
    for (int j : tree) bones[size_t(j)] = r * bones[size_t(j)];
}

// Two-bone IK: each limb end (hand / foot) follows its vehicle bone: target = where the end lands when the vehicle
// bone's deformation (rest -> now) is applied to it. The chain keeps its bend plane (pole = current elbow / knee).
void Attachments::ApplyIK(const CrewSeat& s, const Skeleton& crew, std::vector<glm::mat4>& bones) const
{
    if (!vehSkel_ || s.slot < 0 || s.slot >= int(slots_.size()) || vehBones_.size() != vehSkel_->bones.size()) return;
    const ProxySlot& sl = slots_[s.slot];
    if (!sl.valid) return;
    static const char* chains[4][3] = { { "LeftArm", "LeftForeArm", "LeftHand" }, { "RightArm", "RightForeArm", "RightHand" },
                                        { "LeftUpLeg", "LeftLeg", "LeftFoot" }, { "RightUpLeg", "RightLeg", "RightFoot" } };
    const glm::mat4 toCrew = glm::inverse(sl.xf);
    for (int k = 0; k < 4; ++k)
    {
        if (s.ikBone[k].empty()) continue;
        int vb = vehSkel_->Find(s.ikBone[k]);
        int a = crew.Find(chains[k][0]), b = crew.Find(chains[k][1]), c = crew.Find(chains[k][2]);
        if (vb < 0 || a < 0 || b < 0 || c < 0) continue;
        const glm::vec3 C = glm::vec3(bones[size_t(c)] * glm::vec4(crew.bones[size_t(c)].pivot, 1.0f));
        const glm::vec3 T = glm::vec3(toCrew * vehBones_[vb] * sl.xfRest * glm::vec4(C, 1.0f));
        if (glm::length(T - C) < 1e-5f) continue;
        const glm::quat qChainRot = TwoBoneIK(crew, bones, a, b, c, T);

        // hand / foot orientation: turn with the vehicle bone (steering wheel, pedal) - when that bone rotates.
        // The chain rotations above turned it by r2 * r1; replace that by the vehicle bone's rotation in crew space.
        const glm::mat4 delta = toCrew * vehBones_[vb] * sl.xfRest; // rest crew space -> current crew space
        glm::quat qv = glm::quat_cast(glm::mat3(delta));
        if (std::abs(qv.w) < 0.99999f) // the vehicle bone rotates
        {
            const glm::quat qChain = qChainRot;
            glm::quat corr = glm::normalize(qv * glm::inverse(qChain));
            glm::vec3 end = glm::vec3(bones[c] * glm::vec4(crew.bones[c].pivot, 1.0f));
            std::vector<int> endTree;
            Subtree(crew, c, endTree);
            const glm::mat4 r3 = RotateAbout(end, corr);
            for (int j : endTree) bones[j] = r3 * bones[j];
        }
    }
}

bool Attachments::RefreshIK()
{
    if (!ik) return false;
    auto model = cache_.Get(fs::path(Utf8ToWide(crewModel)), true, projectRoot_);
    if (model->status != AttachedModel::Status::Ok || !model->rigged) return false;
    bool changed = false;
    for (auto& s : seats_)
    {
        if (s.posed.empty() || !s.poseA || (s.ikBone[0].empty() && s.ikBone[1].empty() && s.ikBone[2].empty() && s.ikBone[3].empty())) continue;
        std::vector<glm::mat4> bones;
        s.poseStack.Evaluate(model->skeleton, 0.0, bones);
        SkinSeat(s, *model, bones);
        changed = true;
    }
    return changed;
}

// Get-in position: the memory point, facing towards the ...Dir point (checked on a mod vehicle: the get-in animation then
// steps into the vehicle).
void Attachments::ResolveGetIn(CrewSeat& s) const
{
    s.hasGetIn = false;
    if (!memory_ || s.getInPoint.empty()) return;
    auto centroid = [&](const std::string& name, glm::vec3& out) {
        for (const auto& sel : memory_->selections)
        {
            if (!IEquals(sel.name, name)) continue;
            glm::vec3 sum(0.0f);
            int n = 0;
            for (size_t p = 0; p < sel.pointBytes.size() && p < memory_->points.size(); ++p)
                if (sel.pointBytes[p]) { sum += memory_->points[p].pos; ++n; }
            if (!n) return false;
            out = sum / float(n);
            return true;
        }
        return false;
    };
    glm::vec3 p, d;
    if (!centroid(s.getInPoint, p)) return;
    glm::vec3 f(0, 0, -1);
    if (!s.getInDir.empty() && centroid(s.getInDir, d))
    {
        glm::vec3 v = p - d;
        v.y = 0.0f;
        if (glm::length(v) > 1e-4f) f = glm::normalize(v);
    }
    const glm::vec3 up(0, 1, 0), aside = glm::cross(up, f);
    // same convention as a proxy (z -> f): the character (front -Z) then faces from the point towards ...Dir
    s.getInXf = glm::mat4(glm::vec4(aside, 0), glm::vec4(up, 0), glm::vec4(f, 0), glm::vec4(p.x, 0.0f, p.z, 1));
    s.getInXf[3].y = p.y - 0.0f;
    s.hasGetIn = true;
}

bool Attachments::AddToChain(CrewSeat& s, const std::string& state, const ArmaConfig* cfg, const fs::path& projectRoot, bool atGetIn)
{
    auto& log = Log::Get();
    MovesStateInfo info = LookupMovesState(cfg, state);
    if (!info.found) { log.Warn("Crew {}: state {} not found", s.title, state); return false; }
    fs::path file = ResolveArmaPath(info.file, projectRoot);
    if (!file.has_extension()) file += ".rtm";
    std::string err;
    auto anim = LoadCachedRtm(file, err);
    if (!anim) { log.Warn("Crew {}: state {}: {}", s.title, state, err); return false; }
    CrewSeat::ChainItem it;
    it.state = state;
    it.rtm = anim;
    it.duration = DurationFromMovesSpeed(info.speed);
    if (it.duration <= 0.0f) it.duration = 1.0f; // static pose: hold it for a second in the chain
    it.interpolationSpeed = info.interpolationSpeed;
    it.looped = info.looped;
    it.atGetIn = atGetIn;
    s.chain.push_back(std::move(it));
    return true;
}

bool Attachments::Advance(double dt)
{
    bool changed = false;
    for (auto& s : seats_)
    {
        if (s.posed.empty()) continue;
        if (s.chainIndex >= 0 && s.chainIndex < int(s.chain.size()))
        {
            // chain: item i plays one cycle; the next one fades in over 1 / its interpolationSpeed
            s.chainTime += dt * speedAll * s.speed;
            const auto& cur = s.chain[s.chainIndex];
            s.atGetInNow = cur.atGetIn;
            float ph = float(s.chainTime / std::max(0.01f, cur.duration));
            if (ph >= 1.0f && s.chainIndex + 1 < int(s.chain.size()))
            {
                const auto& next = s.chain[s.chainIndex + 1];
                float blend = 1.0f / std::max(0.5f, next.interpolationSpeed);
                float into = float(s.chainTime - cur.duration);
                if (into >= blend)
                {
                    s.chainTime = into;
                    ++s.chainIndex;
                    PoseSeatAt(s, *next.rtm, std::min(1.0f, float(into / std::max(0.01f, next.duration))));
                }
                else
                    PoseSeatAt(s, *cur.rtm, 1.0f, next.rtm.get(), std::min(1.0f, into / std::max(0.01f, next.duration)), into / blend);
            }
            else if (ph >= 1.0f) // last item: hold, or start again
            {
                if (loopAll) { s.chainIndex = 0; s.chainTime = 0.0; }
                PoseSeatAt(s, *cur.rtm, 1.0f);
            }
            else
                PoseSeatAt(s, *cur.rtm, ph);
            changed = true;
            continue;
        }
        s.atGetInNow = false;
        if (!(playAll || s.playing) || !s.rtm || s.rtm->Frames() < 2 || s.duration <= 0.0f) continue; // static pose (speed 1e10)
        s.time += dt * speedAll * s.speed;
        double p = s.time / std::max(0.01f, s.duration);
        float ph = loopAll ? float(p - std::floor(p)) : float(std::min(p, 1.0));
        PoseSeatAt(s, *s.rtm, ph);
        changed = true;
    }
    return changed;
}

// ============================================================================================ geometry

void Attachments::AppendGeometry(std::vector<MeshVertex>& verts, std::vector<Section>& sections)
{
    instances_.clear();
    auto add = [&](int slot, int seat, const AttachedModel& m, const std::vector<MeshVertex>& src, int extra = -1) {
        Instance in{ slot, seat, uint32_t(verts.size()), uint32_t(src.size()), extra };
        for (const auto& sec : m.sections)
            sections.push_back({ sec.texture, sec.material, uint32_t(verts.size()) + sec.first, sec.count });
        verts.insert(verts.end(), src.begin(), src.end());
        instances_.push_back(in);
    };
    for (int i = 0; i < int(slots_.size()); ++i)
    {
        const ProxySlot& sl = slots_[i];
        if (i < int(hiddenSlot.size()) && hiddenSlot[i]) continue;
        const bool crewSlot = sl.role != SeatRole::None;
        if (sl.item && sl.item->status == AttachedModel::Status::Ok)
        {
            add(i, -1, *sl.item, sl.item->verts); // weapon slot with a chosen item
            continue;
        }
        // crew proxy: the mannequin model only while the crew preview is off
        if (showProxies && (!crewSlot || !showCrew) && (!sl.destroyed || showDestroyed) && (!sl.effect || showEffects) && sl.model &&
            sl.model->status == AttachedModel::Status::Ok)
            add(i, -1, *sl.model, sl.model->verts);
    }
    if (showCrew)
        for (int i = 0; i < int(seats_.size()); ++i)
        {
            const CrewSeat& s = seats_[i];
            if (s.slot < 0 || s.posed.empty()) continue;
            if (s.slot < int(hiddenSlot.size()) && hiddenSlot[s.slot]) continue;
            auto m = cache_.Get(fs::path(Utf8ToWide(crewModel)), true, {});
            if (m->status == AttachedModel::Status::Ok) add(s.slot, i, *m, s.posed);
        }
    for (int e = 0; e < int(extras.size()); ++e)
        if (extras[e].model && extras[e].model->status == AttachedModel::Status::Ok)
            add(-1, -1, *extras[e].model, extras[e].posed.empty() ? extras[e].model->verts : extras[e].posed, e);
    for (int u = 0; u < int(unitParts.size()); ++u)
        if (unitParts[u].model && unitParts[u].model->status == AttachedModel::Status::Ok)
        {
            add(-1, -1, *unitParts[u].model, unitParts[u].posed.empty() ? unitParts[u].model->verts : unitParts[u].posed);
            instances_.back().unit = u;
        }
}

void Attachments::TransformGeometry(std::vector<MeshVertex>& verts, size_t base) const
{
    for (const auto& in : instances_)
    {
        if (in.unit >= 0)
        {
            if (in.unit >= int(unitParts.size())) continue;
            const auto& src = unitParts[in.unit].posed;
            if (src.size() != in.count || in.first < base || in.first - base + in.count > verts.size()) continue;
            std::copy(src.begin(), src.end(), verts.begin() + (in.first - base));
            continue;
        }
        if (in.extra >= 0)
        {
            if (in.extra >= int(extras.size())) continue;
            const auto& src = extras[in.extra].posed;
            if (src.size() != in.count || in.first < base || in.first - base + in.count > verts.size()) continue;
            std::copy(src.begin(), src.end(), verts.begin() + (in.first - base));
            continue;
        }
        if (in.slot < 0 || in.slot >= int(slots_.size())) continue;
        if (in.seat < 0 && slots_[in.slot].item)
        {
            const ProxySlot& sl = slots_[in.slot];
            const glm::mat4 m = sl.adjust * sl.xf;
            const glm::mat3 r(m);
            const auto& src = sl.item->verts;
            if (src.size() != in.count || in.first < base || in.first - base + in.count > verts.size()) continue;
            for (uint32_t k = 0; k < in.count; ++k)
            {
                MeshVertex& d = verts[in.first - base + k];
                d.pos = glm::vec3(m * glm::vec4(src[k].pos, 1.0f));
                d.normal = r * src[k].normal;
            }
            continue;
        }
        const CrewSeat* seat = in.seat >= 0 ? &seats_[in.seat] : nullptr;
        const glm::mat4& m = seat && realGetIn && seat->atGetInNow && seat->hasGetIn ? seat->getInXf : slots_[in.slot].xf;
        const glm::mat3 r(m);
        const std::vector<MeshVertex>* src = nullptr;
        if (in.seat >= 0) src = &seats_[in.seat].posed;
        else if (slots_[in.slot].model) src = &slots_[in.slot].model->verts;
        if (!src || src->size() != in.count || in.first < base || in.first - base + in.count > verts.size()) continue;
        for (uint32_t k = 0; k < in.count; ++k)
        {
            const MeshVertex& s = (*src)[k];
            MeshVertex& d = verts[in.first - base + k];
            d.pos = glm::vec3(m * glm::vec4(s.pos, 1.0f));
            d.normal = r * s.normal;
        }
    }
}

void Attachments::StubLines(std::vector<LineVertex>& out) const
{
    for (const auto& sl : slots_)
    {
        bool crew = sl.role != SeatRole::None;
        if (!sl.valid || (crew ? !showCrew : !showProxies) || (sl.destroyed && !showDestroyed) || (sl.effect && !showEffects)) continue;
        if (crew && sl.seat >= 0 && !seats_[sl.seat].posed.empty()) continue;
        if (!crew && sl.model && sl.model->status == AttachedModel::Status::Ok) continue;
        if (sl.item && sl.item->status == AttachedModel::Status::Ok) continue;
        glm::vec4 col = crew ? (sl.seat >= 0 && seats_[sl.seat].binarized ? glm::vec4(1.0f, 0.6f, 0.1f, 1) : glm::vec4(1.0f, 0.25f, 0.2f, 1))
                      : sl.model && sl.model->status == AttachedModel::Status::Binarized ? glm::vec4(0.5f, 0.75f, 1.0f, 1) : glm::vec4(1.0f, 0.25f, 0.2f, 1);
        // a seated person roughly (crew) or a 0.3 m box, in proxy space (+Y up, -Z forward after the flip)
        glm::vec3 lo = crew ? glm::vec3(-0.25f, 0.0f, -0.3f) : glm::vec3(-0.15f), hi = crew ? glm::vec3(0.25f, 1.0f, 0.3f) : glm::vec3(0.15f);
        glm::vec3 c[8];
        for (int i = 0; i < 8; ++i) c[i] = glm::vec3(sl.xf * glm::vec4(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z, 1.0f));
        static const int e[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        for (auto& ed : e) { out.push_back({ c[ed[0]], col }); out.push_back({ c[ed[1]], col }); }
        // facing: proxy forward
        glm::vec3 o = glm::vec3(sl.xf[3]), f = glm::vec3(sl.xf * glm::vec4(0, 0, -0.5f, 1.0f));
        out.push_back({ o, col });
        out.push_back({ f, col });
    }
}

std::vector<Attachments::Label> Attachments::StubLabels() const
{
    std::vector<Label> out;
    for (const auto& sl : slots_)
    {
        bool crew = sl.role != SeatRole::None;
        if (!sl.valid || (crew ? !showCrew : !showProxies) || (sl.destroyed && !showDestroyed) || (sl.effect && !showEffects)) continue;
        std::string why;
        if (crew)
        {
            if (sl.seat >= 0 && !seats_[sl.seat].posed.empty()) continue;
            why = sl.seat < 0 ? "no seat" : seats_[sl.seat].binarized ? "binarized" : "no pose";
        }
        else
        {
            if (sl.model && sl.model->status == AttachedModel::Status::Ok) continue;
            why = sl.model && sl.model->status == AttachedModel::Status::Binarized ? "binarized" : "missing";
        }
        out.push_back({ glm::vec3(sl.xf * glm::vec4(0, crew ? 1.05f : 0.2f, 0, 1)), std::format("{}.{:03} ({})", sl.stem, sl.index, why), crew });
    }
    return out;
}

std::vector<fs::path> Attachments::WatchFiles() const
{
    std::vector<fs::path> out = cache_.Files();
    std::error_code ec;
    for (const auto& s : seats_)
        if (!s.rtmFile.empty() && fs::exists(s.rtmFile, ec)) out.push_back(s.rtmFile);
    return out;
}

void Attachments::Invalidate(const fs::path& file)
{
    cache_.Invalidate(file);
    rtmCache_.erase(ToLower(PathToUtf8(file)) == "" ? std::wstring() : Utf8ToWide(ToLower(PathToUtf8(file))));
}

// one read per RTM file: every seat with the same action shares it (the posing only reads it)
std::shared_ptr<RtmAnimation> Attachments::LoadCachedRtm(const fs::path& file, std::string& err)
{
    const std::wstring key = Utf8ToWide(ToLower(PathToUtf8(file)));
    if (auto it = rtmCache_.find(key); it != rtmCache_.end()) return it->second;
    auto anim = std::make_shared<RtmAnimation>();
    std::vector<std::string> warnings;
    if (!LoadRtm(file, *anim, err, warnings)) return nullptr;
    rtmCache_.emplace(key, anim);
    return anim;
}
