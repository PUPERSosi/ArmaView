// Reload sync and optic fitting without a view: the character holding the weapon in weapon space (stance + handAnim +
// reload gesture, hand IK by the gesture's curves), the weapon's model.cfg animations of the reload source, the contact
// distance of a hand bone to a part over the reload, the min / max windows (suggested, saved to the model.cfg with .bak
// and diff), another weapon for comparison; attachments on the slots and the sight numbers.
#include "ReloadSync.h"
#include "Log.h"
#include "Strings.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

#include "CoreGuard.h"

namespace fs = std::filesystem;

namespace {

constexpr float kDeg = 57.2957795f;

std::string Num(float v) { return ShortNum(v); }

fs::file_time_type Stamp(const fs::path& p)
{
    std::error_code ec;
    if (p.empty()) return {};
    auto t = fs::last_write_time(p, ec);
    return ec ? fs::file_time_type{} : t;
}

std::shared_ptr<RtmAnimation> LoadRtmFile(const fs::path& p, std::string& problem)
{
    if (p.empty()) return nullptr;
    auto a = std::make_shared<RtmAnimation>();
    std::string err;
    std::vector<std::string> warn;
    if (!LoadRtm(p, *a, err, warn))
    {
        problem += std::format("{}: {}; ", PathToUtf8(p.filename()), err);
        return nullptr;
    }
    return a;
}

fs::path RtmPath(const std::string& arma, const fs::path& root)
{
    if (arma.empty()) return {};
    fs::path p = ResolveArmaPath(arma, root);
    if (!p.has_extension()) p += ".rtm";
    return p;
}

// a class of cfg defined at (file, line): its path "A/B/C" from the root
bool ClassPathAt(const CfgClass& c, int file, int line, std::string prefix, std::string& out)
{
    for (const auto& k : c.classes)
    {
        const std::string p = prefix.empty() ? k->name : prefix + "/" + k->name;
        if (!k->external && k->file == file && k->line == line) { out = p; return true; }
        if (ClassPathAt(*k, file, line, p, out)) return true;
    }
    return false;
}

} // namespace

bool MemoryPointOf(const P3DModel& m, const std::string& name, glm::vec3& out)
{
    for (const auto& l : m.lods)
    {
        if (LodResolutionName(l.resolution) != "Memory") continue;
        for (const auto& s : l.selections)
        {
            if (!IEquals(s.name, name)) continue;
            glm::vec3 sum(0);
            int n = 0;
            for (size_t i = 0; i < s.pointBytes.size() && i < l.points.size(); ++i)
                if (s.pointBytes[i]) { sum += l.points[i].pos; ++n; }
            if (!n) return false;
            out = sum / float(n);
            return true;
        }
    }
    return false;
}

float ReloadSync::ReloadPhase(double t) const { return std::clamp(float(reloadK_ * t / ReloadDuration()), 0.0f, 1.0f); }

float ReloadSync::ReloadDuration() const
{
    if (wchar_.gesture) return std::max(0.05f, wchar_.gestureDuration);
    if (weapon_.magazineReloadTime > 0) return weapon_.magazineReloadTime;
    return 1.0f;
}

