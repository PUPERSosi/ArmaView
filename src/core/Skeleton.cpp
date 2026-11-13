#include "Skeleton.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

#include "CoreGuard.h"

namespace fs = std::filesystem;

int Skeleton::Depth(int bone) const
{
    int d = 0;
    for (int p = bones[bone].parent; p >= 0 && d <= int(bones.size()); p = bones[p].parent) ++d;
    return d;
}

bool Skeleton::IsAncestor(int ancestor, int bone) const
{
    int guard = 0;
    for (int p = bones[bone].parent; p >= 0 && guard <= int(bones.size()); p = bones[p].parent, ++guard)
        if (p == ancestor) return true;
    return false;
}

std::vector<int> Skeleton::ParentFirstOrder() const
{
    std::vector<int> order, stack;
    for (int i = int(bones.size()) - 1; i >= 0; --i)
        if (bones[i].parent < 0) stack.push_back(i);
    while (!stack.empty())
    {
        int b = stack.back();
        stack.pop_back();
        order.push_back(b);
        for (auto it = bones[b].children.rbegin(); it != bones[b].children.rend(); ++it) stack.push_back(*it);
    }
    if (order.size() != bones.size())
    {
        order.resize(bones.size());
        for (int i = 0; i < int(order.size()); ++i) order[i] = i;
    }
    return order;
}

int Skeleton::Find(std::string_view bone) const
{
    for (int i = 0; i < int(bones.size()); ++i)
        if (IEquals(bones[i].name, bone)) return i;
    return -1;
}

namespace {

void CollectBones(const CfgClass& skels, const CfgClass& sk, Skeleton& out, std::set<std::string>& visited)
{
    auto& log = Log::Get();
    if (!visited.insert(ToLower(sk.name)).second)
    {
        log.Error("CfgSkeletons: skeletonInherit cycle at {}", sk.name);
        return;
    }
    std::string inherit = sk.GetString("skeletonInherit");
    if (!inherit.empty())
    {
        if (const CfgClass* parent = skels.FindClass(inherit))
            CollectBones(skels, *parent, out, visited);
        else
            log.Error("CfgSkeletons::{}: skeletonInherit \"{}\" not found", sk.name, inherit);
    }
    const CfgEntry* e = sk.Find("skeletonBones");
    if (!e) return;
    std::vector<std::string> list = sk.GetStrings("skeletonBones");
    if (list.size() % 2)
        log.Warn("CfgSkeletons::{}: skeletonBones has an odd number of entries ({}); last one ignored", sk.name, list.size());
    for (size_t i = 0; i + 1 < list.size(); i += 2)
    {
        if (list[i].empty()) { log.Warn("CfgSkeletons::{}: empty bone name at pair {}", sk.name, i / 2); continue; }
        if (out.Find(list[i]) >= 0) { log.Warn("CfgSkeletons::{}: bone \"{}\" defined twice", sk.name, list[i]); continue; }
        Bone b;
        b.name = list[i];
        b.parentName = list[i + 1];
        out.bones.push_back(std::move(b));
    }
}

void CollectSections(const CfgClass& models, const CfgClass& mc, std::vector<std::string>& out, int depth)
{
    if (depth > 16) return;
    for (auto& s : mc.GetStrings("sections")) out.push_back(s);
    std::string inh = mc.GetString("sectionsInherit");
    if (!inh.empty())
    {
        if (const CfgClass* p = models.FindClass(inh)) CollectSections(models, *p, out, depth + 1);
        else Log::Get().Warn("CfgModels::{}: sectionsInherit \"{}\" not found", mc.name, inh);
    }
}

const P3DLod* FindMemoryLod(const P3DModel& m)
{
    for (const auto& l : m.lods)
        if (std::abs(double(l.resolution) - 1.0e15) < 1.0e10) return &l;
    return m.lods.empty() ? nullptr : &m.lods.front();
}

const P3DSelection* FindSelection(const P3DLod& lod, std::string_view name)
{
    for (const auto& s : lod.selections)
        if (!s.engineIgnored && IEquals(s.name, name)) return &s;
    return nullptr;
}

} // namespace

