#include "SceneView.h"
#include "Lang.h"
#include "Log.h"
#include "Perf.h"
#include "Text.h"

#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace fs = std::filesystem;

namespace {
glm::vec4 WeightColor(float w) // 0 blue -> 0.5 green -> 1 red
{
    const glm::vec3 lo(0.1f, 0.3f, 1.0f), mid(0.1f, 0.9f, 0.2f), hi(1.0f, 0.15f, 0.1f);
    glm::vec3 c = w < 0.5f ? glm::mix(lo, mid, w * 2.0f) : glm::mix(mid, hi, (w - 0.5f) * 2.0f);
    return glm::vec4(c, 1.0f);
}
} // namespace

void SceneView::RebuildLodGeometry()
{
    baseVerts.clear();
    vertPoint.clear();
    normalBase.clear();
    meshSections.clear();
    skin = {};
    if (!s_.hasModel || s_.lod < 0 || s_.lod >= int(s_.model.lods.size()))
    {
        renderer_.SetSections({});
        UpdatePose();
        return;
    }
    const P3DLod& l = s_.model.lods[s_.lod];

    // Per-point highlight colour (alpha 0 = untinted).
    std::vector<glm::vec4> pointColor(l.points.size(), glm::vec4(0.0f));
    if (highlight == Highlight::Selection && selection >= 0 && selection < int(l.selections.size()))
    {
        const P3DSelection& s = l.selections[selection];
        for (size_t p = 0; p < l.points.size(); ++p)
            if (float w = s.Weight(p); w > 0.0f) pointColor[p] = WeightColor(w);
    }
    else if (maskHeat && (!maskView.empty() || !customHeat.empty()) && !s_.skeleton.Empty())
    {
        // mask weight of each point = sum over its bones (selection weight x mask weight)
        const std::vector<float> mw = !customHeat.empty() ? customHeat : gesturePlayer_.MaskWeights(maskView);
        std::vector<float> heat(l.points.size(), 0.0f);
        for (size_t b = 0; b < mw.size() && b < boneSelection.size(); ++b)
            if (mw[b] > 0 && boneSelection[b] >= 0)
            {
                const P3DSelection& s = l.selections[boneSelection[b]];
                for (size_t p = 0; p < l.points.size(); ++p)
                    if (float w = s.Weight(p); w > 0) heat[p] += w * mw[b];
            }
        for (size_t p = 0; p < l.points.size(); ++p) pointColor[p] = WeightColor(std::min(heat[p], 1.0f));
    }
    else if (validation.valid)
    {
        const glm::vec4 red(1.0f, 0.12f, 0.1f, 1.0f);
        if (highlight == Highlight::TooManyBones) for (auto& v : validation.tooManyBones) pointColor[v.point] = red;
        if (highlight == Highlight::BadWeightSum) for (auto& v : validation.badWeightSum) pointColor[v.point] = red;
        if (highlight == Highlight::NoBone) for (auto p : validation.noBone) pointColor[p] = red;
    }

    // Proxy triangles (faces of "proxy:..." selections) are placement markers, not geometry; Buldozer hides them.
    std::vector<uint8_t> proxyFace(l.faces.size(), 0);
    for (const auto& s : l.selections)
        if (s.name.starts_with("proxy:"))
            for (size_t i = 0; i < s.faceBytes.size() && i < proxyFace.size(); ++i)
                if (s.faceBytes[i]) proxyFace[i] = 1;

    // viewer hide / isolate
    faceHidden = vis_.HiddenFaces(l, l.resolution);
    faceInfo = AnalyzeFaces(l);
    auto hashColor = [](const std::string& s) {
        uint32_t h = 2166136261u;
        for (char c : s) h = (h ^ uint8_t(std::tolower(uint8_t(c)))) * 16777619u;
        ImVec4 c(0, 0, 0, 1);
        ImGui::ColorConvertHSVtoRGB(float(h % 1000) / 1000.0f, 0.65f, 0.95f, c.x, c.y, c.z);
        return glm::vec4(c.x, c.y, c.z, 1.0f);
    };
    std::vector<uint8_t> picked(l.faces.size(), 0);
    for (uint32_t f : pickFaces)
        if (f < picked.size()) picked[f] = 1;
    {
        const auto& slots = s_.attach.Slots();
        s_.attach.hiddenSlot.assign(slots.size(), 0);
        for (size_t i = 0; i < slots.size(); ++i)
            for (const auto& s : l.selections)
                if (IEquals(s.name, slots[i].selection))
                {
                    bool all = true, any = false;
                    for (size_t f = 0; f < s.faceBytes.size() && f < faceHidden.size(); ++f)
                        if (s.faceBytes[f]) { any = true; all = all && faceHidden[f]; }
                    s_.attach.hiddenSlot[i] = any && all;
                    break;
                }
    }

    const bool recompute = display_.normalsMode == NormalsMode::Recomputed;
    std::vector<glm::vec3> cornerNormals;
    if (recompute) cornerNormals = ComputeCornerNormals(l, true);
    const glm::vec4 lineColor = recompute ? glm::vec4(1.0f, 0.6f, 0.2f, 1.0f) : glm::vec4(0.2f, 0.9f, 1.0f, 1.0f);

    // Second UV set (#UVSet# stage 1), indexed by face corner.
    const P3DUVSet* uv1 = nullptr;
    for (const auto& s : l.uvSets)
        if (s.stage == 1) uv1 = &s;
    std::vector<size_t> cornerBase(l.faces.size() + 1, 0);
    for (size_t f = 0; f < l.faces.size(); ++f) cornerBase[f + 1] = cornerBase[f] + l.faces[f].count;

    // Config cfgSkin (hiddenSelections): per-face texture/material override; later selections win.
    std::vector<int> faceSkin(l.faces.size(), -1);
    const ConfigSkin* cfgSkin = s_.activeSkin >= 0 && s_.activeSkin < int(s_.configSkins.size()) ? &s_.configSkins[s_.activeSkin] : nullptr;
    if (cfgSkin)
        for (int i = 0; i < int(cfgSkin->selections.size()); ++i)
            for (const auto& s : l.selections)
                if (!s.engineIgnored && IEquals(s.name, cfgSkin->selections[i]))
                    for (size_t f = 0; f < s.faceBytes.size() && f < faceSkin.size(); ++f)
                        if (s.faceBytes[f]) faceSkin[f] = i;

    // Group faces by (texture, material) -> one draw section each.
    std::map<std::pair<std::string, std::string>, int> sectionOf;
    std::vector<std::vector<MeshVertex>> secVerts;
    std::vector<std::vector<uint32_t>> secPoints;

    restMin = glm::vec3(1e30f);
    restMax = glm::vec3(-1e30f);
    normalBase.reserve(l.faces.size() * 4);
    static const int tri[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    for (size_t fi = 0; fi < l.faces.size(); ++fi)
    {
        const P3DFace& f = l.faces[fi];
        bool valid = true;
        for (uint32_t c = 0; c < f.count; ++c)
            if (f.v[c].point >= l.points.size() || (!recompute && f.v[c].normal >= l.normals.size())) valid = false;
        if (!valid || (proxyFace[fi] && !display_.showProxies) || faceHidden[fi]) continue;

        std::string tex = f.texture, mat = f.material;
        if (int hs = faceSkin[fi]; hs >= 0)
        {
            if (hs < int(cfgSkin->textures.size()) && !cfgSkin->textures[hs].empty()) tex = cfgSkin->textures[hs];
            if (hs < int(cfgSkin->materials.size()) && !cfgSkin->materials[hs].empty()) mat = cfgSkin->materials[hs];
        }
        if (h_.DamageState() > 0)
        {
            if (auto it = damageMat.find(ToLower(mat)); it != damageMat.end()) mat = h_.DamageState() == 1 ? it->second.first : it->second.second;
            if (auto it = damageTex.find(ToLower(tex)); it != damageTex.end()) tex = it->second;
        }
        auto key = std::make_pair(ToLower(tex), ToLower(mat));
        auto [it, inserted] = sectionOf.emplace(key, int(meshSections.size()));
        if (inserted)
        {
            meshSections.push_back({ tex, mat, 0, 0, 0 });
            secVerts.emplace_back();
            secPoints.emplace_back();
        }
        const int si = it->second;
        ++meshSections[si].faces;

        auto normalOf = [&](uint32_t c) { return recompute ? cornerNormals[fi * 4 + c] : l.normals[f.v[c].normal]; };
        for (uint32_t c = 0; c < f.count; ++c)
        {
            restMin = glm::min(restMin, l.points[f.v[c].point].pos);
            restMax = glm::max(restMax, l.points[f.v[c].point].pos);
            normalBase.push_back({ f.v[c].point, normalOf(c), lineColor });
        }
        for (int t = 0; t < int(f.count) - 2; ++t)
            for (int k : tri[t])
            {
                const P3DFaceVertex& fv = f.v[k];
                MeshVertex v;
                v.pos = l.points[fv.point].pos;
                v.normal = normalOf(k);
                v.uv = { fv.u, fv.v };
                size_t corner = cornerBase[fi] + k;
                v.uv1 = uv1 && corner < uv1->uv.size() ? uv1->uv[corner] : v.uv;
                v.color = picked[fi] ? glm::vec4(1.0f, 0.55f, 0.1f, 1.0f) : pointColor[fv.point];
                switch (display_.faceDiag)
                {
                case FaceDiag::MirroredUV:
                    v.color = faceInfo.degenerateUV[fi] ? glm::vec4(1, 0.9f, 0.1f, 1) : faceInfo.mirrored[fi] ? glm::vec4(1, 0.15f, 0.9f, 1) : glm::vec4(0.25f, 0.8f, 0.35f, 1);
                    break;
                case FaceDiag::FlippedNormals:
                    v.color = faceInfo.flippedCorners[fi] == 0 ? glm::vec4(0.25f, 0.8f, 0.35f, 1)
                            : faceInfo.flippedCorners[fi] >= f.count ? glm::vec4(1, 0.1f, 0.1f, 1) : glm::vec4(1, 0.6f, 0.1f, 1);
                    break;
                case FaceDiag::RvmatPerFace: v.color = mat.empty() ? glm::vec4(0.5f, 0.5f, 0.5f, 1) : hashColor(mat); break;
                case FaceDiag::TexturePerFace: v.color = tex.empty() ? glm::vec4(0.5f, 0.5f, 0.5f, 1) : hashColor(tex); break;
                default: break;
                }
                secVerts[si].push_back(v);
                secPoints[si].push_back(fv.point);
            }
    }
    size_t total = 0;
    for (auto& v : secVerts) total += v.size();
    baseVerts.reserve(total);
    vertPoint.reserve(total);
    for (size_t s = 0; s < meshSections.size(); ++s)
    {
        meshSections[s].first = UINT(baseVerts.size());
        meshSections[s].count = UINT(secVerts[s].size());
        baseVerts.insert(baseVerts.end(), secVerts[s].begin(), secVerts[s].end());
        vertPoint.insert(vertPoint.end(), secPoints[s].begin(), secPoints[s].end());
    }
    // proxies + crew after the vehicle (their vertices are rewritten from the proxy transforms in UpdatePose)
    attachFirst = baseVerts.size();
    vehicleSections = meshSections.size();
    {
        std::vector<Attachments::Section> extra;
        s_.attach.AppendGeometry(baseVerts, extra);
        for (const auto& e : extra) meshSections.push_back({ e.texture, e.material, 0, e.first, e.count });
        vertPoint.resize(baseVerts.size(), uint32_t(l.points.size())); // attachments: the identity matrix after the points
    }
    if (restMin.x > restMax.x) // no drawable faces (e.g. Memory, Land Contact): use all points
    {
        for (const auto& p : l.points)
        {
            restMin = glm::min(restMin, p.pos);
            restMax = glm::max(restMax, p.pos);
        }
        if (l.points.empty()) restMin = restMax = glm::vec3(0);
    }
    h_.SectionsRebuilt();
    BuildDrawSections();
    {
        PerfNoteScope ps("load:     skin + GPU mesh + pose");
        if (!s_.skeleton.Empty()) skin = BuildSkin(s_.skeleton, l, animView_.skinNormalize, animView_.skinLimit4 ? 4 : 0);
        renderer_.SetMesh(baseVerts);       // rest pose; skinning happens on the GPU from the point matrices
        renderer_.SetVertexPoints(vertPoint);
        UpdatePose();
    }
}

const RvMaterial* SceneView::EffectiveRvMaterial(const std::string& name)
{
    const RvMaterial* orig = mats_.Get(name);
    if (!orig || mats_.showOriginal) return orig;
    return mats_.Current(name, orig);
}

void SceneView::BuildDrawSections()
{
    {
        // decode every texture this LOD needs in parallel (Get below is then a lookup)
        std::vector<std::string> need;
        {
            PerfNoteScope ps("load:     RVMATs (read + parse)");
            std::vector<std::string> mats;
            for (const auto& ms : meshSections) mats.push_back(ms.material);
            mats_.Prefetch(mats);
            for (const auto& ms : meshSections)
            {
                if (!ms.texture.empty()) need.push_back(ms.texture);
                if (const RvMaterial* rv = EffectiveRvMaterial(ms.material); rv && rv->loaded)
                    for (const auto& st : rv->stages)
                        if (!st.texture.empty() && st.role != TexRole::Count) need.push_back(st.texture);
            }
        }
        PerfNoteScope ps("load:     textures (queued for streaming / decoded)");
        if (streamTextures) texCache_.PrefetchAsync(need); // geometry first; textures arrive over the next frames
        else texCache_.Prefetch(need);
    }
    std::vector<DrawSection> out;
    for (size_t msi = 0; msi < meshSections.size(); ++msi)
    {
        const MeshSectionInfo& ms = meshSections[msi];
        if (h_.HideVehicle() && msi < vehicleSections) continue;
        DrawSection ds;
        ds.first = ms.first;
        ds.count = ms.count;
        for (int r = 0; r < kMaterialSlots; ++r) ds.srv[r] = texCache_.Default(TexRole(r));
        MaterialConstants& mc = ds.material;
        const RvMaterial* rv = EffectiveRvMaterial(ms.material);
        if (rv && rv->loaded && (rv->hasFresnel || rv->hasEnvMap))
            mc.env = glm::vec4(1.0f, rv->hasFresnel ? rv->fresnelN : 1.5f, rv->hasFresnel ? rv->fresnelK : 0.0f, 0.0f);
        std::string diffuse = ms.texture;
        if (rv && rv->loaded)
        {
            mc.ambient = rv->ambient;
            mc.diffuse = rv->diffuse;
            mc.forcedDiffuse = rv->forcedDiffuse;
            mc.emissive = rv->emissive;
            mc.specular = rv->specular;
            mc.params.x = rv->specularPower;
            mc.skin.x = IEquals(rv->pixelShader, "Skin") ? 1.0f : 0.0f;
            bool used[kMaterialSlots] = {};
            const bool multi = IEquals(rv->pixelShader, "Multi");
            if (multi)
            {
                // Multi stages: 0-3 layer _co, 4 mask, 5-8 layer _dtsmdi, 9 macro, 10 _adshq, 11-14 layer _nohq
                mc.multi.x = 1.0f;
                bool normalSwz = false;
                for (const auto& st : rv->stages)
                {
                    if (st.texture.empty()) continue;
                    const int k = st.index;
                    const int m = k <= 8 ? k : k >= 11 && k <= 14 ? k - 2 : -1;
                    const TextureEntry& te = texCache_.Get(st.texture);
                    if (m >= 0)
                    {
                        ds.multi[m] = te.srv.Get();
                        mc.muvA[m] = { st.uv.aside.x, st.uv.aside.y, st.uv.up.x, st.uv.up.y };
                        mc.muvB[m] = { st.uv.pos.x, st.uv.pos.y, float(st.uv.source), 0 };
                        if (m >= 9 && !normalSwz && display_.applySwizzle)
                        {
                            const Unswizzle u = UnswizzleOf(te.swizzle);
                            for (int r = 0; r < 4; ++r)
                                for (int c = 0; c < 4; ++c) mc.nmSwizzle[c][r] = u.m[r][c];
                            mc.nmSwizzleOffset = { u.offset[0], u.offset[1], u.offset[2], u.offset[3] };
                            normalSwz = true;
                        }
                    }
                    else if (k == 9 || k == 10)
                    {
                        const int slot = k == 9 ? int(TexRole::Macro) : int(TexRole::AmbientShadow);
                        ds.srv[slot] = te.srv.Get();
                        used[slot] = true;
                        mc.uvA[slot] = { st.uv.aside.x, st.uv.aside.y, st.uv.up.x, st.uv.up.y };
                        mc.uvB[slot] = { st.uv.pos.x, st.uv.pos.y, float(st.uv.source), 0 };
                    }
                }
                for (int m = 0; m < kMultiSlots; ++m)
                    if (!ds.multi[m]) ds.multi[m] = texCache_.Default(m == 4 ? TexRole::Macro : m >= 9 ? TexRole::Normal : m >= 5 ? TexRole::Specular : TexRole::Diffuse);
            }
            for (const auto& st : rv->stages)
            {
                if (multi) break;
                if (st.role == TexRole::Count || st.texture.empty()) continue;
                int slot = int(st.role);
                if (used[slot]) continue;
                used[slot] = true;
                if (st.role == TexRole::Diffuse)
                {
                    if (diffuse.empty()) diffuse = st.texture;
                }
                else
                {
                    const TextureEntry& te = texCache_.Get(st.texture);
                    ds.srv[slot] = te.srv.Get();
                    if (st.role == TexRole::Normal && display_.applySwizzle)
                    {
                        const Unswizzle u = UnswizzleOf(te.swizzle);
                        // HLSL float4x4 from a row-major C array: cbuffer matrices are column-major -> transpose
                        for (int r = 0; r < 4; ++r)
                            for (int c = 0; c < 4; ++c) mc.nmSwizzle[c][r] = u.m[r][c];
                        mc.nmSwizzleOffset = { u.offset[0], u.offset[1], u.offset[2], u.offset[3] };
                    }
                }
                mc.uvA[slot] = { st.uv.aside.x, st.uv.aside.y, st.uv.up.x, st.uv.up.y };
                mc.uvB[slot] = { st.uv.pos.x, st.uv.pos.y, float(st.uv.source), 0 };
            }
        }
        else // no rvmat: Arma's default material is plain diffuse with a little ambient
        {
            mc.specular = glm::vec4(0.0f);
        }
        if (diffuse.empty()) ds.srv[0] = texCache_.Untextured();
        else
        {
            const TextureEntry& te = texCache_.Get(diffuse);
            ds.srv[0] = te.srv.Get();
            std::string lt = ToLower(diffuse);
            bool alpha = false;
            if (lt.starts_with("#"))
            {
                Image img;
                std::string err;
                alpha = MakeProceduralTexture(diffuse, img, err) && img.mips[0][3] < 255;
            }
            else
            {
                std::string stem = PathToUtf8(ArmaStringPath(lt).stem());
                alpha = stem.ends_with("_ca") || stem.ends_with("_lca");
            }
            if (alpha)
            {
                bool glass = ToLower(ms.material).find("glass") != std::string::npos || lt.find("glass") != std::string::npos ||
                             lt.starts_with("#");
                mc.params.y = glass ? 2.0f : 1.0f;
            }
        }
        out.push_back(ds);
    }
    if (out.empty()) out.push_back({}); // nothing visible (e.g. vehicle hidden): an empty section, not the whole mesh
    renderer_.SetSections(std::move(out));
    host_.WatchedFilesChanged(); // textures / rvmats may have been loaded
}

void SceneView::UpdatePose()
{
    const bool rtm = s_.anim.Active();
    const bool cfgAnim = !s_.cfgAnims.empty();
    const bool animate = s_.hasModel && !playback_.restPose && (rtm || cfgAnim) && !s_.skeleton.Empty() && !skin.offset.empty();
    if (animate)
    {
        // RTM pose (or rest), then model.cfg animations in rest space: M = RTM * cfg
        {
            Perf::Scope sc("pose: evaluate RTM + model.cfg");
            if (rtm) s_.anim.Evaluate(s_.skeleton, playback_.time, boneMats);
            else boneMats.assign(s_.skeleton.bones.size(), glm::mat4(1.0f));
            gesturePlayer_.ApplyAim(boneMats);
            h_.ApplyRootMotion(boneMats);
            if (cfgAnim)
            {
                std::vector<glm::mat4> cfgMats;
                EvaluateCfgAnimations(s_.skeleton, s_.cfgAnims, s_.cfgSources, cfgMats);
                for (size_t b = 0; b < boneMats.size(); ++b) boneMats[b] = boneMats[b] * cfgMats[b];
            }
        }
        Perf::Scope sc("pose: point matrices (blend)");
        SkinMatrices(skin, boneMats, pointMats);
    }
    else
    {
        boneMats.clear();
        pointMats.clear();
    }

    Perf::Clock::time_point tv = Perf::Clock::now();
    // GPU skinning: point matrices (+ identity for the attachments); bounds from the mesh points
    boundsMin = restMin;
    boundsMax = restMax;
    if (animate && s_.hasModel)
    {
        renderer_.SetPointMatrices(pointMats, true); // + identity for the attachments
        const P3DLod& l = s_.model.lods[s_.lod];
        boundsMin = glm::vec3(1e30f);
        boundsMax = glm::vec3(-1e30f);
        // bounds (framing, shadow fit): every 4th mesh point is plenty
        for (size_t p = 0; p < l.points.size() && p < pointMats.size(); p += 4)
        {
            if (p < pointInMesh.size() && !pointInMesh[p]) continue;
            glm::vec3 x = glm::vec3(pointMats[p] * glm::vec4(l.points[p].pos, 1.0f));
            boundsMin = glm::min(boundsMin, x);
            boundsMax = glm::max(boundsMax, x);
        }
        if (boundsMin.x > boundsMax.x) { boundsMin = restMin; boundsMax = restMax; }
    }
    else
        renderer_.SetPointMatrices({});
    std::vector<MeshVertex> verts(baseVerts.begin() + std::min(attachFirst, baseVerts.size()), baseVerts.end()); // attachments only
    Perf::Get().Add("pose: point matrices upload + bounds", std::chrono::duration<double, std::milli>(Perf::Clock::now() - tv).count());
    if (s_.hasModel)
    {
        Perf::Scope sc("pose: proxies + crew + IK");
        s_.attach.UpdateTransforms(s_.model.lods[s_.lod], animate ? pointMats : std::vector<glm::mat4>{});
        s_.attach.SetVehiclePose(&s_.skeleton, animate ? boneMats : std::vector<glm::mat4>{});
        s_.attach.RefreshIK(); // hands / legs on the steering wheel, pedals ... (only with IK on)
        if (!s_.attach.unitParts.empty()) s_.unit.Pose(animate ? boneMats : std::vector<glm::mat4>{}, s_.attach); // gear on the same bones
        s_.attach.TransformGeometry(verts, attachFirst);
        // shadow casters: the posed mesh + the attachments (a rifle held out of the body bounds keeps its shadow)
        shadowMin = boundsMin;
        shadowMax = boundsMax;
        for (size_t v = 0; v < verts.size(); v += 3)
        {
            shadowMin = glm::min(shadowMin, verts[v].pos);
            shadowMax = glm::max(shadowMax, verts[v].pos);
        }
        std::vector<LineVertex> stubs;
        s_.attach.StubLines(stubs);
        renderer_.SetLines(LineLayer::Proxies, stubs);
    }
    {
        Perf::Scope sc("pose: upload attachments");
        renderer_.SetMeshRange(UINT(attachFirst), verts);
    }
    Perf::Scope scRest("pose: ghosts, trail, lines, skeleton");
    UpdateGhosts(animate);
    if (animView_.showTrail) UpdateTrail();

    std::vector<LineVertex> lines;
    if (s_.hasModel && display_.showNormals)
    {
        const P3DLod& l = s_.model.lods[s_.lod];
        lines.reserve(normalBase.size() * 2);
        for (const auto& nl : normalBase)
        {
            glm::vec3 p = l.points[nl.point].pos, n = nl.normal;
            if (animate)
            {
                const glm::mat4& m = pointMats[nl.point];
                p = glm::vec3(m * glm::vec4(p, 1.0f));
                n = glm::normalize(glm::mat3(m) * n);
            }
            lines.push_back({ p, nl.color });
            lines.push_back({ p + n * display_.normalLength, nl.color });
        }
    }
    renderer_.SetLines(LineLayer::Normals, lines);
    RebuildSkeletonLines();
}

glm::vec3 SceneView::BonePosition(int bone) const
{
    const glm::vec3& p = s_.skeleton.bones[bone].pivot;
    if (bone < int(boneMats.size())) return glm::vec3(boneMats[bone] * glm::vec4(p, 1.0f));
    return p;
}

void SceneView::RebuildSkeletonLines()
{
    std::vector<LineVertex> v;
    if (!s_.hasModel || s_.skeleton.Empty())
    {
        renderer_.SetLines(LineLayer::Skeleton, v);
        return;
    }
    float j = std::max(glm::length(restMax - restMin) * 0.006f, 0.002f);
    const glm::vec4 cBone(1.0f, 0.85f, 0.25f, 1.0f), cCentroid(1.0f, 0.5f, 0.1f, 1.0f), cSel(1.0f, 0.25f, 1.0f, 1.0f),
        cLink(0.95f, 0.95f, 0.65f, 1.0f), cMissing(1.0f, 0.1f, 0.1f, 1.0f);
    // Bones no active RTM layer has data for (shown red while animating)
    std::vector<bool> covered;
    if (s_.anim.Active() && !playback_.restPose) covered = s_.anim.Covered(s_.skeleton.bones.size());
    for (int i = 0; i < int(s_.skeleton.bones.size()); ++i)
    {
        const Bone& b = s_.skeleton.bones[i];
        if (b.pivotSource == PivotSource::None) continue;
        bool sel = i == selectedBone;
        bool missing = !covered.empty() && !covered[i];
        glm::vec4 c = sel ? cSel : missing ? cMissing : b.pivotSource == PivotSource::PivotsModel ? cBone : cCentroid;
        float s = sel ? j * 2.0f : missing ? j * 2.5f : j;
        glm::vec3 p = BonePosition(i);
        for (int a = 0; a < 3; ++a)
        {
            glm::vec3 d(0.0f);
            d[a] = s;
            v.push_back({ p - d, c });
            v.push_back({ p + d, c });
        }
        if (b.parent >= 0 && s_.skeleton.bones[b.parent].pivotSource != PivotSource::None)
        {
            glm::vec4 lc = sel ? cSel : missing ? cMissing : cLink;
            v.push_back({ BonePosition(b.parent), lc });
            v.push_back({ p, lc });
        }
    }
    renderer_.SetLines(LineLayer::Skeleton, v);
}

IValidationView::BoneUsage SceneView::UsageOfBone(int bone) const
{
    IValidationView::BoneUsage u;
    if (!s_.hasModel || bone < 0 || bone >= int(boneSelection.size()) || boneSelection[bone] < 0) return u;
    const P3DSelection& s = s_.model.lods[s_.lod].selections[boneSelection[bone]];
    std::vector<bool> seen(proxyNames.size(), false);
    for (size_t p = 0; p < s.pointBytes.size() && p < pointInMesh.size(); ++p)
    {
        if (!s.pointBytes[p]) continue;
        if (pointInMesh[p]) ++u.meshPoints;
        if (int px = pointProxy[p]; px >= 0 && !seen[px])
        {
            seen[px] = true;
            u.proxies.push_back(proxyNames[px]);
        }
    }
    return u;
}

void SceneView::LogMissingBones(const std::string& rtmName, const std::vector<int>& bones, bool indent) const
{
    if (bones.empty()) return;
    auto& log = Log::Get();
    const char* pad = indent ? "  " : "";
    std::vector<std::string> names, stretch;
    std::vector<std::pair<std::string, std::string>> proxyLines; // proxy, bone
    for (int b : bones)
    {
        names.push_back(s_.skeleton.bones[b].name);
        IValidationView::BoneUsage u = UsageOfBone(b);
        if (u.meshPoints) stretch.push_back(std::format("{} ({} vert.)", s_.skeleton.bones[b].name, u.meshPoints));
        for (const auto& px : u.proxies) proxyLines.emplace_back(px, s_.skeleton.bones[b].name);
    }
    log.Warn("{}RTM {}: no data for {} skeleton bone(s): {}", pad, rtmName, bones.size(), JoinList(names, 30));
    if (!stretch.empty())
        log.Warn("{}RTM {}: mesh vertices stay in the bind pose - the stretching will show in the game (LOD {}): {}", pad, rtmName, s_.lod,
                 JoinList(stretch, 30));
    for (const auto& [px, bone] : proxyLines)
        log.Warn("{}RTM {}: proxy {} stays in the bind pose (bone {})", pad, rtmName, px, bone);
}

void SceneView::LogLayerCoverage(const AnimLayer& layer) const
{
    if (s_.skeleton.Empty() || !layer.anim) return;
    std::vector<int> missing;
    for (int b = 0; b < int(s_.skeleton.bones.size()); ++b)
        if (b >= int(layer.boneMap.size()) || layer.boneMap[b] < 0) missing.push_back(b);
    LogMissingBones(layer.anim->Name(), missing, false);
}

uint64_t SceneView::PoseSignature() const
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    for (const auto& l : s_.anim.layers)
    {
        const void* ptr = l.anim.get();
        mix(&ptr, sizeof(ptr));
        mix(&l.enabled, 1); mix(&l.mode, sizeof(l.mode)); mix(&l.weight, 4); mix(&l.speed, 4); mix(&l.duration, 4);
        mix(&l.offset, 4); mix(&l.loop, 1); mix(&l.pingPong, 1);
    }
    mix(&s_.anim.missing, sizeof(s_.anim.missing));
    mix(&s_.anim.subtractStep, 1);
    mix(&playback_.restPose, 1);
    return h;
}