void ReloadSync::WeaponCharLoad()
{
    wchar_ = WeaponChar{};
    const ArmaConfig* cfg = CrewConfig();
    if (!cfg || !game_) { wchar_.problem = "game configs are still loading"; return; }
    const fs::path root = projectRoot_.empty() ? fs::path("P:\\") : projectRoot_;
    wchar_.model = attach_.Cache().Get(fs::path(Utf8ToWide(attach_.crewModel)), true, root);
    if (!wchar_.model || wchar_.model->status != AttachedModel::Status::Ok || !wchar_.model->rigged)
    {
        wchar_.problem = std::format("character model {}: {}", attach_.crewModel, wchar_.model ? wchar_.model->message : "-");
        return;
    }
    const Skeleton& sk = wchar_.model->skeleton;
    // the weapon / pistol proxy of the character's first LOD
    const std::string stem = weapon_.pistol ? "pistol" : "weapon";
    for (const auto& s : wchar_.model->model.lods.front().selections)
    {
        if (!s.name.starts_with("proxy:")) continue;
        std::string p = ToLower(s.name.substr(6));
        if (size_t dot = p.find_last_of('.'); dot != std::string::npos) p.resize(dot);
        if (size_t sl = p.find_last_of("\\/"); sl != std::string::npos) p = p.substr(sl + 1);
        if (p != stem) continue;
        int n = 0;
        for (size_t i = 0; i < s.pointBytes.size() && n < 3; ++i)
            if (s.pointBytes[i]) wchar_.proxyPoints[n++] = uint32_t(i);
        wchar_.proxyOk = n == 3;
        break;
    }
    if (!wchar_.proxyOk) wchar_.problem += std::format("no proxy '{}' in the character's first LOD; ", stem);
    else
    {
        const auto& pts = wchar_.model->model.lods.front().points;
        const glm::vec3 a = pts[wchar_.proxyPoints[0]].pos, b = pts[wchar_.proxyPoints[1]].pos, c = pts[wchar_.proxyPoints[2]].pos;
        Log::Get().Debug("Weapon: character proxy '{}' ({:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f}), edges {:.4f} {:.4f} {:.4f}", stem,
                        a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z, glm::length(b - a), glm::length(c - a), glm::length(c - b));
    }
    // masks of the move + gesture configs
    auto masks = ReadBlendAnims(*cfg, "CfgMovesMaleSdr");
    for (auto& [k, m] : ReadBlendAnims(*cfg, "CfgGesturesMale")) masks[k] = m;
    auto maskWeights = [&](const std::string& name) {
        std::vector<float> w(sk.bones.size(), 0.0f);
        auto it = masks.find(ToLower(name));
        if (it == masks.end()) return std::vector<float>{};
        for (size_t b = 0; b < sk.bones.size(); ++b)
            if (auto jt = it->second.find(ToLower(sk.bones[b].name)); jt != it->second.end()) w[b] = jt->second;
        return w;
    };
    // stance
    wchar_.baseState = weapon_.pistol ? "AmovPercMstpSrasWpstDnon" : "AmovPercMstpSrasWrflDnon";
    const MovesStateInfo st = LookupMovesState(cfg, wchar_.baseState);
    if (st.found)
    {
        wchar_.baseFile = RtmPath(st.file, root);
        wchar_.baseDuration = DurationFromMovesSpeed(st.speed) > 0 ? DurationFromMovesSpeed(st.speed) : 1.0f;
        wchar_.base = LoadRtmFile(wchar_.baseFile, wchar_.problem);
    }
    else wchar_.problem += wchar_.baseState + " not in CfgMovesMaleSdr; ";
    // handAnim
    wchar_.handFile = RtmPath(weapon_.handAnim, root);
    wchar_.hand = LoadRtmFile(wchar_.handFile, wchar_.problem);
    // reload gesture: CfgGesturesMale >> States >> reloadAction
    if (!weapon_.reloadAction.empty())
    {
        // just this state: the whole gesture graph is not needed (and the move graph never)
        MoveState s;
        {
            if (ReadMoveState(*cfg, "CfgGesturesMale", weapon_.reloadAction, root, s))
            {
                wchar_.gestureName = s.name;
                wchar_.gestureMask = s.mask.empty() ? "handsWeapon" : s.mask;
                wchar_.gestureSpeed = s.speed;
                wchar_.gestureDuration = s.duration;
                wchar_.gestureInterp = s.interpolationSpeed;
                wchar_.gestureLooped = s.looped;
                wchar_.gestureFile = s.path;
                wchar_.gestureRef.file = cfg->FileOf(s.cfgFile);
                wchar_.gestureRef.line = s.cfgLine;
                wchar_.leftIK = s.leftHandIKCurve;
                wchar_.rightIK = s.rightHandIKCurve;
                if (s.fileExists) wchar_.gesture = LoadRtmFile(s.path, wchar_.problem);
                else wchar_.problem += std::format("gesture RTM {} not on disk; ", s.file);
            }
            else wchar_.problem += std::format("reloadAction {} is not a CfgGesturesMale state; ", weapon_.reloadAction);
        }
    }
    // stack: stance (blend, looped) + handAnim (override, hands mask, frame 0) + reload gesture (override, its mask)
    AnimLayer base;
    base.anim = wchar_.base;
    base.duration = wchar_.baseDuration;
    base.enabled = wchar_.base != nullptr;
    AnimLayer hand;
    hand.anim = wchar_.hand;
    hand.mode = LayerMode::Override;
    hand.speed = 0.0f;
    hand.loop = false;
    hand.boneWeight = maskWeights("handsWeapon");
    hand.enabled = wchar_.hand != nullptr;
    AnimLayer ges;
    ges.anim = wchar_.gesture;
    ges.mode = LayerMode::Override;
    ges.loop = false;
    ges.duration = wchar_.gestureDuration;
    ges.boneWeight = maskWeights(wchar_.gestureMask);
    ges.enabled = wchar_.gesture != nullptr;
    for (AnimLayer* l : { &base, &hand, &ges })
        if (!l->anim) { l->anim = std::make_shared<RtmAnimation>(); l->enabled = false; }
    wchar_.stack.layers = { base, hand, ges };
    wchar_.stack.missing = MissingBones::FollowParent;
    wchar_.stack.Bind(sk);
    wchar_.stamp[0] = Stamp(wchar_.baseFile);
    wchar_.stamp[1] = Stamp(wchar_.handFile);
    wchar_.stamp[2] = Stamp(wchar_.gestureFile);
    Log::Get().Info("Weapon {}: reloadAction {} -> {} ({:.2f} s, mask {}), handAnim {}, stance {}{}", weapon_.cls, weapon_.reloadAction,
                    PathToUtf8(wchar_.gestureFile.filename()), wchar_.gestureDuration, wchar_.gestureMask, PathToUtf8(wchar_.handFile.filename()),
                    wchar_.baseState, wchar_.problem.empty() ? "" : " - " + wchar_.problem);
}