bool BuildSkeleton(const ArmaConfig& cfg, const std::string& modelName, Skeleton& out, ModelCfgInfo& info)
{
    out = {};
    info = {};
    auto& log = Log::Get();
    const CfgClass* models = cfg.Root().FindClass("CfgModels");
    if (!models) { log.Warn("model.cfg has no CfgModels"); return false; }
    const CfgClass* mc = models->FindClass(modelName);
    if (!mc)
    {
        log.Warn("CfgModels has no class \"{}\" (model name) - no skeleton", modelName);
        return false;
    }
    info.modelClass = mc->name;
    CollectSections(*models, *mc, info.sections, 0);
    info.skeletonName = mc->GetString("skeletonName");
    if (info.skeletonName.empty())
    {
        log.Info("CfgModels::{}: no skeletonName", mc->name);
        return false;
    }
    const CfgClass* skels = cfg.Root().FindClass("CfgSkeletons");
    const CfgClass* sk = skels ? skels->FindClass(info.skeletonName) : nullptr;
    if (!sk)
    {
        log.Error("CfgModels::{}: skeleton \"{}\" not found in CfgSkeletons", mc->name, info.skeletonName);
        return false;
    }

    out.name = sk->name;
    out.inherit = sk->GetString("skeletonInherit");
    out.isDiscrete = sk->GetNumber("isDiscrete", 0) != 0;
    out.pivotsModel = sk->GetString("pivotsModel");
    std::set<std::string> visited;
    CollectBones(*skels, *sk, out, visited);

    for (int i = 0; i < int(out.bones.size()); ++i)
    {
        Bone& b = out.bones[i];
        if (b.parentName.empty()) continue;
        b.parent = out.Find(b.parentName);
        if (b.parent < 0)
            log.Warn("Skeleton {}: bone \"{}\" has unknown parent \"{}\" (treated as root)", out.name, b.name, b.parentName);
        else if (b.parent == i)
        {
            log.Error("Skeleton {}: bone \"{}\" is its own parent", out.name, b.name);
            b.parent = -1;
        }
        else
            out.bones[b.parent].children.push_back(i);
    }
    // parent cycles
    for (int i = 0; i < int(out.bones.size()); ++i)
    {
        int steps = 0;
        for (int p = out.bones[i].parent; p >= 0 && steps <= int(out.bones.size()); p = out.bones[p].parent) ++steps;
        if (steps > int(out.bones.size())) log.Error("Skeleton {}: parent cycle through bone \"{}\"", out.name, out.bones[i].name);
    }
    return true;
}

void ResolvePivots(Skeleton& skel, const P3DModel* pivots, const P3DLod* lod)
{
    const P3DLod* mem = pivots ? FindMemoryLod(*pivots) : nullptr;
    for (Bone& b : skel.bones)
    {
        b.pivotSource = PivotSource::None;
        b.pivot = glm::vec3(0.0f);
        if (mem)
            if (const P3DSelection* s = FindSelection(*mem, b.name); s && s->pointCount)
            {
                glm::vec3 sum(0.0f);
                size_t n = 0;
                for (size_t i = 0; i < s->pointBytes.size() && i < mem->points.size(); ++i)
                    if (s->pointBytes[i]) { sum += mem->points[i].pos; ++n; }
                b.pivot = sum / float(n);
                b.pivotSource = PivotSource::PivotsModel;
                continue;
            }
        if (lod)
            if (const P3DSelection* s = FindSelection(*lod, b.name); s && s->pointCount)
            {
                glm::vec3 sum(0.0f);
                float wsum = 0.0f;
                for (size_t i = 0; i < s->pointBytes.size() && i < lod->points.size(); ++i)
                {
                    float w = s->Weight(i);
                    sum += lod->points[i].pos * w;
                    wsum += w;
                }
                if (wsum > 0.0f)
                {
                    b.pivot = sum / wsum;
                    b.pivotSource = PivotSource::SelectionCentroid;
                }
            }
    }
}