void SceneView::UpdateGhosts(bool animate)
{
    std::vector<MeshVertex> gv;
    std::vector<Renderer::Ghost> ghosts;
    const int p = playback_.PrimaryLayer();
    auto addPose = [&](const std::vector<glm::mat4>& bones, glm::vec4 color, glm::vec3 shift) {
        std::vector<glm::mat4> pm;
        SkinMatrices(skin, bones, pm);
        Renderer::Ghost g;
        g.first = UINT(gv.size());
        for (size_t i = 0; i < attachFirst && i < baseVerts.size(); ++i)
        {
            MeshVertex v = baseVerts[i];
            const glm::mat4& m = pm[vertPoint[i]];
            v.pos = glm::vec3(m * glm::vec4(v.pos, 1.0f)) + shift;
            v.normal = glm::mat3(m) * v.normal;
            gv.push_back(v);
        }
        g.count = UINT(gv.size()) - g.first;
        g.color = color;
        ghosts.push_back(g);
    };
    if (animate && !skin.offset.empty() && p >= 0)
    {
        const AnimLayer& l = s_.anim.layers[p];
        if (animView_.onionSkin && l.anim->Frames() > 1)
        {
            const float n = float(l.anim->Frames() - 1);
            const float f = std::round(s_.anim.Phase(l, playback_.time) * n);
            for (int d : { -animView_.onionStep, animView_.onionStep })
            {
                float ph = (f + float(d)) / n;
                if (ph < 0.0f || ph > 1.0f)
                {
                    if (!l.loop) continue;
                    ph -= std::floor(ph);
                }
                std::vector<glm::mat4> bones;
                AnimStack one = s_.anim;
                for (auto& ol : one.layers) ol.pingPong = false;
                one.Evaluate(s_.skeleton, playback_.TimeOfPhase(l, ph), bones);
                addPose(bones, d < 0 ? glm::vec4(0.35f, 0.6f, 1.0f, 0.28f) : glm::vec4(1.0f, 0.55f, 0.25f, 0.28f), glm::vec3(0.0f));
            }
        }
        if (playback_.compareMode && !playback_.compare.layers.empty() && playback_.compare.layers[0].anim)
        {
            std::vector<glm::mat4> bones;
            playback_.compare.Evaluate(s_.skeleton, playback_.time, bones);
            // side by side: to the right on screen, by the model's horizontal size
            const float extent = std::max(restMax.x - restMin.x, restMax.z - restMin.z);
            glm::vec3 right = camera_.Right();
            right.y = 0.0f;
            right = glm::length(right) > 1e-4f ? glm::normalize(right) : glm::vec3(1, 0, 0);
            glm::vec3 shift = playback_.compareMode == 2 ? right * (extent * 1.1f + 0.4f) : glm::vec3(0.0f);
            addPose(bones, playback_.compareMode == 2 ? glm::vec4(0.75f, 0.8f, 0.95f, 0.95f) : glm::vec4(0.3f, 1.0f, 0.5f, 0.35f), shift);
        }
    }
    h_.ExtraGhosts(gv, ghosts); // Reload sync onion skin (weapon + character)
    renderer_.SetGhosts(gv, std::move(ghosts));
}

