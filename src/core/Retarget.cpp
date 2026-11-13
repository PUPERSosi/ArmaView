#include "Retarget.h"
#include "ArmaConfig.h"
#include "P3D.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <map>

#include "CoreGuard.h"

namespace fs = std::filesystem;

namespace {

std::string Lower(std::string s)
{
    for (auto& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

std::string PathText(const fs::path& p)
{
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path FindModelCfgUp(const fs::path& p3d, const fs::path& root)
{
    std::error_code ec;
    for (fs::path d = p3d.parent_path(); !d.empty(); d = d.parent_path())
    {
        if (fs::exists(d / "model.cfg", ec)) return d / "model.cfg";
        if (d == root || d == d.root_path() || d.parent_path() == d) break;
    }
    return {};
}

// shortest rotation taking a onto b
glm::quat FromTo(glm::vec3 a, glm::vec3 b)
{
    const float la = glm::length(a), lb = glm::length(b);
    if (la < 1e-6f || lb < 1e-6f) return glm::quat(1, 0, 0, 0);
    a /= la;
    b /= lb;
    const float d = glm::dot(a, b);
    if (d > 0.99999f) return glm::quat(1, 0, 0, 0);
    if (d < -0.99999f)
    {
        glm::vec3 axis = glm::cross(glm::vec3(1, 0, 0), a);
        if (glm::length(axis) < 1e-3f) axis = glm::cross(glm::vec3(0, 1, 0), a);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 c = glm::cross(a, b);
    return glm::normalize(glm::quat(1.0f + d, c.x, c.y, c.z));
}

// Bind-pose correction with the twist fixed: frames from the bone direction and a world reference axis (up, or forward
// for nearly vertical bones) in both skeletons; rotation taking the target frame onto the source frame. Both skeletons
// face the same way (Arma model space), so the reference axis gives the same twist; a bare FromTo leaves the twist free
// and elbows / knees bend in a wrong plane.
glm::quat BindCorrection(glm::vec3 dT, glm::vec3 dS)
{
    if (glm::length(dT) < 1e-6f || glm::length(dS) < 1e-6f) return glm::quat(1, 0, 0, 0);
    auto frame = [](glm::vec3 d) {
        d = glm::normalize(d);
        glm::vec3 ref = std::abs(d.y) > 0.7f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        glm::vec3 z = glm::normalize(glm::cross(d, ref));
        glm::vec3 y = glm::cross(z, d);
        return glm::mat3(d, y, z);
    };
    const glm::vec3 nT = glm::normalize(dT), nS = glm::normalize(dS);
    // reference axis switch between the two (one bone near vertical, the other not): fall back to the shortest arc
    if ((std::abs(nT.y) > 0.7f) != (std::abs(nS.y) > 0.7f)) return FromTo(dT, dS);
    return glm::normalize(glm::quat_cast(frame(dS) * glm::transpose(frame(dT))));
}

// Best rotation taking the directions a[i] onto b[i] (least squares, Horn's quaternion method; the eigenvector of the
// largest eigenvalue by power iteration on the shifted 4x4 matrix).
glm::quat FitRotation(const std::vector<glm::vec3>& a, const std::vector<glm::vec3>& b)
{
    float S[3][3] = {};
    for (size_t i = 0; i < a.size(); ++i)
        for (int x = 0; x < 3; ++x)
            for (int y = 0; y < 3; ++y) S[x][y] += a[i][x] * b[i][y];
    const float Sxx = S[0][0], Sxy = S[0][1], Sxz = S[0][2], Syx = S[1][0], Syy = S[1][1], Syz = S[1][2], Szx = S[2][0], Szy = S[2][1], Szz = S[2][2];
    const float N[4][4] = { { Sxx + Syy + Szz, Syz - Szy, Szx - Sxz, Sxy - Syx },
                            { Syz - Szy, Sxx - Syy - Szz, Sxy + Syx, Szx + Sxz },
                            { Szx - Sxz, Sxy + Syx, -Sxx + Syy - Szz, Syz + Szy },
                            { Sxy - Syx, Szx + Sxz, Syz + Szy, -Sxx - Syy + Szz } };
    float shift = 0;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) shift += std::abs(N[r][c]);
    float v[4] = { 1, 0, 0, 0 };
    for (int it = 0; it < 200; ++it)
    {
        float w[4] = {};
        for (int r = 0; r < 4; ++r)
        {
            w[r] = shift * v[r];
            for (int c = 0; c < 4; ++c) w[r] += N[r][c] * v[c];
        }
        const float len = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2] + w[3] * w[3]);
        if (len < 1e-12f) return glm::quat(1, 0, 0, 0);
        for (int r = 0; r < 4; ++r) v[r] = w[r] / len;
    }
    return glm::normalize(glm::quat(v[0], v[1], v[2], v[3]));
}

glm::mat4 Compose(const glm::vec3& posedPivot, const glm::quat& rot, const glm::vec3& bindPivot)
{
    return glm::translate(glm::mat4(1.0f), posedPivot) * glm::mat4_cast(rot) * glm::translate(glm::mat4(1.0f), -bindPivot);
}

} // namespace

bool LoadRetargetRig(const fs::path& p3d, const fs::path& projectRoot, RetargetRig& out)
{
    out = {};
    out.model = p3d;
    P3DModel model;
    if (!LoadP3D(p3d, model) || model.lods.empty()) { out.info = "cannot read " + PathText(p3d); return false; }
    out.modelCfg = FindModelCfgUp(p3d, projectRoot);
    if (out.modelCfg.empty()) { out.info = "no model.cfg next to / above the model"; return false; }
    ArmaConfig cfg;
    cfg.Load(out.modelCfg, projectRoot);
    ModelCfgInfo info;
    if (!BuildSkeleton(cfg, PathText(p3d.stem()), out.skel, info)) { out.info = "no CfgModels / skeleton for " + PathText(p3d.stem()); return false; }
    P3DModel pivots;
    bool hasPivots = false;
    if (!out.skel.pivotsModel.empty())
    {
        fs::path pp = ResolveArmaPath(out.skel.pivotsModel, projectRoot);
        if (!pp.has_extension()) pp += ".p3d";
        std::error_code ec;
        hasPivots = fs::exists(pp, ec) && LoadP3D(pp, pivots);
    }
    ResolvePivots(out.skel, hasPivots ? &pivots : nullptr, &model.lods.front());
    int fromPivots = 0;
    for (const auto& b : out.skel.bones) fromPivots += b.pivotSource == PivotSource::PivotsModel ? 1 : 0;
    out.ok = true;
    out.info = std::format("{}: {} bones, {} joints from pivotsModel{}", out.skel.name, out.skel.bones.size(), fromPivots,
                           hasPivots ? "" : " (no pivotsModel: selection centroids)");
    return true;
}

BoneMapping AutoMapBones(const Skeleton& src, const Skeleton& tgt)
{
    BoneMapping m;
    m.srcOf.assign(tgt.bones.size(), -1);
    m.manual.assign(tgt.bones.size(), 0);
    for (size_t b = 0; b < tgt.bones.size(); ++b) m.srcOf[b] = src.Find(tgt.bones[b].name);
    // a name match is a pair only when the hierarchy matches too: the nearest mapped target ancestor must map to an
    // ancestor of the source bone (both roots: fine). Helper bones placed elsewhere (e.g. launcher / weapon as roots
    // in one skeleton, under the spine in the other) stay unmapped: bind pose under their parent.
    for (int b : tgt.ParentFirstOrder())
    {
        const int s = m.srcOf[size_t(b)];
        if (s < 0) continue;
        int tp = tgt.bones[size_t(b)].parent;
        while (tp >= 0 && m.srcOf[size_t(tp)] < 0) tp = tgt.bones[size_t(tp)].parent;
        const bool ok = tp >= 0 ? src.IsAncestor(m.srcOf[size_t(tp)], s) : src.bones[size_t(s)].parent < 0;
        if (!ok) m.srcOf[size_t(b)] = -1;
    }
    return m;
}

bool SaveMappingPreset(const fs::path& file, const Skeleton& src, const Skeleton& tgt, const BoneMapping& m)
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::trunc);
    if (!f) return false;
    f << "# ArmaView bone mapping: target (" << tgt.name << ") = source (" << src.name << ")\n";
    for (size_t b = 0; b < tgt.bones.size(); ++b)
        f << tgt.bones[b].name << " = " << (m.srcOf[b] >= 0 ? src.bones[m.srcOf[b]].name : std::string("-")) << "\n";
    return bool(f);
}