std::vector<int> MapBonesToSelections(const Skeleton& skel, const P3DLod& lod)
{
    std::unordered_map<std::string, int> byName;
    for (int i = 0; i < int(lod.selections.size()); ++i)
        if (lod.selections[i].pointCount && !lod.selections[i].engineIgnored) byName.emplace(ToLower(lod.selections[i].name), i);
    std::vector<int> map(skel.bones.size(), -1);
    for (size_t b = 0; b < skel.bones.size(); ++b)
        if (auto it = byName.find(ToLower(skel.bones[b].name)); it != byName.end()) map[b] = it->second;
    return map;
}

ValidationReport Validate(const Skeleton& skel, const P3DLod& lod, const std::vector<std::string>& sections)
{
    ValidationReport r;
    if (skel.Empty()) return r;
    r.valid = true;
    r.points = lod.points.size();

    std::set<std::string> boneNames, sectionNames;
    for (const auto& b : skel.bones) boneNames.insert(ToLower(b.name));
    for (const auto& s : sections) sectionNames.insert(ToLower(s));

    std::vector<const P3DSelection*> boneSels;
    std::set<std::string> selNames;
    for (const auto& s : lod.selections)
    {
        std::string ln = ToLower(s.name);
        if (s.engineIgnored)
        {
            r.otherNotInSkeleton.push_back(s.name + " [ignored by engine: '-'/'.']");
            continue;
        }
        if (s.pointCount) selNames.insert(ln);
        if (boneNames.count(ln))
        {
            if (s.pointCount) boneSels.push_back(&s);
            continue;
        }
        if (s.partialCount && !ln.starts_with("proxy:") && !sectionNames.count(ln))
            r.weightedNotInSkeleton.push_back(s.name);
        else
            r.otherNotInSkeleton.push_back(s.name + (sectionNames.count(ln) ? " [section]" : ""));
    }
    for (const auto& b : skel.bones)
        if (!selNames.count(ToLower(b.name))) r.bonesWithoutSelection.push_back(b.name);

    r.discrete = skel.isDiscrete;
    std::vector<int> selBone(boneSels.size());
    for (size_t i = 0; i < boneSels.size(); ++i) selBone[i] = skel.Find(boneSels[i]->name);
    std::vector<int> bonesOfPoint;
    for (uint32_t p = 0; p < lod.points.size(); ++p)
    {
        int count = 0;
        float sum = 0.0f;
        bonesOfPoint.clear();
        for (size_t i = 0; i < boneSels.size(); ++i)
        {
            float w = boneSels[i]->Weight(p);
            if (w > 0.0f) { ++count; sum += w; bonesOfPoint.push_back(selBone[i]); }
        }
        if (count == 0) { r.noBone.push_back(p); continue; }
        if (r.discrete)
        {
            // Discrete: a point may sit in nested selections (bone + its ancestors); it belongs to the deepest one.
            int deepest = bonesOfPoint[0];
            for (int b : bonesOfPoint) if (skel.Depth(b) > skel.Depth(deepest)) deepest = b;
            for (int b : bonesOfPoint)
                if (b != deepest && !skel.IsAncestor(b, deepest)) { r.badWeightSum.push_back({ p, count, sum }); break; }
            continue;
        }
        if (count > 4) r.tooManyBones.push_back({ p, count, sum });
        if (std::abs(sum - 1.0f) > 0.01f) r.badWeightSum.push_back({ p, count, sum });
    }
    return r;
}

// ============================================================================================ model.cfg animations

namespace {

// Animation classes in definition order; a derived "class Animations : Base" starts from the base list.
void CollectAnimClasses(const CfgClass& anims, std::vector<const CfgClass*>& out, int depth)
{
    if (depth > 16) return;
    if (anims.base) CollectAnimClasses(*anims.base, out, depth + 1);
    for (const auto& k : anims.classes)
    {
        if (k->external) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](const CfgClass* c) { return IEquals(c->name, k->name); });
        if (it != out.end()) *it = k.get();
        else out.push_back(k.get());
    }
}

glm::vec3 SelectionCentroid(const P3DLod& lod, const P3DSelection& s)
{
    glm::vec3 sum(0.0f);
    size_t n = 0;
    for (size_t i = 0; i < s.pointBytes.size() && i < lod.points.size(); ++i)
        if (s.pointBytes[i]) { sum += lod.points[i].pos; ++n; }
    return n ? sum / float(n) : sum;
}