void SceneView::UpdateTrail()
{
    const int p = playback_.PrimaryLayer();
    if (p < 0 || selectedBone < 0 || selectedBone >= int(s_.skeleton.bones.size()))
    {
        if (!trail.empty()) { trail.clear(); renderer_.SetLines(LineLayer::Trails, trail); }
        trailKey.clear();
        return;
    }
    const AnimLayer& l = s_.anim.layers[p];
    std::string key = std::format("{}|{}|{}|{}|{}", selectedBone, (const void*)l.anim.get(), s_.anim.layers.size(), l.offset, int(s_.anim.missing));
    if (key == trailKey) return;
    trailKey = key;
    trail.clear();
    const int samples = int(std::clamp<size_t>(l.anim->Frames() * 4, 32, 400));
    std::vector<glm::vec3> pts;
    std::vector<glm::mat4> bones;
    const glm::vec3 pivot = s_.skeleton.bones[selectedBone].pivot;
    for (int i = 0; i <= samples; ++i)
    {
        float ph = float(i) / float(samples);
        s_.anim.Evaluate(s_.skeleton, playback_.TimeOfPhase(l, std::min(ph, 0.9999f)), bones);
        pts.push_back(glm::vec3(bones[selectedBone] * glm::vec4(pivot, 1.0f)));
    }
    std::vector<float> len;
    for (size_t i = 1; i < pts.size(); ++i) len.push_back(glm::length(pts[i] - pts[i - 1]));
    std::vector<float> sorted = len;
    std::sort(sorted.begin(), sorted.end());
    float median = sorted.empty() ? 0.0f : sorted[sorted.size() / 2];
    for (size_t i = 1; i < pts.size(); ++i)
    {
        bool jump = len[i - 1] > std::max(0.01f, median * 4.0f);
        glm::vec4 c = jump ? glm::vec4(1, 0.15f, 0.1f, 1) : glm::vec4(1, 0.9f, 0.2f, 1);
        trail.push_back({ pts[i - 1], c });
        trail.push_back({ pts[i], c });
    }
    // keyframes as small crosses
    const size_t nf = l.anim->Frames();
    for (size_t f = 0; f < nf && nf > 1; ++f)
    {
        glm::vec3 q = pts[size_t(std::round(float(f) / float(nf - 1) * float(samples)))];
        const float s = 0.01f;
        for (int ax = 0; ax < 3; ++ax)
        {
            glm::vec3 d(0.0f);
            d[ax] = s;
            trail.push_back({ q - d, glm::vec4(0.3f, 1, 1, 1) });
            trail.push_back({ q + d, glm::vec4(0.3f, 1, 1, 1) });
        }
    }
    renderer_.SetLines(LineLayer::Trails, trail);
    Log::Get().Debug("Bone trail: {} over {} samples ({} line vertices), first ({:.3f} {:.3f} {:.3f}), pivot ({:.3f} {:.3f} {:.3f})", s_.skeleton.bones[selectedBone].name, samples, trail.size(), pts[0].x, pts[0].y, pts[0].z, pivot.x, pivot.y, pivot.z);
}