bool ReloadSync::WeaponCharPose(double t, bool withGesture, std::vector<glm::mat4>& bones, glm::mat4& toWeapon, std::vector<MeshVertex>* verts)
{
    if (!wchar_.model || !wchar_.model->rigged || !wchar_.proxyOk || wchar_.stack.layers.size() < 3) return false;
    const AttachedModel& m = *wchar_.model;
    AnimLayer& g = wchar_.stack.layers[2];
    const bool gestureWas = g.enabled;
    g.enabled = gestureWas && withGesture;
    wchar_.stack.overrideRelative = blendMode_ == 1;
    g.weight = gestureFade_ ? GestureFadeWeight(t, wchar_.gestureDuration, wchar_.gestureInterp, wchar_.gestureLooped) : 1.0f;
    const double te = std::clamp(t, 0.0, double(ReloadDuration()) / std::max(0.01f, reloadK_));
    wchar_.stack.Evaluate(m.skeleton, te, bones);
    g.enabled = gestureWas;
    const P3DLod& l = m.model.lods.front();
    // the weapon proxy (character space) of a pose
    auto proxyOf = [&](const std::vector<glm::mat4>& bs, bool& ok) {
        std::vector<glm::mat4> pm;
        SkinMatrices(m.skin, bs, pm);
        glm::vec3 p[3];
        for (int i = 0; i < 3; ++i)
        {
            const uint32_t pi = wchar_.proxyPoints[i];
            const glm::vec3 x = pi < l.points.size() ? l.points[pi].pos : glm::vec3(0);
            p[i] = pi < pm.size() ? glm::vec3(pm[pi] * glm::vec4(x, 1.0f)) : x;
        }
        return ProxyMatrix(p, &ok);
    };
    bool valid = false;
    const glm::mat4 w = proxyOf(bones, valid);
    if (!valid) return false;
    // Hand IK as in the game: leftHandIKCurve / rightHandIKCurve of the gesture (phase, weight) pull each hand back to its
    // grip on the weapon. The grip is where the handAnim puts the hand relative to the weapon (pose without the gesture),
    // carried along with the weapon as the gesture moves it. Weight 1: the hand is on the grip, 0: the gesture's hand.
    handIKWeight_[0] = handIKWeight_[1] = 0.0f;
    if (handIK_ && withGesture && gestureWas && (wchar_.leftIK.size() >= 2 || wchar_.rightIK.size() >= 2))
    {
        std::vector<glm::mat4> grip;
        g.enabled = false;
        wchar_.stack.Evaluate(m.skeleton, te, grip);
        g.enabled = gestureWas;
        bool okGrip = false;
        const glm::mat4 w0 = proxyOf(grip, okGrip);
        const float phase = float(std::clamp(te / std::max(1e-3f, wchar_.gestureDuration), 0.0, 1.0));
        static const char* const kChain[2][3] = { { "LeftArm", "LeftForeArm", "LeftHand" }, { "RightArm", "RightForeArm", "RightHand" } };
        for (int k = 0; k < 2 && okGrip; ++k)
        {
            const auto& c = k ? wchar_.rightIK : wchar_.leftIK;
            if (c.size() < 2) continue;
            const float wk = std::clamp(CurveAt(c, phase), 0.0f, 1.0f);
            handIKWeight_[k] = wk;
            const int ja = m.skeleton.Find(kChain[k][0]), jb = m.skeleton.Find(kChain[k][1]), jc = m.skeleton.Find(kChain[k][2]);
            if (wk <= 0.0f || ja < 0 || jb < 0 || jc < 0) continue;
            const glm::mat4 target = w * glm::inverse(w0) * grip[size_t(jc)]; // the grip, moved with the weapon
            const glm::vec3 pivot = m.skeleton.bones[size_t(jc)].pivot;
            const glm::vec3 cur = glm::vec3(bones[size_t(jc)] * glm::vec4(pivot, 1.0f)), tgt = glm::vec3(target * glm::vec4(pivot, 1.0f));
            TwoBoneIK(m.skeleton, bones, ja, jb, jc, glm::mix(cur, tgt, wk));
            // hand orientation towards the grip's by the same weight
            const glm::quat qNow = glm::normalize(glm::quat_cast(glm::mat3(bones[size_t(jc)])));
            const glm::quat qWant = glm::slerp(qNow, glm::normalize(glm::quat_cast(glm::mat3(target))), wk);
            RotateSubtree(m.skeleton, bones, jc, glm::vec3(bones[size_t(jc)] * glm::vec4(pivot, 1.0f)), glm::normalize(qWant * glm::inverse(qNow)));
        }
    }
    std::vector<glm::mat4> pm;
    SkinMatrices(m.skin, bones, pm);
    toWeapon = glm::inverse(w);
    if (verts)
    {
        SkinModelVerts(m, pm, *verts);
        const glm::mat3 r(toWeapon);
        for (auto& v : *verts)
        {
            v.pos = glm::vec3(toWeapon * glm::vec4(v.pos, 1.0f));
            v.normal = r * v.normal;
        }
    }
    return true;
}