// Points of a selection in index order.
std::vector<glm::vec3> SelectionPoints(const P3DLod& lod, const P3DSelection& s)
{
    std::vector<glm::vec3> pts;
    for (size_t i = 0; i < s.pointBytes.size() && i < lod.points.size(); ++i)
        if (s.pointBytes[i]) pts.push_back(lod.points[i].pos);
    return pts;
}

} // namespace

float CfgAnimation::Phase(float v) const
{
    float range = maxValue - minValue;
    float p = std::abs(range) > 1e-9f ? (v - minValue) / range : 0.0f;
    switch (address)
    {
    case SourceAddress::Loop: return p - std::floor(p);
    case SourceAddress::Mirror:
    {
        float m = std::fmod(std::abs(p), 2.0f);
        return m > 1.0f ? 2.0f - m : m;
    }
    default: return std::clamp(p, 0.0f, 1.0f);
    }
}

glm::mat4 CfgAnimation::Matrix(float v, const glm::vec3& bonePivot) const
{
    const float p = Phase(v);
    auto rotateAbout = [](const glm::vec3& c, const glm::vec3& dir, float a) {
        glm::mat4 m = glm::translate(glm::mat4(1.0f), c);
        m = glm::rotate(m, a, dir);
        return glm::translate(m, -c);
    };
    switch (type)
    {
    case CfgAnimType::Rotation:
        if (!axisOk) return glm::mat4(1.0f);
        return rotateAbout(axisPos, axisDir, angle0 + (angle1 - angle0) * p);
    case CfgAnimType::RotationX:
    case CfgAnimType::RotationY:
    case CfgAnimType::RotationZ:
    {
        glm::vec3 dir(type == CfgAnimType::RotationX, type == CfgAnimType::RotationY, type == CfgAnimType::RotationZ);
        return rotateAbout(axisOk ? axisPos : glm::vec3(0.0f), dir, angle0 + (angle1 - angle0) * p);
    }
    case CfgAnimType::Translation:
        if (!axisOk) return glm::mat4(1.0f);
        return glm::translate(glm::mat4(1.0f), axisDir * axisLength * (offset0 + (offset1 - offset0) * p));
    case CfgAnimType::TranslationX:
    case CfgAnimType::TranslationY:
    case CfgAnimType::TranslationZ:
    {
        glm::vec3 dir(type == CfgAnimType::TranslationX, type == CfgAnimType::TranslationY, type == CfgAnimType::TranslationZ);
        return glm::translate(glm::mat4(1.0f), dir * (offset0 + (offset1 - offset0) * p));
    }
    case CfgAnimType::Direct:
    {
        if (!axisOk) return glm::mat4(1.0f);
        glm::mat4 m = glm::translate(glm::mat4(1.0f), axisDir * directOffset * p);
        return m * rotateAbout(axisPos, axisDir, directAngle * p);
    }
    case CfgAnimType::Hide:
    {
        bool hidden = p >= hideValue && (unHideValue < 0.0f || p < unHideValue);
        if (!hidden) return glm::mat4(1.0f);
        // collapse to the bone pivot (children inherit -> hidden too)
        glm::mat4 m = glm::translate(glm::mat4(1.0f), bonePivot);
        m = glm::scale(m, glm::vec3(0.0f));
        return glm::translate(m, -bonePivot);
    }
    default: return glm::mat4(1.0f);
    }
}