void SceneView::RebuildGrid()
{
    std::vector<LineVertex> v;
    const int n = 10;
    for (int i = -n; i <= n; ++i)
    {
        glm::vec4 c = (i % 5 == 0) ? glm::vec4(0.42f, 0.42f, 0.45f, 1) : glm::vec4(0.31f, 0.32f, 0.35f, 1);
        glm::vec4 cx = i == 0 ? glm::vec4(0.25f, 0.4f, 0.95f, 1) : c; // line along Z at x=0 -> blue (Z axis)
        glm::vec4 cz = i == 0 ? glm::vec4(0.95f, 0.3f, 0.3f, 1) : c;  // line along X at z=0 -> red (X axis)
        const float gx = gridCell.x, gz = gridCell.y; // control mode: the grid travels with the character
        const float y = gridFloor = FloorY(); // the floor: the shadow falls on the grid
        v.push_back({ { float(i) + gx, y, float(-n) + gz }, cx });
        v.push_back({ { float(i) + gx, y, float(n) + gz }, cx });
        v.push_back({ { float(-n) + gx, y, float(i) + gz }, cz });
        v.push_back({ { float(n) + gx, y, float(i) + gz }, cz });
    }
    renderer_.SetLines(LineLayer::Grid, v);
}

const FaceDiagInfo& SceneView::FaceInfo()
{
    if (s_.hasModel && faceInfo.mirrored.size() != s_.model.lods[size_t(s_.lod)].faces.size()) faceInfo = AnalyzeFaces(s_.model.lods[size_t(s_.lod)]);
    return faceInfo;
}