bool ReloadSync::WeaponEnsure()
{
    const ArmaConfig* cfg = CrewConfig();
    const std::string key = hasModel_ ? std::format("{}|{}|{}|{}", PathToUtf8(model_.path), (const void*)cfg, (const void*)game_.get(), attach_.crewModel) : std::string();
    bool reloaded = false;
    if (key != weaponKey_)
    {
        reloaded = true;
        weaponKey_ = key;
        weapon_ = hasModel_ ? ReadWeaponConfig(cfg, ArmaPathOf(model_.path, projectRoot_), "OFP2_ManSkeleton") : WeaponCfg{};
        WeaponCharLoad();
        weaponStamp_ = Stamp(model_.path);
    }
    // windows driven by the reload source; unsaved edits survive a model.cfg reload
    reloadAnims_.clear();
    for (int i = 0; i < int(cfgAnims_.size()); ++i)
        if (IEquals(cfgAnims_[i].source, reloadSource_)) reloadAnims_.push_back(i);
    for (const auto& [name, e] : windowEdits_)
        for (auto& a : cfgAnims_)
            if (IEquals(a.name, name) && (a.minValue != e.min1 || a.maxValue != e.max1) && a.minValue == e.min0 && a.maxValue == e.max0)
            {
                a.minValue = e.min1;
                a.maxValue = e.max1;
                poseChanged_ = true;
            }
    return reloaded;
}