void BuildCfgAnimations(const ArmaConfig& cfg, const ModelCfgInfo& info, const Skeleton& skel,
                        std::vector<CfgAnimation>& anims, std::vector<CfgSource>& sources)
{
    anims.clear();
    sources.clear();
    const CfgClass* models = cfg.Root().FindClass("CfgModels");
    const CfgClass* mc = models && !info.modelClass.empty() ? models->FindClass(info.modelClass) : nullptr;
    const CfgClass* ac = mc ? mc->FindClass("Animations") : nullptr;
    if (!ac) return;
    std::vector<const CfgClass*> classes;
    CollectAnimClasses(*ac, classes, 0);

    static const std::pair<const char*, CfgAnimType> types[] = {
        { "rotation", CfgAnimType::Rotation }, { "rotationx", CfgAnimType::RotationX }, { "rotationy", CfgAnimType::RotationY },
        { "rotationz", CfgAnimType::RotationZ }, { "translation", CfgAnimType::Translation }, { "translationx", CfgAnimType::TranslationX },
        { "translationy", CfgAnimType::TranslationY }, { "translationz", CfgAnimType::TranslationZ }, { "hide", CfgAnimType::Hide }, { "direct", CfgAnimType::Direct },
    };
    for (const CfgClass* k : classes)
    {
        CfgAnimation a;
        a.name = k->name;
        a.file = k->file;
        a.line = k->line;
        a.typeName = k->GetString("type");
        std::string t = ToLower(a.typeName);
        for (auto& [n, v] : types)
            if (t == n) a.type = v;
        if (a.type == CfgAnimType::Unsupported) a.problem = std::format("type \"{}\" not supported", a.typeName);
        a.source = k->GetString("source");
        a.selection = k->GetString("selection");
        a.axis = k->GetString("axis");
        a.begin = k->GetString("begin");
        a.end = k->GetString("end");
        a.axisPosName = k->GetString("axisPos");
        a.axisDirName = k->GetString("axisDir");
        a.directAngle = float(k->GetNumber("angle", 0.0));
        a.directOffset = float(k->GetNumber("axisOffset", 0.0));
        a.memory = k->GetNumber("memory", 1.0) != 0.0;
        std::string cfv = ToLower(k->GetString("centerFirstVertex"));
        a.centerFirstVertex = cfv == "true" || cfv == "1";
        a.minValue = float(k->GetNumber("minValue", 0.0));
        a.maxValue = float(k->GetNumber("maxValue", 1.0));
        a.angle0 = float(k->GetNumber("angle0", 0.0));
        a.angle1 = float(k->GetNumber("angle1", 0.0));
        a.offset0 = float(k->GetNumber("offset0", 0.0));
        a.offset1 = float(k->GetNumber("offset1", 0.0));
        a.hideValue = float(k->GetNumber("hideValue", 0.0));
        a.unHideValue = k->Find("unHideValue") ? float(k->GetNumber("unHideValue", -1.0)) : -1.0f;
        for (const char* key : { "angle0", "angle1", "offset0", "offset1", "minValue", "maxValue", "hideValue", "unHideValue" })
            if (k->Find(key) && !k->HasNumber(key))
                Log::Get().Warn("{}: animation {}: {} = \"{}\" is not a number (0 used)", cfg.Where(k->file, k->line), a.name, key, k->GetString(key));

        std::string addr = ToLower(k->GetString("sourceAddress", "clamp"));
        if (addr == "mirror") a.address = SourceAddress::Mirror;
        else if (addr == "loop") a.address = SourceAddress::Loop;
        else if (addr != "clamp" && addr != "0")
            Log::Get().Warn("{}: animation {}: sourceAddress \"{}\" treated as clamp", cfg.Where(k->file, k->line), a.name, addr);

        if (a.problem.empty())
        {
            if (a.selection.empty()) a.problem = "no selection";
            else if ((a.bone = skel.Find(a.selection)) < 0)
            {
                a.problem = std::format("selection \"{}\" is not a skeleton bone (ignored by Arma)", a.selection);
                a.problemIsWarning = true;
            }
            else if (a.source.empty()) a.problem = "no source";
        }
        if (!a.source.empty())
        {
            auto it = std::find_if(sources.begin(), sources.end(), [&](const CfgSource& s) { return IEquals(s.name, a.source); });
            if (it == sources.end())
            {
                sources.push_back({ a.source, std::min(a.minValue, a.maxValue), std::max(a.minValue, a.maxValue), 0.0f, false });
                it = sources.end() - 1;
            }
            else
            {
                it->minValue = std::min({ it->minValue, a.minValue, a.maxValue });
                it->maxValue = std::max({ it->maxValue, a.minValue, a.maxValue });
            }
            a.sourceIndex = int(it - sources.begin());
        }
        anims.push_back(std::move(a));
    }
    for (auto& s : sources) s.value = std::clamp(0.0f, s.minValue, s.maxValue);
}