bool LoadMappingPreset(const fs::path& file, const Skeleton& src, const Skeleton& tgt, BoneMapping& m)
{
    std::ifstream f(file);
    if (!f) return false;
    m = AutoMapBones(src, tgt);
    for (std::string line; std::getline(f, line);)
    {
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            s.erase(0, s.find_first_not_of(" \t\r"));
            s.erase(s.find_last_not_of(" \t\r") + 1);
            return s;
        };
        const int t = tgt.Find(trim(line.substr(0, eq)));
        if (t < 0) continue;
        const std::string sname = trim(line.substr(eq + 1));
        m.srcOf[t] = sname == "-" ? -1 : src.Find(sname);
        m.manual[t] = 1;
    }
    return true;
}

RetargetResult RetargetAnimation(const RtmAnimation& src, const Skeleton& S, const Skeleton& T, const BoneMapping& map, const RetargetOptions& opt)
{
    RetargetResult r;
    const size_t nS = S.bones.size(), nT = T.bones.size(), nf = src.Frames();
    if (!nf || !nS || !nT) { r.report = "nothing to retarget"; return r; }
    // source bone -> animation bone
    std::vector<int> srcAnim(nS, -1);
    for (size_t b = 0; b < nS; ++b)
        for (size_t k = 0; k < src.info.bones.size(); ++k)
            if (Lower(src.info.bones[k]) == Lower(S.bones[b].name)) { srcAnim[b] = int(k); break; }
    const auto orderS = S.ParentFirstOrder(), orderT = T.ParentFirstOrder();
    for (int t : map.srcOf) r.mapped += t >= 0 ? 1 : 0;

    // bind-pose correction per target bone: target bind direction -> source bind direction (via a mapped child)
    std::vector<glm::quat> corr(nT, glm::quat(1, 0, 0, 0));
    std::vector<uint8_t> hasCorr(nT, 0);
    for (int b : orderT)
    {
        const int s = map.srcOf[b];
        if (s < 0) continue;
        // per child branch: the nearest mapped descendant whose joint is away from this one in both skeletons (roll
        // bones often sit on the joint itself: zero length, no direction)
        std::vector<glm::vec3> jT, jS;
        std::vector<std::string> jName;
        for (int c0 : T.bones[b].children)
        {
            std::vector<int> queue{ c0 };
            for (size_t qi = 0; qi < queue.size() && queue.size() < 64; ++qi)
            {
                const int c = queue[qi];
                const int sc = map.srcOf[c];
                if (sc >= 0 && glm::length(T.bones[c].pivot - T.bones[b].pivot) > 0.02f && glm::length(S.bones[sc].pivot - S.bones[s].pivot) > 0.02f)
                {
                    jT.push_back(T.bones[c].pivot);
                    jS.push_back(S.bones[sc].pivot);
                    jName.push_back(T.bones[c].name);
                    break;
                }
                for (int g : T.bones[c].children) queue.push_back(g);
            }
        }
        // a root (pelvis) with several branches: fit them all (spine + both legs); one direction (to a hip) rolls it by
        // the hip width difference. Other bones keep the direction of their first branch (the chain they continue).
        if (jT.size() > 1 && T.bones[b].parent >= 0) { jT.resize(1); jS.resize(1); jName.resize(1); }
        if (jT.size() == 1)
        {
            corr[b] = BindCorrection(jT[0] - T.bones[b].pivot, jS[0] - S.bones[s].pivot);
            hasCorr[b] = 1;
        }
        else if (jT.size() > 1)
        {
            // fit all branches around their centre
            // a branch that does not fit (a helper bone with its joint elsewhere in one rig) can flip the whole fit: try
            // all branches and every set with one left out, keep the fit with the smallest worst residual
            auto fit = [&](int skip, float& worstAngle) {
                glm::vec3 cT(0), cS(0);
                int n = 0;
                for (size_t i = 0; i < jT.size(); ++i)
                    if (int(i) != skip) { cT += jT[i]; cS += jS[i]; ++n; }
                cT /= float(n);
                cS /= float(n);
                std::vector<glm::vec3> a, bb;
                auto add = [&](glm::vec3 x, glm::vec3 y) {
                    if (glm::length(x) > 1e-3f && glm::length(y) > 1e-3f) { a.push_back(glm::normalize(x)); bb.push_back(glm::normalize(y)); }
                };
                // (no joint -> centre direction: a root's pivot is often a placeholder - on the ground in one rig, at the
                // origin in the other - and that direction flips)
                for (size_t i = 0; i < jT.size(); ++i)
                    if (int(i) != skip) add(jT[i] - cT, jS[i] - cS);
                const glm::quat q = FitRotation(a, bb);
                float minDot = 1;
                for (size_t i = 0; i < a.size(); ++i) minDot = std::min(minDot, glm::dot(q * a[i], bb[i]));
                worstAngle = glm::degrees(std::acos(std::clamp(minDot, -1.0f, 1.0f)));
                return q;
            };
            float best = 0;
            corr[b] = fit(-1, best);
            int skipped = -1;
            if (best > 30.0f && jT.size() > 2)
                for (int k = 0; k < int(jT.size()); ++k)
                {
                    float w = 0;
                    const glm::quat q = fit(k, w);
                    if (w < best) { best = w; corr[b] = q; skipped = k; }
                }
            if (skipped >= 0) r.notes += std::format("; {}: branch {} ignored in the bind fit", T.bones[b].name, jName[size_t(skipped)]);
            hasCorr[b] = 1;
        }
        if (!hasCorr[b] && T.bones[b].parent >= 0) corr[b] = corr[T.bones[b].parent]; // end bones: like the parent
    }
    // pelvis height ratio (leg length)
    int pelS = S.Find("Pelvis"), pelT = T.Find("Pelvis");
    if (pelS < 0 || pelT < 0)
        for (int b : orderT)
            if (T.bones[b].parent < 0 && map.srcOf[b] >= 0) { pelT = b; pelS = map.srcOf[b]; break; }
    // leg length: thigh + shin (UpLeg -> Leg -> Foot joints), average of both sides
    auto legLength = [](const Skeleton& k) {
        float sum = 0;
        int n = 0;
        for (const char* side : { "Left", "Right" })
        {
            const int u = k.Find(std::string(side) + "UpLeg"), l = k.Find(std::string(side) + "Leg"), f = k.Find(std::string(side) + "Foot");
            if (u < 0 || l < 0 || f < 0) continue;
            sum += glm::length(k.bones[l].pivot - k.bones[u].pivot) + glm::length(k.bones[f].pivot - k.bones[l].pivot);
            ++n;
        }
        return n ? sum / n : 0.0f;
    };
    const float legS = legLength(S), legT = legLength(T);
    if (opt.scalePelvis && legS > 0.05f && legT > 0.05f) r.scale = legT / legS;
    else if (opt.scalePelvis && pelS >= 0 && pelT >= 0 && S.bones[pelS].pivot.y > 0.05f) r.scale = T.bones[pelT].pivot.y / S.bones[pelS].pivot.y;
    r.legInfo = std::format("legs {:.3f} m -> {:.3f} m, pelvis height {:.3f} m -> {:.3f} m", legS, legT, pelS >= 0 ? S.bones[pelS].pivot.y : 0.0f,
                            pelT >= 0 ? T.bones[pelT].pivot.y : 0.0f);

    auto out = std::make_shared<RtmAnimation>();
    out->path = src.path;
    out->info.signature = "RTM_0101";
    out->info.move = src.info.move * r.scale;
    out->info.frames = uint32_t(nf);
    for (const auto& b : T.bones) out->info.bones.push_back(b.name);
    out->times = src.times;
    out->poses.resize(nf * nT);
    r.sourceJoints.resize(nf);

    std::vector<glm::mat4> Ms(nS), Mt(nT);
    std::vector<glm::quat> Rs(nS), Rt(nT);
    std::vector<glm::vec3> Ps(nS);
    // feet chains for the IK option
    struct Leg { int up = -1, knee = -1, foot = -1; } legs[2];
    for (int side = 0; side < 2; ++side)
    {
        const char* pre = side ? "Right" : "Left";
        legs[side] = { T.Find(std::string(pre) + "UpLeg"), T.Find(std::string(pre) + "Leg"), T.Find(std::string(pre) + "Foot") };
    }
    auto fk = [&](glm::vec3 rootOffset) {
        for (int b : orderT)
        {
            const int p = T.bones[b].parent;
            // unmapped roots (helper bones without a pair) stay in the bind pose
            const glm::vec3 posed = p < 0 ? T.bones[b].pivot + (map.srcOf[b] >= 0 || b == pelT ? rootOffset : glm::vec3(0))
                                          : glm::vec3(Mt[p] * glm::vec4(T.bones[b].pivot, 1.0f));
            Mt[b] = Compose(posed, Rt[b], T.bones[b].pivot);
        }
    };
    float maxIkMiss = 0, maxDrop = 0;
    for (size_t f = 0; f < nf; ++f)
    {
        // source pose (model space): bones without data follow the parent
        for (int b : orderS)
        {
            const int k = srcAnim[b], p = S.bones[b].parent;
            Ms[b] = k >= 0 ? src.poses[f * src.Bones() + size_t(k)].Matrix() : p >= 0 ? Ms[p] : glm::mat4(1.0f);
            Rs[b] = BonePose::FromMatrix(Ms[b]).rot;
            Ps[b] = glm::vec3(Ms[b] * glm::vec4(S.bones[b].pivot, 1.0f));
        }
        r.sourceJoints[f] = Ps;
        // target rotations
        for (int b : orderT)
        {
            const int s = map.srcOf[b], p = T.bones[b].parent;
            Rt[b] = s >= 0 ? glm::normalize(Rs[s] * corr[b]) : p >= 0 ? Rt[p] : glm::quat(1, 0, 0, 0);
        }
        // root translation: source pelvis motion, scaled
        glm::vec3 rootOffset(0);
        if (pelS >= 0 && pelT >= 0) rootOffset = (Ps[pelS] - S.bones[pelS].pivot) * r.scale;
        // a root that is not the pelvis: place the pelvis by moving the root
        fk(rootOffset);
        if (pelT >= 0 && T.bones[pelT].parent >= 0)
        {
            const glm::vec3 want = T.bones[pelT].pivot + rootOffset, have = glm::vec3(Mt[pelT] * glm::vec4(T.bones[pelT].pivot, 1.0f));
            rootOffset += want - have;
            fk(rootOffset);
        }
        // ground: the lower target foot at the height of the lower source foot (different hip / leg proportions)
        if (opt.scalePelvis)
        {
            float srcLow = 1e30f, tgtLow = 1e30f;
            for (const auto& L : legs)
                if (L.foot >= 0 && map.srcOf[L.foot] >= 0)
                {
                    srcLow = std::min(srcLow, Ps[map.srcOf[L.foot]].y);
                    tgtLow = std::min(tgtLow, glm::vec3(Mt[L.foot] * glm::vec4(T.bones[L.foot].pivot, 1)).y);
                }
            if (srcLow < 1e29f && tgtLow < 1e29f)
            {
                rootOffset.y += srcLow - tgtLow;
                fk(rootOffset);
            }
        }
        // pelvis drop: a foot goal farther than the leg reaches (straight legs, longer stride after scaling) - lower the
        // root just enough that every goal is within 99.5 % of the leg length; the IK then reaches it exactly
        auto footGoal = [&](const Leg& L) {
            glm::vec3 g = Ps[map.srcOf[L.foot]] * r.scale; // ground-relative: scaled horizontally like the body
            g.y = Ps[map.srcOf[L.foot]].y;                 // height of the source foot over the ground as is
            return g;
        };
        if (opt.footIK)
        {
            float drop = 0;
            for (const auto& L : legs)
            {
                if (L.up < 0 || L.knee < 0 || L.foot < 0 || map.srcOf[L.foot] < 0) continue;
                const glm::vec3 H = glm::vec3(Mt[L.up] * glm::vec4(T.bones[L.up].pivot, 1)), goal = footGoal(L);
                const float reach = 0.995f * (glm::length(T.bones[L.knee].pivot - T.bones[L.up].pivot) + glm::length(T.bones[L.foot].pivot - T.bones[L.knee].pivot));
                const float h = glm::length(glm::vec2(goal.x - H.x, goal.z - H.z));
                if (h >= reach) continue; // horizontally out of reach: lowering does not help
                const float need = (H.y - goal.y) - std::sqrt(reach * reach - h * h);
                drop = std::max(drop, need);
            }
            if (drop > 0)
            {
                rootOffset.y -= drop;
                fk(rootOffset);
                maxDrop = std::max(maxDrop, drop);
            }
        }
        if (opt.footIK)
            for (const auto& L : legs)
            {
                if (L.up < 0 || L.knee < 0 || L.foot < 0 || map.srcOf[L.foot] < 0) continue;
                const glm::vec3 goal = footGoal(L);
                const glm::vec3 H = glm::vec3(Mt[L.up] * glm::vec4(T.bones[L.up].pivot, 1)), K = glm::vec3(Mt[L.knee] * glm::vec4(T.bones[L.knee].pivot, 1)),
                                F = glm::vec3(Mt[L.foot] * glm::vec4(T.bones[L.foot].pivot, 1));
                const float a = glm::length(T.bones[L.knee].pivot - T.bones[L.up].pivot), b = glm::length(T.bones[L.foot].pivot - T.bones[L.knee].pivot);
                const float d = std::clamp(glm::length(goal - H), std::abs(a - b) + 1e-4f, a + b - 1e-4f);
                const glm::vec3 dir = glm::normalize(goal - H);
                glm::vec3 n = glm::cross(F - H, K - H);
                if (glm::length(n) < 1e-6f) n = glm::vec3(1, 0, 0);
                n = glm::normalize(n);
                glm::vec3 u = glm::normalize(glm::cross(n, dir));
                if (glm::dot(u, K - H) < 0) u = -u;
                const float cosA = std::clamp((a * a + d * d - b * b) / (2 * a * d), -1.0f, 1.0f);
                const glm::vec3 K2 = H + a * (cosA * dir + std::sqrt(1 - cosA * cosA) * u);
                const glm::quat dUp = FromTo(K - H, K2 - H);
                const glm::vec3 F1 = K2 + dUp * (F - K);
                const glm::quat dKnee = FromTo(F1 - K2, H + dir * d - K2);
                // rotate the thigh / shin subtrees, the foot subtree keeps its world rotation
                for (size_t x = 0; x < nT; ++x)
                {
                    if (T.IsAncestor(L.foot, int(x)) || int(x) == L.foot) continue;
                    if (int(x) == L.knee || T.IsAncestor(L.knee, int(x))) Rt[x] = glm::normalize(dKnee * dUp * Rt[x]);
                    else if (int(x) == L.up || T.IsAncestor(L.up, int(x))) Rt[x] = glm::normalize(dUp * Rt[x]);
                }
                fk(rootOffset);
                const glm::vec3 F2 = glm::vec3(Mt[L.foot] * glm::vec4(T.bones[L.foot].pivot, 1));
                maxIkMiss = std::max(maxIkMiss, glm::length(F2 - goal));
            }
        for (size_t b = 0; b < nT; ++b) out->poses[f * nT + b] = BonePose::FromMatrix(Mt[b]);
        if (f == 0)
            for (const char* bn : { "Pelvis", "Head", "LeftFoot", "RightHand" })
            {
                const int bs = S.Find(bn), bt = T.Find(bn);
                if (bs >= 0 && bt >= 0)
                {
                    const glm::vec3 pt = glm::vec3(Mt[bt] * glm::vec4(T.bones[bt].pivot, 1));
                    r.debug += std::format(" {} src ({:.2f} {:.2f} {:.2f}) bind ({:.2f} {:.2f} {:.2f}) anim {} | tgt ({:.2f} {:.2f} {:.2f});", bn, Ps[bs].x, Ps[bs].y,
                                           Ps[bs].z, S.bones[bs].pivot.x, S.bones[bs].pivot.y, S.bones[bs].pivot.z, srcAnim[bs], pt.x, pt.y, pt.z);
                }
            }
    }
    r.anim = out;
    r.report = std::format("{} frames, {} of {} target bones mapped, pelvis motion x {:.3f} ({}){}", nf, r.mapped, nT, r.scale, r.legInfo,
                           opt.footIK ? std::format(", foot IK miss max {:.3f} m, pelvis lowered up to {:.3f} m to reach", maxIkMiss, maxDrop) : "") + r.notes;
    return r;
}