bool ReloadSync::CompareLoad(const std::string& input)
{
    cmp_ = CompareWeapon{};
    std::string s = input;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.empty()) { weaponMsg_ = T("comparison off"); return true; }
    const ArmaConfig* cfg = CrewConfig();
    const fs::path root = projectRoot_.empty() ? fs::path("P:\\") : projectRoot_;
    std::string modelPath, reloadAction;
    if (s.find('\\') != std::string::npos || s.find('/') != std::string::npos || ToLower(s).ends_with(".p3d"))
        modelPath = s;
    else if (const CfgClass* w = cfg && cfg->Root().FindClass("CfgWeapons") ? cfg->Root().FindClass("CfgWeapons")->FindClass(s) : nullptr)
    {
        modelPath = w->GetString("model");
        reloadAction = w->GetString("reloadAction");
    }
    else { cmp_.problem = TF("{}: no such CfgWeapons class (or give a .p3d path)", s); weaponMsg_ = cmp_.problem; return false; }
    fs::path file = modelPath.size() > 1 && modelPath[1] == ':' ? fs::path(Utf8ToWide(modelPath)) : ResolveArmaPath(modelPath, root);
    if (!file.has_extension()) file += ".p3d";
    cmp_.cls = s;
    cmp_.model = attach_.Cache().Get(file, false, root);
    if (!cmp_.model || cmp_.model->status != AttachedModel::Status::Ok) { cmp_.problem = TF("{}: {}", PathToUtf8(file), cmp_.model ? cmp_.model->message : "-"); weaponMsg_ = cmp_.problem; cmp_.model.reset(); return false; }
    // its rig: the model.cfg next to the MLOD (or in a parent folder up to the project root)
    fs::path modelCfg;
    std::error_code ec;
    for (fs::path dir = file.parent_path(); !dir.empty(); dir = dir.parent_path())
    {
        if (fs::exists(dir / "model.cfg", ec)) { modelCfg = dir / "model.cfg"; break; }
        if (dir == dir.root_path() || fs::equivalent(dir, root, ec)) break;
    }
    ModelCfgInfo info;
    ArmaConfig mcfg;
    if (modelCfg.empty() || !mcfg.Load(modelCfg, root) || !BuildSkeleton(mcfg, PathToUtf8(file.stem()), cmp_.skel, info))
    {
        cmp_.problem = TF("{}: no model.cfg with its skeleton next to the model", PathToUtf8(file.filename()));
        weaponMsg_ = cmp_.problem;
        cmp_.model.reset();
        return false;
    }
    BuildCfgAnimations(mcfg, info, cmp_.skel, cmp_.anims, cmp_.sources);
    ResolveAnimationAxes(cmp_.anims, cmp_.model->model, cmp_.model->model.lods.front(), true);
    cmp_.skin = BuildSkin(cmp_.skel, cmp_.model->model.lods.front(), true, 4);
    for (int i = 0; i < int(cmp_.anims.size()); ++i)
        if (IEquals(cmp_.anims[size_t(i)].source, reloadSource_)) cmp_.reloadAnims.push_back(i);
    // its reload duration: the reload gesture of its class (as ours), else ours
    cmp_.duration = ReloadDuration();
    if (!reloadAction.empty() && cfg)
    {
        MoveState g;
        if (ReadMoveState(*cfg, "CfgGesturesMale", reloadAction, root, g)) cmp_.duration = std::max(0.05f, g.duration);
    }
    weaponMsg_ = TF("comparison: {} ({} animations, {} on {}, reload {:.2f} s) below the weapon", s, cmp_.anims.size(), cmp_.reloadAnims.size(), reloadSource_, cmp_.duration);
    Log::Get().Info("Weapon: {}", weaponMsg_);
    return true;
}

glm::vec3 ReloadSync::SelectionCentroidAt(int sel, float phase)
{
    const P3DLod& l = model_.lods[lod_];
    if (sel < 0 || sel >= int(l.selections.size())) return glm::vec3(0);
    std::vector<glm::mat4> pm;
    if (!skeleton_.Empty() && !skin_.offset.empty() && !cfgAnims_.empty())
    {
        std::vector<CfgSource> src = cfgSources_;
        for (auto& s : src)
            if (IEquals(s.name, reloadSource_)) s.value = phase;
        std::vector<glm::mat4> mats;
        EvaluateCfgAnimations(skeleton_, cfgAnims_, src, mats);
        SkinMatrices(skin_, mats, pm);
    }
    const auto& s = l.selections[sel];
    glm::vec3 sum(0);
    float n = 0;
    for (size_t i = 0; i < s.pointBytes.size() && i < l.points.size(); ++i)
        if (s.pointBytes[i])
        {
            sum += i < pm.size() ? glm::vec3(pm[i] * glm::vec4(l.points[i].pos, 1.0f)) : l.points[i].pos;
            n += 1;
        }
    return n > 0 ? sum / n : glm::vec3(0);
}