void SceneView::RebuildLodOverlay()
{
    std::vector<LineVertex> v;
    if (s_.hasModel)
        for (int i = 0; i < int(s_.model.lods.size()); ++i)
        {
            const P3DLod& l = s_.model.lods[i];
            if (i == s_.lod || !vis_.overlays.count(ModelVisibility::Key(l.resolution))) continue;
            ImVec4 c(0, 0, 0, 1);
            ImGui::ColorConvertHSVtoRGB(std::fmod(i * 0.137f + 0.55f, 1.0f), 0.7f, 1.0f, c.x, c.y, c.z);
            const glm::vec4 col(c.x, c.y, c.z, 0.85f);
            for (const auto& f : l.faces)
                for (uint32_t k = 0; k < f.count; ++k)
                {
                    const uint32_t pa = f.v[k].point, pb = f.v[(k + 1) % f.count].point;
                    if (pa >= l.points.size() || pb >= l.points.size()) continue;
                    v.push_back({ l.points[pa].pos, col });
                    v.push_back({ l.points[pb].pos, col });
                }
        }
    renderer_.SetLines(LineLayer::LodOverlay, v);
}

void SceneView::LoadDamageMaterials()
{
    damageMat.clear();
    damageTex.clear();
    const ArmaConfig* cfg = s_.CrewConfig();
    if (!cfg || s_.attach.VehicleClass().empty()) return;
    const CfgClass* veh = cfg->Root().FindClass("CfgVehicles");
    const CfgClass* cls = veh ? veh->FindClass(s_.attach.VehicleClass()) : nullptr;
    const CfgClass* dmg = cls ? cls->FindClass("Damage") : nullptr;
    if (!dmg) return;
    const auto mats = dmg->GetStrings("mat");
    for (size_t i = 0; i + 2 < mats.size(); i += 3)
        damageMat[ToLower(mats[i])] = { mats[i + 1], mats[i + 2] };
    const auto tex = dmg->GetStrings("tex");
    for (size_t i = 0; i + 1 < tex.size(); i += 2) damageTex[ToLower(tex[i])] = tex[i + 1];
    Log::Get().Info("Damage materials of {}: {} RVMAT triples, {} texture pairs", s_.attach.VehicleClass(), damageMat.size(), damageTex.size());
}