void ResolveAnimationAxes(std::vector<CfgAnimation>& anims, const P3DModel& model, const P3DLod& lod, bool memoryFallback)
{
    const P3DLod* mem = nullptr;
    for (const auto& l : model.lods)
        if (std::abs(double(l.resolution) - 1.0e15) < 1.0e10) mem = &l;
    for (auto& a : anims)
    {
        a.axisOk = false;
        a.axisProblem.clear();
        a.axisWarning.clear();
        const P3DLod* src = a.memory ? mem : &lod;
        if (!a.memory && mem && memoryFallback)
        {
            // memory=0 but the axis points are only in the Memory LOD: use them and flag it
            auto has = [&](const P3DLod* l, const std::string& n) { const P3DSelection* s = n.empty() ? nullptr : FindSelection(*l, n); return s && s->pointCount; };
            std::string probe = a.type == CfgAnimType::Direct ? a.axisPosName : !a.begin.empty() ? a.begin : a.axis;
            if (!probe.empty() && !has(&lod, probe) && has(mem, probe))
            {
                src = mem;
                a.axisWarning = std::format("memory=0, but axis \"{}\" is only in the Memory LOD - the documentation asks for memory=1 (the viewer moves it through the Memory LOD)", probe);
            }
        }
        const bool needsAxis = a.type == CfgAnimType::Rotation || a.type == CfgAnimType::Translation || a.type == CfgAnimType::Direct;
        if (a.type == CfgAnimType::Hide || a.type == CfgAnimType::Unsupported) continue;
        if (!src)
        {
            a.axisProblem = "model has no Memory LOD";
            continue;
        }
        std::vector<glm::vec3> pts;
        if (a.type == CfgAnimType::Direct)
        {
            const P3DSelection* ps = FindSelection(*src, a.axisPosName);
            const P3DSelection* ds = FindSelection(*src, a.axisDirName);
            if (!ps || !ps->pointCount || !ds || !ds->pointCount)
            {
                a.axisProblem = std::format("axisPos/axisDir \"{}\"/\"{}\" not found in {} LOD", a.axisPosName, a.axisDirName, a.memory ? "Memory" : "this");
                continue;
            }
            glm::vec3 p0 = SelectionCentroid(*src, *ps), p1 = SelectionCentroid(*src, *ds);
            float len = glm::length(p1 - p0);
            if (len < 1e-6f) { a.axisProblem = "axisPos and axisDir coincide"; continue; }
            a.axisPos = p0;
            a.axisDir = (p1 - p0) / len;
            a.axisLength = len;
            a.axisOk = true;
            continue;
        }
        if (!a.begin.empty() || !a.end.empty())
        {
            const P3DSelection* b = FindSelection(*src, a.begin);
            const P3DSelection* e = FindSelection(*src, a.end);
            if (!b || !b->pointCount || !e || !e->pointCount)
            {
                a.axisProblem = std::format("begin/end \"{}\"/\"{}\" not found in {} LOD", a.begin, a.end, a.memory ? "Memory" : "this");
                continue;
            }
            pts = { SelectionCentroid(*src, *b), SelectionCentroid(*src, *e) };
        }
        else if (!a.axis.empty())
        {
            const P3DSelection* s = FindSelection(*src, a.axis);
            if (!s || !s->pointCount)
            {
                a.axisProblem = std::format("axis \"{}\" not found in {} LOD", a.axis, a.memory ? "Memory" : "this");
                continue;
            }
            pts = SelectionPoints(*src, *s);
        }
        else
        {
            if (needsAxis) a.axisProblem = "no axis";
            continue; // rotationX/Y/Z without axis: around the origin
        }

        if (a.type == CfgAnimType::Rotation || a.type == CfgAnimType::Translation)
        {
            if (pts.size() < 2)
            {
                a.axisProblem = std::format("axis \"{}\" needs 2 points, has {}", a.axis.empty() ? a.begin : a.axis, pts.size());
                continue;
            }
            glm::vec3 d = pts[1] - pts[0];
            a.axisLength = glm::length(d);
            if (a.axisLength < 1e-6f) { a.axisProblem = "axis points coincide"; continue; }
            a.axisDir = d / a.axisLength;
            a.axisPos = pts[0];
        }
        else // rotationX/Y/Z: axis selection gives the centre
        {
            glm::vec3 c(0.0f);
            for (auto& p : pts) c += p;
            a.axisPos = a.centerFirstVertex ? pts[0] : c / float(pts.size());
        }
        a.axisOk = true;
    }
}