glm::vec3 ReloadSync::CharJoint(int bone, const std::vector<glm::mat4>& bones, const glm::mat4& toWeapon) const
{
    if (!wchar_.model || bone < 0 || bone >= int(bones.size())) return glm::vec3(0);
    return glm::vec3(toWeapon * bones[bone] * glm::vec4(wchar_.model->skeleton.bones[bone].pivot, 1.0f));
}

void ReloadSync::ComputeTouch()
{
    touchDist_.clear();
    if (touchBone_ < 0 || touchSel_ < 0 || !wchar_.model) return;
    const float T = ReloadDuration() / std::max(0.01f, reloadK_);
    const int n = 121;
    touchMin_ = 1e30f;
    for (int i = 0; i < n; ++i)
    {
        const double t = double(T) * i / (n - 1);
        std::vector<glm::mat4> bones;
        glm::mat4 tw(1.0f);
        if (!WeaponCharPose(t, true, bones, tw, nullptr)) { touchDist_.clear(); return; }
        const float d = glm::length(CharJoint(touchBone_, bones, tw) - SelectionCentroidAt(touchSel_, ReloadPhase(t)));
        touchDist_.push_back(d);
        if (d < touchMin_) { touchMin_ = d; touchMinT_ = float(t); }
    }
}

bool ReloadSync::SuggestWindow()
{
    if (touchDist_.empty() || touchAnim_ < 0 || touchAnim_ >= int(cfgAnims_.size())) { weaponMsg_ = "compute the distance and pick the animation first"; return false; }
    const float T = ReloadDuration() / std::max(0.01f, reloadK_);
    const float thr = touchThresholdCm_ * 0.01f;
    const int n = int(touchDist_.size());
    int im = int(std::min_element(touchDist_.begin(), touchDist_.end()) - touchDist_.begin());
    if (touchDist_[im] > thr) { weaponMsg_ = std::format("closest {:.1f} cm - over the threshold {:.1f} cm", touchDist_[im] * 100, touchThresholdCm_); return false; }
    int a = im, b = im;
    while (a > 0 && touchDist_[a - 1] <= thr) --a;
    while (b + 1 < n && touchDist_[b + 1] <= thr) ++b;
    const float t0 = T * a / (n - 1), t1 = T * b / (n - 1);
    SetWindow(touchAnim_, ReloadPhase(t0), ReloadPhase(t1));
    weaponMsg_ = std::format("{}: window {:.3f}..{:.3f} (contact {:.2f}..{:.2f} s, closest {:.1f} cm at {:.2f} s)", cfgAnims_[touchAnim_].name,
                             ReloadPhase(t0), ReloadPhase(t1), t0, t1, touchMin_ * 100, touchMinT_);
    return true;
}

void ReloadSync::SetWindow(int a, float mn, float mx)
{
    if (a < 0 || a >= int(cfgAnims_.size())) return;
    CfgAnimation& an = cfgAnims_[a];
    const std::string key = ToLower(an.name);
    auto it = windowEdits_.find(key);
    if (it == windowEdits_.end()) it = windowEdits_.emplace(key, WindowEdit{ an.minValue, an.maxValue, an.minValue, an.maxValue }).first;
    an.minValue = std::clamp(mn, 0.0f, 1.0f);
    an.maxValue = std::clamp(mx, 0.0f, 1.0f);
    it->second.min1 = an.minValue;
    it->second.max1 = an.maxValue;
    if (it->second.min1 == it->second.min0 && it->second.max1 == it->second.max0) windowEdits_.erase(it);
    poseChanged_ = true;
}