bool WriteRtm0101(const RtmAnimation& a, const fs::path& file, std::string& error)
{
    std::error_code ec;
    if (fs::exists(file, ec)) { error = "file exists - not overwritten: " + PathText(file); return false; }
    fs::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) { error = "cannot create " + PathText(file); return false; }
    const uint32_t nf = uint32_t(a.Frames()), nb = uint32_t(a.info.bones.size());
    f.write("RTM_0101", 8);
    f.write(reinterpret_cast<const char*>(&a.info.move), 12);
    f.write(reinterpret_cast<const char*>(&nf), 4);
    f.write(reinterpret_cast<const char*>(&nb), 4);
    auto name32 = [&](const std::string& n) {
        char buf[32] = {};
        std::memcpy(buf, n.data(), std::min<size_t>(31, n.size()));
        f.write(buf, 32);
    };
    for (const auto& n : a.info.bones) name32(n);
    for (uint32_t fr = 0; fr < nf; ++fr)
    {
        f.write(reinterpret_cast<const char*>(&a.times[fr]), 4);
        for (uint32_t b = 0; b < nb; ++b)
        {
            name32(a.info.bones[b]);
            const glm::mat4 m = a.poses[size_t(fr) * nb + b].Matrix();
            const float v[12] = { m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2], m[3][0], m[3][1], m[3][2] };
            f.write(reinterpret_cast<const char*>(v), 48);
        }
    }
    if (!f) { error = "write failed: " + PathText(file); return false; }
    return true;
}

void CompareAnimations(const RtmAnimation& a, const RtmAnimation& b, float& maxPos, float& maxElem, std::string& note)
{
    maxPos = maxElem = 0;
    note.clear();
    if (a.Frames() != b.Frames() || a.info.bones.size() != b.info.bones.size())
    {
        note = std::format("shape differs: {} x {} vs {} x {}", a.Frames(), a.info.bones.size(), b.Frames(), b.info.bones.size());
        maxPos = maxElem = 1e9f;
        return;
    }
    const size_t nb = a.info.bones.size();
    for (size_t f = 0; f < a.Frames(); ++f)
        for (size_t k = 0; k < nb; ++k)
        {
            const glm::mat4 x = a.poses[f * nb + k].Matrix(), y = b.poses[f * nb + k].Matrix();
            maxPos = std::max(maxPos, glm::length(glm::vec3(x[3]) - glm::vec3(y[3])));
            for (int c = 0; c < 3; ++c)
                for (int r = 0; r < 3; ++r) maxElem = std::max(maxElem, std::abs(x[c][r] - y[c][r]));
        }
}