// the face of the current LOD under a screen point (normalized device x, y), posed
bool SceneView::PickFace(const glm::mat4& viewProj, float nx, float ny, uint32_t& face) const
{
    if (!s_.hasModel) return false;
    const glm::mat4 inv = glm::inverse(viewProj);
    const glm::vec4 a = inv * glm::vec4(nx, ny, 0.0f, 1.0f), b = inv * glm::vec4(nx, ny, 1.0f, 1.0f);
    const glm::vec3 ro = glm::vec3(a) / a.w, rd = glm::normalize(glm::vec3(b) / b.w - ro);
    const P3DLod& l = s_.model.lods[size_t(s_.lod)];
    auto posed = [&](uint32_t p) {
        const glm::vec3 q = l.points[p].pos;
        return p < pointMats.size() ? glm::vec3(pointMats[p] * glm::vec4(q, 1.0f)) : q;
    };
    float best = 1e30f;
    bool hit = false;
    for (uint32_t fi = 0; fi < uint32_t(l.faces.size()); ++fi)
    {
        if (fi < faceHidden.size() && faceHidden[fi]) continue;
        const P3DFace& f = l.faces[fi];
        if (f.count < 3) continue;
        bool ok = true;
        for (uint32_t c = 0; c < f.count; ++c) ok = ok && f.v[c].point < l.points.size();
        if (!ok) continue;
        for (uint32_t t = 0; t + 2 < f.count; ++t)
        {
            // Moller-Trumbore, both sides
            const glm::vec3 p0 = posed(f.v[0].point), p1 = posed(f.v[t + 1].point), p2 = posed(f.v[t + 2].point);
            const glm::vec3 e1 = p1 - p0, e2 = p2 - p0, pv = glm::cross(rd, e2);
            const float det = glm::dot(e1, pv);
            if (std::abs(det) < 1e-12f) continue;
            const float id = 1.0f / det;
            const glm::vec3 tv = ro - p0;
            const float u = glm::dot(tv, pv) * id;
            if (u < 0 || u > 1) continue;
            const glm::vec3 qv = glm::cross(tv, e1);
            const float w = glm::dot(rd, qv) * id;
            if (w < 0 || u + w > 1) continue;
            const float d = glm::dot(e2, qv) * id;
            if (d > 0 && d < best) { best = d; face = fi; hit = true; }
        }
    }
    return hit;
}