bool ReloadSync::SaveWindows(bool write, std::string& saved)
{
    windowDiff_.clear();
    if (windowEdits_.empty()) { weaponMsg_ = "no window changes"; return false; }
    // one model.cfg per defining file
    std::map<int, std::vector<const CfgAnimation*>> byFile;
    for (const auto& a : cfgAnims_)
        if (windowEdits_.count(ToLower(a.name))) byFile[a.file].push_back(&a);
    bool ok = true;
    saved.clear();
    for (const auto& [file, anims] : byFile)
    {
        const fs::path path = cfg_.FileOf(file);
        ConfigDoc doc;
        std::string err;
        if (!doc.Load(path, err)) { weaponMsg_ = err; return false; }
        for (const CfgAnimation* a : anims)
        {
            std::string cls;
            if (!ClassPathAt(cfg_.Root(), a->file, a->line, "", cls)) { weaponMsg_ = a->name + ": class not found in " + PathToUtf8(path); return false; }
            if (!doc.SetEntry(cls, "minValue", Num(a->minValue), false, err) || !doc.SetEntry(cls, "maxValue", Num(a->maxValue), false, err))
            {
                weaponMsg_ = err;
                return false;
            }
        }
        windowDiff_.push_back("--- " + PathToUtf8(path));
        for (const auto& d : doc.Diff(1)) windowDiff_.push_back(std::format("{} {}", d.kind, d.text));
        if (write)
        {
            if (!doc.Save(err)) { weaponMsg_ = err; ok = false; break; }
            saved += std::format("{} (backup {}) ", PathToUtf8(path.filename()), PathToUtf8(doc.LastBackup().filename()));
        }
    }
    if (write && ok) windowEdits_.clear();
    return ok;
}

void OpticsFit::ApplySlotItems(bool weaponMode)
{
    const fs::path root = s_.projectRoot.empty() ? fs::path("P:\\") : s_.projectRoot;
    auto& slots = s_.attach.SlotsMut();
    for (auto& s : slots) { s.item.reset(); s.adjust = glm::mat4(1.0f); }
    if (!weaponMode) return; // the chosen optic stays on the weapon in the hands while reloading
    // the weapon's magazine on its magazineslot proxy (CfgMagazines modelSpecial), as the game shows it
    if (!rs_.weapon_.magazineModel.empty())
    {
        fs::path file = ResolveArmaPath(rs_.weapon_.magazineModel, root);
        if (!file.has_extension()) file += ".p3d";
        auto mag = s_.attach.Cache().Get(file, false, root);
        for (auto& s : slots)
            if (s.stem == "magazineslot" && mag && mag->status == AttachedModel::Status::Ok) s.item = mag;
    }
    for (const auto& sc : rs_.weapon_.slots)
    {
        auto it = slotItem_.find(sc.name);
        if (it == slotItem_.end() || it->second.empty()) continue;
        const ItemCfg item = ReadItemConfig(s_.CrewConfig(), it->second);
        if (!item.found || item.model.empty()) continue;
        fs::path file = ResolveArmaPath(item.model, root);
        if (!file.has_extension()) file += ".p3d";
        auto model = s_.attach.Cache().Get(file, false, root);
        const std::string want = NormalizeModelPath(sc.linkProxy);
        for (auto& s : slots)
            if (NormalizeModelPath(s.armaPath) == want)
            {
                s.item = model;
                const fs::file_time_type t = Stamp(file);
                if (itemStamp_[sc.name] != t) // the item model was re-exported: the temporary fit is done
                {
                    if (itemStamp_[sc.name] != fs::file_time_type{}) { fitPos_[sc.name] = glm::vec3(0); fitRot_[sc.name] = glm::vec3(0); }
                    itemStamp_[sc.name] = t;
                }
            }
    }
}

