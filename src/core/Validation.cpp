#include "Validation.h"
#include "Log.h"
#include "Session.h"

#include <format>

#include "CoreGuard.h"

std::string JoinList(const std::vector<std::string>& v, size_t max)
{
    std::string r;
    for (size_t i = 0; i < v.size() && i < max; ++i) r += (i ? ", " : "") + v[i];
    if (v.size() > max) r += std::format(" ... (+{} more)", v.size() - max);
    return r;
}

void LogLodValidation(const Skeleton& skel, int lodIndex, const P3DLod& l, const ValidationReport& r)
{
    auto& log = Log::Get();
    if (r.discrete)
        log.Info("Validation LOD {} [{}] vs {} (isDiscrete): weighted selections not in skeleton {}, bones without selection {}, points in unrelated bones {}, points without bone {} (of {})",
                 lodIndex, l.Name(), skel.name, r.weightedNotInSkeleton.size(), r.bonesWithoutSelection.size(),
                 r.badWeightSum.size(), r.noBone.size(), r.points);
    else
        log.Info("Validation LOD {} [{}] vs {}: weighted selections not in skeleton {}, bones without selection {}, points with >4 bones {}, weight sum off by >0.01 {}, points without bone {} (of {})",
                 lodIndex, l.Name(), skel.name, r.weightedNotInSkeleton.size(), r.bonesWithoutSelection.size(),
                 r.tooManyBones.size(), r.badWeightSum.size(), r.noBone.size(), r.points);
    if (!r.weightedNotInSkeleton.empty()) log.Warn("  LOD {}: weighted selections not in skeleton: {}", lodIndex, JoinList(r.weightedNotInSkeleton, 40));
    if (!r.bonesWithoutSelection.empty()) log.Info("  LOD {}: skeleton bones without selection: {}", lodIndex, JoinList(r.bonesWithoutSelection, 20));
    log.Debug("  LOD {}: all bones without selection: {}", lodIndex, JoinList(r.bonesWithoutSelection, 100000));
}

void LogAllLodValidation(const Session& s)
{
    if (s.skeleton.Empty()) return;
    for (int i = 0; i < int(s.model.lods.size()); ++i)
    {
        const P3DLod& l = s.model.lods[size_t(i)];
        if (l.faces.empty() || l.resolution >= 1.0e4f) continue;
        LogLodValidation(s.skeleton, i, l, Validate(s.skeleton, l, s.modelInfo.sections));
    }
}

AnimStatus AnimStatusOf(const CfgAnimation& a, const std::vector<int>& boneSelection, int lod)
{
    const bool active = a.bone >= 0 && a.bone < int(boneSelection.size()) && boneSelection[size_t(a.bone)] >= 0;
    if (!a.problem.empty()) return { a.problemIsWarning ? Severity::Warning : Severity::Error, a.problem };
    if (!active) return { Severity::Inactive, std::format("no points in LOD {}", lod) };
    if (!a.axisProblem.empty()) return { Severity::Error, a.axisProblem };
    if (!a.axisWarning.empty()) return { Severity::Error, a.axisWarning }; // per BI wiki: memory=0 means the axis is in the same LOD
    return {};
}

ModelChecks CheckModel(const Session& s, const ValidationReport& v, const std::vector<int>& boneSelection)
{
    ModelChecks c;
    if (!s.skeleton.Empty())
    {
        c.text += std::format("skeleton {}: {} bones\n", s.skeleton.name, s.skeleton.bones.size());
        if (v.valid)
        {
            const size_t badSum = v.discrete ? 0 : v.badWeightSum.size();
            c.text += std::format("LOD {}: >4 bones {}, weight sum != 1 {}, points without bone {}, weighted selections not in skeleton {}\n", s.lod,
                                  v.tooManyBones.size(), badSum, v.noBone.size(), v.weightedNotInSkeleton.size());
        }
    }
    int animErr = 0;
    for (const auto& an : s.cfgAnims)
        if (const AnimStatus st = AnimStatusOf(an, boneSelection, s.lod); st.severity == Severity::Error)
        {
            ++animErr;
            c.text += std::format("  model.cfg ERROR {}: {}\n", an.name, st.message);
        }
    c.text += std::format("model.cfg animations {} ({} error(s))\n", s.cfgAnims.size(), animErr);
    c.errors += animErr;
    int crewBad = 0;
    for (const auto& seat : s.attach.Seats())
        if (!seat.problem.empty())
        {
            crewBad += !seat.problemIsWarning;
            c.warnings += seat.problemIsWarning;
            c.text += std::format("  crew {} {}: {}\n", seat.problemIsWarning ? "warning" : "ERROR", seat.title, seat.problem);
        }
    c.text += std::format("crew seats {} ({} with problems), proxies {}\n", s.attach.Seats().size(), crewBad, s.attach.Slots().size());
    c.errors += crewBad;
    return c;
}

FaceDiagInfo AnalyzeFaces(const P3DLod& l)
{
    FaceDiagInfo d;
    const size_t n = l.faces.size();
    d.flippedCorners.assign(n, 0);
    d.mirrored.assign(n, 0);
    d.degenerateUV.assign(n, 0);
    std::vector<glm::vec3> geo(n, glm::vec3(0));
    std::vector<float> uvArea(n, 0.0f);
    long agree = 0, against = 0, uvPos = 0, uvNeg = 0;
    auto valid = [&](const P3DFace& f) {
        if (f.count < 3) return false;
        for (uint32_t c = 0; c < f.count; ++c)
            if (f.v[c].point >= l.points.size() || f.v[c].normal >= l.normals.size()) return false;
        return true;
    };
    for (size_t fi = 0; fi < n; ++fi)
    {
        const P3DFace& f = l.faces[fi];
        if (!valid(f)) continue;
        const glm::vec3 p0 = l.points[f.v[0].point].pos;
        glm::vec3 g(0);
        float a = 0;
        for (uint32_t k = 1; k + 1 < f.count; ++k)
        {
            g += glm::cross(l.points[f.v[k].point].pos - p0, l.points[f.v[k + 1].point].pos - p0);
            const glm::vec2 e1(f.v[k].u - f.v[0].u, f.v[k].v - f.v[0].v), e2(f.v[k + 1].u - f.v[0].u, f.v[k + 1].v - f.v[0].v);
            a += e1.x * e2.y - e1.y * e2.x;
        }
        if (glm::dot(g, g) < 1e-20f) continue;
        geo[fi] = glm::normalize(g);
        uvArea[fi] = a;
        for (uint32_t c = 0; c < f.count; ++c) (glm::dot(l.normals[f.v[c].normal], geo[fi]) >= 0 ? agree : against)++;
        if (a > 1e-10f) ++uvPos;
        else if (a < -1e-10f) ++uvNeg;
    }
    d.windingSign = agree >= against ? 1 : -1;
    const int uvMajority = uvPos >= uvNeg ? 1 : -1;
    for (size_t fi = 0; fi < n; ++fi)
    {
        const P3DFace& f = l.faces[fi];
        if (geo[fi] == glm::vec3(0)) continue;
        for (uint32_t c = 0; c < f.count; ++c)
            if (glm::dot(l.normals[f.v[c].normal], geo[fi] * float(d.windingSign)) < -1e-4f) ++d.flippedCorners[fi];
        if (std::abs(uvArea[fi]) <= 1e-10f) d.degenerateUV[fi] = 1;
        else d.mirrored[fi] = (uvArea[fi] > 0 ? 1 : -1) != uvMajority;
    }
    return d;
}