// control mode: the character at its root position / heading, footsteps on the ground
void SceneView::ApplyRootMotion(MovesControl& control, std::vector<glm::mat4>& bones)
{
    if (!control.on) return;
    const glm::mat4 m = control.RootMatrix();
    for (auto& b : bones) b = m * b;
    if (!control.AddFootsteps(bones, s_.skeleton)) return;
    std::vector<LineVertex> v;
    for (const auto& s : control.footsteps)
    {
        const glm::vec4 c(1.0f, 0.75f, 0.3f, 1.0f);
        v.push_back({ s + glm::vec3(-0.06f, 0, 0), c });
        v.push_back({ s + glm::vec3(0.06f, 0, 0), c });
        v.push_back({ s + glm::vec3(0, 0, -0.06f), c });
        v.push_back({ s + glm::vec3(0, 0, 0.06f), c });
    }
    renderer_.SetLines(LineLayer::Footsteps, v);
}

// ---------------------------------------------------------------------------------------------- hide / isolate

void SceneView::VisibilityChanged()
{
    vis_.Save();
    RebuildLodGeometry();
}

void SceneView::HideSelected()
{
    if (!s_.hasModel) return;
    const P3DLod& l = s_.model.lods[s_.lod];
    LodVisibility& v = vis_.Lod(l.resolution);
    if (!pickFaces.empty())
    {
        v.hiddenFaces.insert(pickFaces.begin(), pickFaces.end());
        pickFaces.clear();
    }
    else if (highlight == Highlight::Selection && selection >= 0 && selection < int(l.selections.size()))
    {
        v.hidden.insert(ToLower(l.selections[selection].name));
        selection = selectedBone = -1;
        highlight = Highlight::None;
    }
    else
    {
        Log::Get().Info("H: nothing selected - click a selection / proxy in the Outliner or Ctrl+click faces");
        return;
    }
    VisibilityChanged();
    RebuildSkeletonLines();
}