void EvaluateCfgAnimations(const Skeleton& skel, const std::vector<CfgAnimation>& anims, const std::vector<CfgSource>& sources,
                           std::vector<glm::mat4>& out)
{
    const size_t nb = skel.bones.size();
    std::vector<glm::mat4> local(nb, glm::mat4(1.0f));
    for (const auto& a : anims)
    {
        if (!a.problem.empty() || a.bone < 0 || a.sourceIndex < 0) continue;
        if ((a.type == CfgAnimType::Rotation || a.type == CfgAnimType::Translation || a.type == CfgAnimType::Direct) && !a.axisOk) continue;
        local[a.bone] = a.Matrix(sources[a.sourceIndex].value, skel.bones[a.bone].pivot) * local[a.bone];
    }
    out.assign(nb, glm::mat4(1.0f));
    for (int b : skel.ParentFirstOrder())
    {
        int p = skel.bones[b].parent;
        out[b] = p >= 0 ? out[p] * local[b] : local[b];
    }
}

SkinData BuildSkin(const Skeleton& skel, const P3DLod& lod, bool normalize, int maxBones)
{
    SkinData s;
    const size_t n = lod.points.size();
    std::vector<int> map = MapBonesToSelections(skel, lod);
    s.offset.assign(n + 1, 0);
    s.restWeight.assign(n, 1.0f);
    std::vector<std::vector<SkinData::Influence>> per(n);
    for (int b = 0; b < int(map.size()); ++b)
    {
        if (map[b] < 0) continue;
        const P3DSelection& sel = lod.selections[map[b]];
        for (size_t p = 0; p < n; ++p)
            if (float w = sel.Weight(p); w > 0.0f) per[p].push_back({ b, w });
    }
    for (size_t p = 0; p < n; ++p)
    {
        auto& v = per[p];
        if (skel.isDiscrete && v.size() > 1) // one bone per point: the deepest in the hierarchy
        {
            auto best = std::max_element(v.begin(), v.end(), [&](auto& a, auto& b) { return skel.Depth(a.bone) < skel.Depth(b.bone); });
            v = { { best->bone, 1.0f } };
        }
        else if (skel.isDiscrete && v.size() == 1)
            v[0].weight = 1.0f;
        if (maxBones > 0 && int(v.size()) > maxBones)
        {
            std::partial_sort(v.begin(), v.begin() + maxBones, v.end(), [](auto& a, auto& b) { return a.weight > b.weight; });
            v.resize(maxBones);
        }
        float sum = 0.0f;
        for (auto& i : v) sum += i.weight;
        if (normalize && sum > 0.0f)
        {
            for (auto& i : v) i.weight /= sum;
            sum = 1.0f;
        }
        s.restWeight[p] = std::max(0.0f, 1.0f - sum);
        s.offset[p] = uint32_t(s.influences.size());
        s.influences.insert(s.influences.end(), v.begin(), v.end());
    }
    s.offset[n] = uint32_t(s.influences.size());
    return s;
}

void SkinMatrices(const SkinData& skin, const std::vector<glm::mat4>& bones, std::vector<glm::mat4>& points)
{
    const size_t n = skin.restWeight.size();
    points.resize(n);
    for (size_t p = 0; p < n; ++p)
    {
        glm::mat4 m = glm::mat4(1.0f) * skin.restWeight[p];
        for (uint32_t i = skin.offset[p]; i < skin.offset[p + 1]; ++i)
        {
            const auto& inf = skin.influences[i];
            if (inf.bone < int(bones.size())) m += bones[inf.bone] * inf.weight;
        }
        if (skin.offset[p] == skin.offset[p + 1]) m = glm::mat4(1.0f);
        points[p] = m;
    }
}