void OpticsFit::ComputeSight(bool charShown)
{
    sight_ = SightNumbers{};
    if (!s_.hasModel) return;
    glm::vec3 a, b;
    if (!MemoryPointOf(s_.model, rs_.weapon_.muzzleEnd, a) || !MemoryPointOf(s_.model, rs_.weapon_.muzzlePos, b))
    {
        sight_.problem = std::format("barrel: memory points '{}' / '{}' not in the Memory LOD", rs_.weapon_.muzzlePos, rs_.weapon_.muzzleEnd);
        return;
    }
    sight_.barrelA = a;
    sight_.barrelB = b;
    const glm::vec3 d = glm::normalize(b - a);
    // sight: the optic on the slot chosen in the panel (its OpticsModes), else the weapon's own (ironsights)
    std::vector<WeaponOptics> modes = rs_.weapon_.optics;
    const ProxySlot* slot = nullptr;
    bool itemModes = false;
    sight_.from = "ironsights (weapon)";
    if (opticsSlot_ >= 0 && opticsSlot_ < int(rs_.weapon_.slots.size()))
    {
        const WeaponSlotCfg& sc = rs_.weapon_.slots[opticsSlot_];
        auto it = slotItem_.find(sc.name);
        if (it != slotItem_.end() && !it->second.empty())
        {
            const ItemCfg item = ReadItemConfig(s_.CrewConfig(), it->second);
            for (const auto& s : s_.attach.Slots())
                if (s.item && NormalizeModelPath(s.armaPath) == NormalizeModelPath(sc.linkProxy)) slot = &s;
            if (slot && !item.optics.empty()) { modes = item.optics; sight_.from = item.cls; itemModes = true; }
            else if (slot) sight_.from = item.cls + " (no OpticsModes - weapon's ironsights)";
        }
    }
    sight_.modes = modes;
    if (modes.empty()) { sight_.problem = "no OpticsModes / memoryPointCamera"; return; }
    const int mi = std::clamp(opticsMode_, 0, int(modes.size()) - 1);
    const WeaponOptics& om = modes[mi];
    sight_.zoomInit = om.zoomInit;
    sight_.zoomMin = om.zoomMin;
    sight_.zoomMax = om.zoomMax;
    const bool onItem = itemModes;
    glm::vec3 e, l;
    const P3DModel& src = onItem ? slot->item->model : s_.model;
    const glm::mat4 xf = onItem ? slot->adjust * slot->xf : glm::mat4(1.0f);
    if (!MemoryPointOf(src, om.cameraPoint, e))
    {
        sight_.problem = std::format("memoryPointCamera '{}' not in the Memory LOD of {}", om.cameraPoint, onItem ? PathToUtf8(slot->item->file.filename()) : "the weapon");
        return;
    }
    e = glm::vec3(xf * glm::vec4(e, 1.0f));
    // view direction: memoryPointCamera -> cameraDir; without cameraDir the sight looks along the barrel
    glm::vec3 dir = d;
    if (!om.cameraDir.empty() && MemoryPointOf(src, om.cameraDir, l)) dir = glm::normalize(glm::vec3(xf * glm::vec4(l, 1.0f)) - e);
    else sight_.note = om.cameraDir.empty() ? "no cameraDir: the view runs parallel to the barrel" : std::format("cameraDir '{}' not found: parallel to the barrel", om.cameraDir);
    sight_.ok = true;
    sight_.camera = e;
    sight_.dir = dir;
    // numbers in the barrel frame: forward = muzzle direction, up = model +y without its forward part, side = up x forward
    const glm::vec3 up = glm::length(glm::vec3(0, 1, 0) - d * d.y) > 1e-4f ? glm::normalize(glm::vec3(0, 1, 0) - d * d.y) : glm::vec3(0, 0, 1);
    const glm::vec3 side = glm::normalize(glm::cross(up, d));
    const glm::vec3 v = e - a;
    const glm::vec3 perp = v - d * glm::dot(v, d);
    sight_.offsetMm = glm::vec3(glm::dot(perp, side), glm::dot(perp, up), glm::dot(v, d)) * 1000.0f; // side, height, along the bore from the chamber point
    sight_.heightMm = glm::dot(perp, up) * 1000.0f;
    sight_.angleDeg = std::acos(std::clamp(glm::dot(dir, d), -1.0f, 1.0f)) * kDeg;
    sight_.elevDeg = std::atan2(glm::dot(dir, up), glm::dot(dir, d)) * kDeg;
    sight_.windDeg = std::atan2(glm::dot(dir, side), glm::dot(dir, d)) * kDeg;
    // zero: where the sight line comes down to the bore line (vertical plane)
    const float dy = sight_.elevDeg / kDeg;
    sight_.zeroM = dy < -1e-6f ? sight_.heightMm * 0.001f / std::tan(-dy) : -1.0f;
    // the character's eye (memory point "pilot" on the head bone) in weapon space
    if (charShown && rs_.wchar_.model && !rs_.wchar_.bones.empty())
    {
        glm::vec3 p;
        const int head = rs_.wchar_.model->skeleton.Find("head");
        if (head >= 0 && head < int(rs_.wchar_.bones.size()) && (MemoryPointOf(rs_.wchar_.model->model, "pilot", p) || MemoryPointOf(rs_.wchar_.model->model, "eye", p)))
        {
            sight_.eye = glm::vec3(rs_.wchar_.toWeapon * rs_.wchar_.bones[head] * glm::vec4(p, 1.0f));
            sight_.eyeOk = true;
            sight_.eyeDistMm = glm::length(sight_.eye - e) * 1000.0f;
        }
    }
}