void SceneView::IsolateSelected()
{
    if (!s_.hasModel) return;
    const P3DLod& l = s_.model.lods[s_.lod];
    LodVisibility& v = vis_.Lod(l.resolution);
    if (!pickFaces.empty())
    {
        v.isolate = "#faces";
        v.isolateFaces = pickFaces;
        pickFaces.clear();
    }
    else if (highlight == Highlight::Selection && selection >= 0 && selection < int(l.selections.size()))
    {
        const std::string& name = l.selections[selection].name;
        v.isolate = IEquals(v.isolate, name) ? std::string() : name; // again: back
        v.isolateFaces.clear();
    }
    else if (!v.isolate.empty())
    {
        v.isolate.clear();
        v.isolateFaces.clear();
    }
    else
    {
        Log::Get().Info("Shift+H: nothing selected - click a selection / proxy in the Outliner or Ctrl+click faces");
        return;
    }
    VisibilityChanged();
}

void SceneView::ShowAllHidden()
{
    if (!s_.hasModel) return;
    vis_.ShowAll(s_.model.lods[s_.lod].resolution);
    VisibilityChanged();
}

// what the drawn LOD needs once: bone pivots and animation axes in this LOD, bone -> selection, validation, which points
// are mesh vertices and which only proxy corners, the highlight kept on the selected bone
void SceneView::PrepareLod(bool memoryAxisFallback)
{
    if (!s_.hasModel) return;
    const P3DLod& l = s_.model.lods[size_t(s_.lod)];
    ResolvePivots(s_.skeleton, s_.hasPivots ? &s_.pivots : nullptr, &l);
    ResolveAnimationAxes(s_.cfgAnims, s_.model, l, memoryAxisFallback);
    boneSelection = MapBonesToSelections(s_.skeleton, l);
    validation = Validate(s_.skeleton, l, s_.modelInfo.sections);

    // Which points are real mesh vertices and which only belong to proxy triangles.
    pointInMesh.assign(l.points.size(), 0);
    pointProxy.assign(l.points.size(), -1);
    proxyNames.clear();
    {
        std::vector<int> faceProxy(l.faces.size(), -1);
        for (const auto& s : l.selections)
            if (s.name.starts_with("proxy:"))
            {
                int idx = int(proxyNames.size());
                proxyNames.push_back(s.name.substr(6));
                for (size_t f = 0; f < s.faceBytes.size() && f < faceProxy.size(); ++f)
                    if (s.faceBytes[f]) faceProxy[f] = idx;
            }
        for (size_t f = 0; f < l.faces.size(); ++f)
            for (uint32_t k = 0; k < l.faces[f].count; ++k)
            {
                uint32_t p = l.faces[f].v[k].point;
                if (p >= l.points.size()) continue;
                if (faceProxy[f] >= 0) pointProxy[p] = faceProxy[f];
                else pointInMesh[p] = 1;
            }
    }

    if (selectedBone >= 0 && selectedBone < int(boneSelection.size()))
    {
        selection = boneSelection[selectedBone];
        highlight = selection >= 0 ? Highlight::Selection : Highlight::None;
    }
    else if (highlight == Highlight::Selection)
    {
        selection = -1;
        highlight = Highlight::None;
    }

    if (!s_.skeleton.Empty())
    {
        int fromPivots = 0, fromCentroid = 0, none = 0;
        for (const auto& b : s_.skeleton.bones)
            (b.pivotSource == PivotSource::PivotsModel ? fromPivots : b.pivotSource == PivotSource::SelectionCentroid ? fromCentroid : none)++;
        Log::Get().Debug("LOD {}: bone pivots: {} from pivotsModel, {} from selection centroid, {} missing", s_.lod, fromPivots, fromCentroid, none);
    }
}

AnimStatus SceneView::AnimStatusOf(const CfgAnimation& a) const { return ::AnimStatusOf(a, boneSelection, s_.lod); }

void SceneView::LogMaterials() const
{
    if (!s_.hasModel) return;
    int ok = 0, proc = 0, missing = 0, errors = 0, rvOk = 0, rvBad = 0;
    for (const auto& [k, m] : mats_.loaded) (m.loaded ? rvOk : rvBad)++;
    auto& cache = const_cast<TextureCache&>(texCache_); // Get() only fills the cache
    for (const auto& ms : meshSections)
    {
        if (ms.texture.empty()) continue;
        switch (cache.Get(ms.texture).status)
        {
        case TextureEntry::Status::Ok: ++ok; break;
        case TextureEntry::Status::Procedural: ++proc; break;
        case TextureEntry::Status::Missing: ++missing; break;
        default: ++errors; break;
        }
    }
    Log::Get().Info("Materials LOD {}: {} sections; face textures: {} files, {} procedural, {} missing, {} broken; rvmats: {} loaded, {} missing; {} textures in cache",
                    s_.lod, meshSections.size(), ok, proc, missing, errors, rvOk, rvBad, texCache_.Count());
}

void SceneView::LogCfgAnimations() const
{
    auto& log = Log::Get();
    if (s_.cfgAnims.empty())
    {
        if (!s_.skeleton.Empty()) log.Info("CfgModels::{}: no Animations", s_.modelInfo.modelClass);
        return;
    }
    std::map<std::string, int> byType;
    int problems = 0;
    for (const auto& a : s_.cfgAnims)
    {
        ++byType[ToLower(a.typeName)];
        problems += AnimStatusOf(a).severity == Severity::Error;
    }
    std::string types;
    for (auto& [t, n] : byType) types += std::format("{}{} {}", types.empty() ? "" : ", ", t, n);
    log.Info("CfgModels::{} animations: {} ({}), sources {}, with problems {}", s_.modelInfo.modelClass, s_.cfgAnims.size(), types,
             s_.cfgSources.size(), problems);
    int inactive = 0;
    for (const auto& a : s_.cfgAnims)
    {
        AnimStatus st = AnimStatusOf(a);
        if (st.severity == Severity::Error) log.Error("  {}: animation {}: {}", s_.cfg.Where(a.file, a.line), a.name, st.message);
        else if (st.severity == Severity::Warning) log.Warn("  {}: animation {}: {}", s_.cfg.Where(a.file, a.line), a.name, st.message);
        else if (st.severity == Severity::Inactive) ++inactive;
    }
    if (inactive)
        log.Info("  {} animation(s) move bones without points in LOD {} (e.g. interior parts of other LODs) - not checked here", inactive, s_.lod);
}
