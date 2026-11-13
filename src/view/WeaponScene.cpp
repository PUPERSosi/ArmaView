// Weapon modes: (A) Reload sync - the reload gesture on a character holding the weapon, one timeline for the gesture
// and the weapon's reload source, min/max windows of the animations driven by it (edited live, saved to model.cfg),
// a touch helper (hand bone to weapon part distance over time); (B) Attachments / optics - items on the weapon slot
// proxies, barrel / sight axes and their numbers, the view through the sight, a temporary fit with the correction to
// make in the P3D.
// Scene: the weapon is the main model (its model.cfg animations, memory points, validation, hot reload); the character
// is drawn in weapon space: inverse of its weapon (pistol) proxy matrix, posed by its own animation stack.
#include "WeaponScene.h"
#include "Lang.h"

#include "Log.h"
#include "Perf.h"
#include "Weapon.h"

#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <sstream>

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

glm::vec3 Project(const glm::mat4& vp, const glm::vec3& p, float ox, float oy, float w, float h, bool& front)
{
    glm::vec4 c = vp * glm::vec4(p, 1.0f);
    front = c.w > 1e-5f;
    if (!front) return glm::vec3(0);
    glm::vec3 n = glm::vec3(c) / c.w;
    return glm::vec3(ox + (n.x * 0.5f + 0.5f) * w, oy + (0.5f - n.y * 0.5f) * h, n.z);
}

void AddLine(std::vector<LineVertex>& v, glm::vec3 a, glm::vec3 b, glm::vec4 c)
{
    v.push_back({ a, c });
    v.push_back({ b, c });
}

void AddCross(std::vector<LineVertex>& v, glm::vec3 p, float s, glm::vec4 c)
{
    AddLine(v, p - glm::vec3(s, 0, 0), p + glm::vec3(s, 0, 0), c);
    AddLine(v, p - glm::vec3(0, s, 0), p + glm::vec3(0, s, 0), c);
    AddLine(v, p - glm::vec3(0, 0, s), p + glm::vec3(0, 0, s), c);
}

} // namespace

// ============================================================================================ scene









void WeaponScene::ComparePose(double t)
{
    if (!reload_.cmp_.model || s_.attach.extras.empty()) return;
    const float ph = std::clamp(float(reload_.reloadK_ * t / std::max(0.05f, reload_.cmp_.duration)), 0.0f, 1.0f);
    for (auto& s : reload_.cmp_.sources)
        if (IEquals(s.name, reload_.reloadSource_)) s.value = ph;
        else if (IEquals(s.name, "hasmagazine")) s.value = 1.0f; // a loaded weapon
    std::vector<glm::mat4> bones, pm;
    EvaluateCfgAnimations(reload_.cmp_.skel, reload_.cmp_.anims, reload_.cmp_.sources, bones);
    SkinMatrices(reload_.cmp_.skin, bones, pm);
    auto& posed = s_.attach.extras.back().posed;
    SkinModelVerts(*reload_.cmp_.model, pm, posed);
    const glm::vec3 off(0.0f, -0.3f, 0.0f); // below our weapon
    for (auto& v : posed) v.pos += off;
    reload_.cmp_.shownT = t;
    host_.PoseChanged();
}

void WeaponScene::Update(float dt)
{
    const bool on = Active() && s_.hasModel;
    if (!on)
    {
        if (cameraSaved_) { h_.SceneCamera() = savedCamera_; cameraSaved_ = false; }
        if (weaponCharShown_ || !s_.attach.extras.empty())
        {
            s_.attach.extras.clear();
            weaponCharShown_ = false;
            ApplySlotItems();
            if (s_.hasModel) host_.RebuildGeometry();
        }
        if (weaponLinesShown_) { h_.SceneRenderer().SetLines(LineLayer::Weapon, {}); weaponLinesShown_ = false; }
        return;
    }
    WeaponEnsure();
    // hot reload: gesture / handAnim / stance RTM (e.g. from Blender) keeps the timeline position
    weaponPoll_ += dt;
    if (weaponPoll_ > 0.5)
    {
        weaponPoll_ = 0;
        const fs::path files[3] = { reload_.wchar_.baseFile, reload_.wchar_.handFile, reload_.wchar_.gestureFile };
        bool changed = false;
        for (int i = 0; i < 3; ++i)
            if (!files[i].empty() && Stamp(files[i]) != reload_.wchar_.stamp[i]) changed = true;
        if (changed)
        {
            const double keep = reload_.time;
            WeaponCharLoad();
            reload_.time = keep;
            Log::Get().Info("Weapon: RTM reloaded, timeline kept at {:.3f} s", reload_.time);
        }
        if (Stamp(s_.model.path) != reload_.weaponStamp_) // weapon re-exported: fits are done
        {
            reload_.weaponStamp_ = Stamp(s_.model.path);
            optics_.fitPos_.clear();
            optics_.fitRot_.clear();
        }
    }
    // the character (Reload: always; Optics: in the aiming pose when asked) and the comparison weapon (Reload)
    const bool wantChar = reload_.wchar_.model && reload_.wchar_.proxyOk && (h_.Mode() == UiMode::Reload || optics_.aimPose);
    const bool wantCmp = h_.Mode() == UiMode::Reload && reload_.cmp_.model && reload_.cmp_.model->status == AttachedModel::Status::Ok;
    if (wantChar != weaponCharShown_ || wantCmp != cmpShown_)
    {
        s_.attach.extras.clear();
        if (wantChar) s_.attach.extras.push_back({ reload_.wchar_.model, {} });
        if (wantCmp) s_.attach.extras.push_back({ reload_.cmp_.model, {} });
        weaponCharShown_ = wantChar;
        cmpShown_ = wantCmp;
        ApplySlotItems();
        host_.RebuildGeometry();
        weaponCharT_ = -1;
        reload_.cmp_.shownT = -1;
    }
    if (h_.Mode() == UiMode::Reload)
    {
        const float T = reload_.ReloadDuration() / std::max(0.01f, reload_.reloadK_);
        if (reload_.playing)
        {
            reload_.time += double(dt) * reload_.speed;
            if (reload_.time > T)
            {
                if (reload_.loop) reload_.time = std::fmod(reload_.time, double(T));
                else { reload_.time = T; reload_.playing = false; }
            }
        }
        reload_.time = std::clamp(reload_.time, 0.0, double(std::max(T, reload_.ReloadDuration())));
        const float ph = reload_.ReloadPhase(reload_.time);
        for (auto& s : s_.cfgSources)
            if (IEquals(s.name, reload_.reloadSource_) && s.value != ph) { s.value = ph; s.animate = false; host_.PoseChanged(); }
    }
    if (const int sig = s_.blendMode * 2 + (reload_.gestureFade_ ? 1 : 0); sig != weaponBlendShown_) { weaponBlendShown_ = sig; weaponCharT_ = -1; }
    if (wantCmp && reload_.time != reload_.cmp_.shownT) ComparePose(reload_.time);
    if (wantChar && !s_.attach.extras.empty())
    {
        const double t = h_.Mode() == UiMode::Reload ? reload_.time : 0.0;
        if (t != weaponCharT_)
        {
            glm::mat4 tw(1.0f);
            if (reload_.WeaponCharPose(t, h_.Mode() == UiMode::Reload, reload_.wchar_.bones, tw, &s_.attach.extras[0].posed)) reload_.wchar_.toWeapon = tw;
            weaponCharT_ = t;
            host_.PoseChanged();
        }
    }
    // optics: temporary fit of the slot items (weapon axes: mm, degrees about the proxy origin)
    if (h_.Mode() == UiMode::Optics)
    {
        for (auto& s : s_.attach.SlotsMut())
        {
            if (!s.item) continue;
            for (const auto& sc : s_.weapon.slots)
                if (NormalizeModelPath(sc.linkProxy) == NormalizeModelPath(s.armaPath))
                {
                    const glm::vec3 pos = optics_.fitPos_[sc.name] * 0.001f, rot = optics_.fitRot_[sc.name] / kDeg;
                    const glm::vec3 o(s.xf[3]);
                    const glm::mat4 r = glm::eulerAngleYXZ(rot.y, rot.x, rot.z);
                    const glm::mat4 adj = glm::translate(glm::mat4(1.0f), pos + o) * r * glm::translate(glm::mat4(1.0f), -o);
                    if (adj != s.adjust) { s.adjust = adj; host_.PoseChanged(); }
                }
        }
        optics_.ComputeSight(weaponCharShown_);
        // view through the sight / from the character's eye
        if ((optics_.throughSight || optics_.eyeView) && optics_.sight_.ok)
        {
            if (!cameraSaved_) { savedCamera_ = h_.SceneCamera(); cameraSaved_ = true; }
            const glm::vec3 from = optics_.eyeView && optics_.sight_.eyeOk ? optics_.sight_.eye : optics_.sight_.camera;
            const glm::vec3 d = glm::normalize(optics_.sight_.dir);
            h_.SceneCamera().distance = 0.05f;
            h_.SceneCamera().target = from + d * h_.SceneCamera().distance;
            h_.SceneCamera().pitch = std::asin(std::clamp(-d.y, -1.0f, 1.0f));
            h_.SceneCamera().yaw = std::atan2(-d.x, -d.z);
            const float zoom = optics_.zoom > 0 ? optics_.zoom : optics_.sight_.zoomInit;
            h_.SceneCamera().fovY = 2.0f * std::atan(std::max(0.001f, zoom));
            h_.SceneCamera().nearZ = 0.002f;
            h_.SceneCamera().farZ = 3000.0f;
        }
        else if (cameraSaved_)
        {
            h_.SceneCamera() = savedCamera_;
            cameraSaved_ = false;
        }
    }
    else if (cameraSaved_)
    {
        h_.SceneCamera() = savedCamera_;
        cameraSaved_ = false;
    }
    WeaponLines();
}








void WeaponScene::WeaponLines()
{
    std::vector<LineVertex> v;
    const glm::vec4 red(1.0f, 0.3f, 0.25f, 1), green(0.3f, 1.0f, 0.4f, 1), yellow(1.0f, 0.85f, 0.2f, 1), cyan(0.3f, 0.9f, 1.0f, 1);
    if (h_.Mode() == UiMode::Reload && reload_.touchBone_ >= 0 && reload_.touchSel_ >= 0 && reload_.wchar_.model && !reload_.wchar_.bones.empty())
    {
        const glm::vec3 j = reload_.CharJoint(reload_.touchBone_, reload_.wchar_.bones, reload_.wchar_.toWeapon);
        const glm::vec3 c = reload_.SelectionCentroidAt(reload_.touchSel_, reload_.ReloadPhase(reload_.time));
        AddLine(v, j, c, yellow);
        AddCross(v, j, 0.01f, yellow);
        AddCross(v, c, 0.01f, cyan);
    }
    if (h_.Mode() == UiMode::Optics && !optics_.throughSight && !optics_.eyeView)
    {
        if (optics_.sight_.barrelA != optics_.sight_.barrelB)
        {
            const glm::vec3 d = glm::normalize(optics_.sight_.barrelB - optics_.sight_.barrelA);
            AddLine(v, optics_.sight_.barrelA - d * 0.1f, optics_.sight_.barrelB + d * 0.6f, red);
            AddCross(v, optics_.sight_.barrelB, 0.006f, red);
        }
        if (optics_.sight_.ok)
        {
            AddLine(v, optics_.sight_.camera - optics_.sight_.dir * 0.08f, optics_.sight_.camera + optics_.sight_.dir * 0.8f, green);
            AddCross(v, optics_.sight_.camera, 0.006f, green);
            if (optics_.sight_.eyeOk)
            {
                AddCross(v, optics_.sight_.eye, 0.008f, cyan);
                AddLine(v, optics_.sight_.eye, optics_.sight_.camera, cyan);
            }
        }
    }
    h_.SceneRenderer().SetLines(LineLayer::Weapon, v);
    weaponLinesShown_ = !v.empty();
}

void WeaponScene::Ghosts(std::vector<MeshVertex>& gv, std::vector<Renderer::Ghost>& ghosts)
{
    if (h_.Mode() != UiMode::Reload || !animView_.reloadOnion || !s_.hasModel) return;
    const float T = reload_.ReloadDuration() / std::max(0.01f, reload_.reloadK_);
    for (int side : { -1, 1 })
    {
        const double t = reload_.time + side * double(animView_.reloadOnionDt);
        if (t < 0 || t > T) continue;
        const glm::vec4 col = side < 0 ? glm::vec4(0.35f, 0.6f, 1.0f, 0.3f) : glm::vec4(1.0f, 0.55f, 0.25f, 0.3f);
        // weapon at that reload phase
        if (!s_.skeleton.Empty() && !h_.Skin().offset.empty())
        {
            std::vector<CfgSource> src = s_.cfgSources;
            for (auto& s : src)
                if (IEquals(s.name, reload_.reloadSource_)) s.value = reload_.ReloadPhase(t);
            std::vector<glm::mat4> mats, pm;
            EvaluateCfgAnimations(s_.skeleton, s_.cfgAnims, src, mats);
            SkinMatrices(h_.Skin(), mats, pm);
            Renderer::Ghost g;
            g.first = UINT(gv.size());
            for (size_t i = 0; i < h_.AttachFirst() && i < h_.BaseVerts().size(); ++i)
            {
                MeshVertex x = h_.BaseVerts()[i];
                const uint32_t p = h_.VertPoint()[i];
                if (p < pm.size())
                {
                    x.pos = glm::vec3(pm[p] * glm::vec4(x.pos, 1.0f));
                    x.normal = glm::mat3(pm[p]) * x.normal;
                }
                gv.push_back(x);
            }
            g.count = UINT(gv.size()) - g.first;
            g.color = col;
            ghosts.push_back(g);
        }
        // character
        std::vector<glm::mat4> bones;
        std::vector<MeshVertex> cv;
        glm::mat4 tw(1.0f);
        if (weaponCharShown_ && reload_.WeaponCharPose(t, true, bones, tw, &cv))
        {
            Renderer::Ghost g;
            g.first = UINT(gv.size());
            gv.insert(gv.end(), cv.begin(), cv.end());
            g.count = UINT(cv.size());
            g.color = col;
            ghosts.push_back(g);
        }
    }
}





void WeaponScene::DrawOverlay()
{
    float sceneX_ = 0, sceneY_ = 0, sceneW_ = 1, sceneH_ = 1;
    h_.SceneRect(sceneX_, sceneY_, sceneW_, sceneH_);
    if (!Active() || !s_.hasModel) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 origin = ImGui::GetMainViewport()->Pos;
    const float ox = origin.x + sceneX_, oy = origin.y + sceneY_;
    if (h_.Mode() == UiMode::Optics && (optics_.throughSight || optics_.eyeView) && optics_.sight_.ok)
    {
        const ImVec2 c(ox + sceneW_ * 0.5f, oy + sceneH_ * 0.5f);
        const ImU32 col = IM_COL32(255, 40, 40, 230);
        const float s = 18 * host_.Dpi(), gap = 4 * host_.Dpi();
        dl->AddLine(ImVec2(c.x - s, c.y), ImVec2(c.x - gap, c.y), col, 1.5f);
        dl->AddLine(ImVec2(c.x + gap, c.y), ImVec2(c.x + s, c.y), col, 1.5f);
        dl->AddLine(ImVec2(c.x, c.y - s), ImVec2(c.x, c.y - gap), col, 1.5f);
        dl->AddLine(ImVec2(c.x, c.y + gap), ImVec2(c.x, c.y + s), col, 1.5f);
        dl->AddText(ImVec2(ox + 10 * host_.Dpi(), oy + 10 * host_.Dpi()), IM_COL32(255, 255, 255, 230),
                    std::format("{} - {}  FOV {:.1f} deg", optics_.eyeView ? "character's eye" : "sight camera", optics_.sight_.from,
                                h_.SceneCamera().fovY * kDeg).c_str());
        // where the barrel points (muzzle direction far away)
        if (optics_.sight_.barrelA != optics_.sight_.barrelB)
        {
            const glm::vec3 d = glm::normalize(optics_.sight_.barrelB - optics_.sight_.barrelA);
            bool front = false;
            const glm::vec3 p = Project(h_.ViewProj(), optics_.sight_.barrelB + d * 100.0f, ox, oy, sceneW_, sceneH_, front);
            if (front) dl->AddCircle(ImVec2(p.x, p.y), 6 * host_.Dpi(), IM_COL32(255, 200, 60, 230), 0, 1.5f);
        }
    }
}

// ============================================================================================ ui-script

std::string WeaponScene::Command(const std::string& line)
{
    // weapon mode reload|optics | time <s> | play on|off | k <v> | onion on|off [dt] | window <anim> <min> <max> |
    // touch <bone> <selection> | suggest <anim> <cm> | diff | save | item <slot> <class|none> | sight <slot> [mode] |
    // fit <slot> x y z rx ry rz | scope on|off | aim on|off | eye on|off | report
    std::istringstream is(line);
    std::string op;
    is >> op;
    auto onOff = [&]() { std::string v; is >> v; return v == "on" || v == "1"; };
    if (op == "mode")
    {
        std::string m;
        is >> m;
        h_.SetMode(m == "optics" ? UiMode::Optics : UiMode::Reload);
        Update(0.0f);
        return std::format("{} - {}", UiShell::ModeName(h_.Mode()), s_.weapon.found ? s_.weapon.cls : "no weapon class");
    }
    WeaponEnsure();
    if (op == "blend") { std::string m; is >> m; s_.blendMode = m == "parent" ? 1 : 0; Update(0.0f); return std::string(s_.blendMode ? "parent-relative" : "model space"); }
    if (op == "fade") { reload_.gestureFade_ = onOff(); Update(0.0f); return std::string("ok"); }
    if (op == "ik") { reload_.handIK_ = onOff(); weaponCharT_ = -1; Update(0.0f); return std::format("hand IK {}", reload_.handIK_ ? "on" : "off"); }
    if (op == "cfg") // cfg set <Cls/Sub> <entry> <value...> | cfg diff | cfg save: the config.cpp of the weapon class (own files only)
    {
        std::string sub, err;
        is >> sub;
        if (sub == "target") // cfg target config|model: the addon's config.cpp or the weapon's model.cfg
        {
            std::string t;
            is >> t;
            weaponCfgModel_ = t == "model";
            weaponCfgDoc_ = ConfigDoc{};
            return std::string(weaponCfgModel_ ? "model.cfg" : "config.cpp");
        }
        const fs::path file = weaponCfgModel_ ? s_.modelCfg : s_.weapon.ref.file;
        if (file.empty()) return std::string(weaponCfgModel_ ? "no model.cfg" : "no weapon class");
        // own files only (game configs are read only)
        if (!weaponCfgDoc_.Loaded() || weaponCfgDoc_.File() != file)
        {
            if (ToLower(PathToUtf8(file)).find("\\a3\\") != std::string::npos) return std::string("game config - read only");
            if (!weaponCfgDoc_.Load(file, err)) return err;
        }
        if (sub == "restore") // cfg restore <Cls/Sub> <entry> <file>: the entry as written in another file (e.g. the .bak)
        {
            std::string path, entry, from, text;
            is >> path >> entry;
            std::getline(is, from);
            from.erase(0, from.find_first_not_of(' '));
            ConfigDoc other;
            if (!other.Load(fs::path(Utf8ToWide(from)), err)) return err;
            if (!other.EntryText(path, entry, text)) return std::string("entry not in ") + from;
            if (!weaponCfgDoc_.ReplaceEntryText(path, entry, text, err)) return err;
            return std::string("ok");
        }
        if (sub == "set")
        {
            std::string path, entry, value;
            is >> path >> entry;
            std::getline(is, value);
            value.erase(0, value.find_first_not_of(' '));
            if (!weaponCfgDoc_.SetEntry(path, entry, value, value.starts_with("{"), err)) return err;
            return std::string("ok");
        }
        if (sub == "diff")
        {
            std::string out;
            for (const auto& d : weaponCfgDoc_.Diff(1)) out += std::format("\n    {} {}", d.kind, d.text);
            return out.empty() ? std::string("no changes") : out;
        }
        if (sub == "save")
        {
            if (!weaponCfgDoc_.Save(err)) return err;
            const std::string backup = PathToUtf8(weaponCfgDoc_.LastBackup().filename());
            weaponCfgDoc_ = ConfigDoc{};
            h_.ReloadModel(); // config.cpp of the addon read again
            reload_.weaponKey_.clear();
            Update(0.0f);
            const std::string v = h_.ValidationSummary("weapon config");
            const size_t r = v.rfind("RESULT");
            return std::format("saved {} (backup {}) - {}", PathToUtf8(file.filename()), backup, r == std::string::npos ? v : v.substr(r));
        }
        return "unknown cfg command " + sub;
    }
    if (op == "compare") // compare <CfgWeapons class | .p3d> | compare off
    {
        std::string a;
        std::getline(is, a);
        a.erase(0, a.find_first_not_of(' '));
        CompareLoad(a == "off" ? std::string() : a);
        Update(0.0f);
        return reload_.weaponMsg_;
    }
    if (op == "time") { is >> reload_.time; reload_.playing = false; return std::format("t {:.3f} phase {:.3f}", reload_.time, reload_.ReloadPhase(reload_.time)); }
    if (op == "play") { reload_.playing = onOff(); return std::string("ok"); }
    if (op == "k") { is >> reload_.reloadK_; return std::format("k {}", reload_.reloadK_); }
    if (op == "onion") { animView_.reloadOnion = onOff(); float d = 0; if (is >> d) animView_.reloadOnionDt = d; host_.PoseChanged(); return std::string("ok"); }
    if (op == "window" || op == "suggest" || op == "touch")
    {
        auto animIndex = [&](const std::string& n) { for (int i = 0; i < int(s_.cfgAnims.size()); ++i) if (IEquals(s_.cfgAnims[i].name, n)) return i; return -1; };
        if (op == "window")
        {
            std::string n;
            float a = 0, b = 1;
            is >> n >> a >> b;
            const int i = animIndex(n);
            if (i < 0) return "no animation " + n;
            SetWindow(i, a, b);
            return std::format("{} {}..{}", n, Num(a), Num(b));
        }
        if (op == "touch")
        {
            std::string bone, sel;
            is >> bone >> sel;
            reload_.touchBone_ = reload_.wchar_.model ? reload_.wchar_.model->skeleton.Find(bone) : -1;
            reload_.touchSel_ = -1;
            const P3DLod& l = s_.model.lods[s_.lod];
            for (int s = 0; s < int(l.selections.size()); ++s)
                if (IEquals(l.selections[s].name, sel)) reload_.touchSel_ = s;
            ComputeTouch();
            if (reload_.touchDist_.empty()) return std::string("touch: bone / selection not found or no pose");
            return std::format("closest {:.1f} cm at {:.2f} s (phase {:.3f})", reload_.touchMin_ * 100, reload_.touchMinT_, reload_.ReloadPhase(reload_.touchMinT_));
        }
        std::string n;
        is >> n >> reload_.touchThresholdCm_;
        reload_.touchAnim_ = animIndex(n);
        reload_.SuggestWindow();
        return reload_.weaponMsg_;
    }
    if (op == "diff")
    {
        SaveWindows(false);
        std::string out;
        for (const auto& l : reload_.windowDiff_) out += "\n    " + l;
        return out.empty() ? reload_.weaponMsg_ : out;
    }
    if (op == "save") { SaveWindows(true); return reload_.weaponMsg_; }
    if (op == "item")
    {
        std::string slot, cls;
        is >> slot >> cls;
        optics_.slotItem_[slot] = cls == "none" ? std::string() : cls;
        for (int i = 0; i < int(s_.weapon.slots.size()); ++i)
            if (IEquals(s_.weapon.slots[i].name, slot) && cls != "none" && optics_.opticsSlot_ < 0) optics_.opticsSlot_ = i;
        ApplySlotItems();
        host_.RebuildGeometry();
        host_.PoseChanged();
        for (const auto& s : s_.attach.Slots())
            if (s.item) return std::format("{} -> {} ({})", slot, cls, s.item->status == AttachedModel::Status::Ok ? "loaded" : s.item->message);
        return std::format("{} -> {}", slot, cls);
    }
    if (op == "sight")
    {
        std::string slot;
        is >> slot;
        optics_.opticsSlot_ = -1;
        for (int i = 0; i < int(s_.weapon.slots.size()); ++i)
            if (IEquals(s_.weapon.slots[i].name, slot)) optics_.opticsSlot_ = i;
        is >> optics_.opticsMode_;
        return std::format("sight slot {}", optics_.opticsSlot_);
    }
    if (op == "fit")
    {
        std::string slot;
        glm::vec3 p(0), r(0);
        is >> slot >> p.x >> p.y >> p.z >> r.x >> r.y >> r.z;
        optics_.fitPos_[slot] = p;
        optics_.fitRot_[slot] = r;
        return std::string("ok");
    }
    if (op == "layers") // layers <stance 0/1> <handAnim 0/1> <gesture 0/1>: diagnostics of the character's stack
    {
        int b = 1, h = 1, g = 1;
        is >> b >> h >> g;
        if (reload_.wchar_.stack.layers.size() >= 3)
        {
            reload_.wchar_.stack.layers[0].enabled = b && reload_.wchar_.base;
            reload_.wchar_.stack.layers[1].enabled = h && reload_.wchar_.hand;
            reload_.wchar_.stack.layers[2].enabled = g && reload_.wchar_.gesture;
        }
        weaponCharT_ = -1;
        Update(0.0f);
        return std::format("layers stance {} hand {} gesture {}", b, h, g);
    }
    if (op == "scope") { optics_.throughSight = onOff(); if (optics_.throughSight) optics_.eyeView = false; return std::string("ok"); }
    if (op == "aim") { optics_.aimPose = onOff(); return std::string("ok"); }
    if (op == "eye") { optics_.eyeView = onOff(); if (optics_.eyeView) optics_.throughSight = false; return std::string("ok"); }
    if (op == "report")
    {
        Update(0.0f);
        std::string s = std::format("{} ({}), reloadAction {} -> {} {:.3f} s mask {}, handAnim {}, magazineReloadTime {}, switchPhase {}{}",
                                    s_.weapon.cls, s_.weapon.ref.Where(), s_.weapon.reloadAction, PathToUtf8(reload_.wchar_.gestureFile.filename()), reload_.wchar_.gestureDuration,
                                    reload_.wchar_.gestureMask, s_.weapon.handAnim, s_.weapon.magazineReloadTime, s_.weapon.switchPhase,
                                    reload_.wchar_.problem.empty() ? "" : " PROBLEM " + reload_.wchar_.problem);
        s += std::format("\n    reload windows ({}):", reload_.reloadSource_);
        for (int i : reload_.reloadAnims_) s += std::format(" {} {}..{};", s_.cfgAnims[i].name, Num(s_.cfgAnims[i].minValue), Num(s_.cfgAnims[i].maxValue));
        s += std::format("\n    magazine {} -> {}", s_.weapon.magazine.empty() ? "-" : s_.weapon.magazine, s_.weapon.magazineModel.empty() ? "-" : s_.weapon.magazineModel);
        for (const auto& sl : s_.attach.Slots())
            if (sl.stem == "magazineslot") s += std::format(" (slot {}: item {}, valid {})", sl.selection, sl.item ? PathToUtf8(sl.item->file.filename()) : "-", sl.valid);
        s += "\n    slots:";
        for (const auto& sc : s_.weapon.slots) s += std::format(" {} [{}] {} items;", sc.name, sc.linkProxy, sc.items.size());
        if (optics_.sight_.ok)
            s += std::format("\n    sight {}: side {:+.1f} up {:+.1f} along {:.1f} mm, height {:.1f} mm, angle {:.3f} deg (elev {:.3f}, wind {:.3f}){}", optics_.sight_.from,
                             optics_.sight_.offsetMm.x, optics_.sight_.offsetMm.y, optics_.sight_.offsetMm.z, optics_.sight_.heightMm, optics_.sight_.angleDeg, optics_.sight_.elevDeg, optics_.sight_.windDeg,
                             optics_.sight_.eyeOk ? std::format(", eye {:.0f} mm", optics_.sight_.eyeDistMm) : "") +
                 std::format("\n    points: barrel {} ({:.3f} {:.3f} {:.3f}) -> {} ({:.3f} {:.3f} {:.3f}), sight ({:.3f} {:.3f} {:.3f}) dir ({:.3f} {:.3f} {:.3f}){}",
                             s_.weapon.muzzleEnd, optics_.sight_.barrelA.x, optics_.sight_.barrelA.y, optics_.sight_.barrelA.z, s_.weapon.muzzlePos, optics_.sight_.barrelB.x, optics_.sight_.barrelB.y,
                             optics_.sight_.barrelB.z, optics_.sight_.camera.x, optics_.sight_.camera.y, optics_.sight_.camera.z, optics_.sight_.dir.x, optics_.sight_.dir.y, optics_.sight_.dir.z,
                             optics_.sight_.note.empty() ? "" : " - " + optics_.sight_.note);
        else if (!optics_.sight_.problem.empty()) s += "\n    sight: " + optics_.sight_.problem;
        // where the character stands in weapon space (pelvis, head, hands at the current time)
        if (reload_.wchar_.model && !reload_.wchar_.bones.empty())
        {
            if (optics_.sight_.barrelA != optics_.sight_.barrelB)
            {
                const glm::vec3 bd = glm::normalize(glm::mat3(glm::inverse(reload_.wchar_.toWeapon)) * glm::normalize(optics_.sight_.barrelB - optics_.sight_.barrelA));
                s += std::format("\n    barrel in character space ({:.3f} {:.3f} {:.3f}): pitch {:.1f} deg", bd.x, bd.y, bd.z,
                                 std::asin(std::clamp(bd.y, -1.0f, 1.0f)) * kDeg);
            }
            s += std::format("\n    hand IK {}: left {:.2f}, right {:.2f} (curves: {} / {} points)", reload_.handIK_ ? "on" : "off", reload_.handIKWeight_[0], reload_.handIKWeight_[1],
                             reload_.wchar_.leftIK.size() / 2, reload_.wchar_.rightIK.size() / 2);
            s += "\n    character (weapon space):";
            for (const char* b : { "pelvis", "head", "lefthand", "righthand" })
            {
                const glm::vec3 p = reload_.CharJoint(reload_.wchar_.model->skeleton.Find(b), reload_.wchar_.bones, reload_.wchar_.toWeapon);
                s += std::format(" {} ({:.2f} {:.2f} {:.2f});", b, p.x, p.y, p.z);
            }
        }
        return s;
    }
    return "unknown weapon command " + op;
}

// ============================================================================================ core wrappers (ReloadSync / OpticsFit)
// The logic is in the core; these add what the view does around it (the shown character, geometry, model reload).


void WeaponScene::WeaponCharLoad()
{
    // the shown character (geometry, pose cache) belongs to the old load: rebuilt by UpdateWeaponModes
    weaponCharT_ = -1;
    if (weaponCharShown_)
    {
        s_.attach.extras.clear();
        weaponCharShown_ = false;
    }
    reload_.WeaponCharLoad();
}

void WeaponScene::WeaponEnsure()
{
    bool reloaded;
    {
        PerfNoteScope ps("weapon: config + character with the reload gesture");
        reloaded = reload_.WeaponEnsure();
    }
    if (reloaded)
    {
        if (s_.weapon.found && optics_.opticsMode_ >= int(s_.weapon.optics.size())) optics_.opticsMode_ = 0;
        weaponCharT_ = -1;
        if (weaponCharShown_) { s_.attach.extras.clear(); weaponCharShown_ = false; }
        if (s_.hasModel)
        {
            h_.RebuildAttachments(); // slot items need the weapon's slots
            host_.RebuildGeometry();
        }
    }
    if (reload_.TakePoseChanged()) host_.PoseChanged();
}

bool WeaponScene::CompareLoad(const std::string& input)
{
    cmpShown_ = false;
    return reload_.CompareLoad(input);
}

void WeaponScene::ComputeTouch()
{
    reload_.ComputeTouch();
    weaponCharT_ = -1; // pose the shown character again
}

void WeaponScene::SetWindow(int a, float mn, float mx)
{
    reload_.SetWindow(a, mn, mx);
    if (reload_.TakePoseChanged()) host_.PoseChanged();
}

bool WeaponScene::SaveWindows(bool write)
{
    std::string saved;
    const bool ok = reload_.SaveWindows(write, saved);
    if (write && ok)
    {
        h_.ReloadModel();
        const std::string v = h_.ValidationSummary("weapon model.cfg");
        reload_.weaponMsg_ = "saved " + saved + "- " + v.substr(v.rfind("RESULT") == std::string::npos ? 0 : v.rfind("RESULT"));
        Log::Get().Info("Reload sync: {}", reload_.weaponMsg_);
    }
    return ok;
}

void WeaponScene::SlotItemsChanged()
{
    ApplySlotItems();
    host_.RebuildGeometry();
    host_.PoseChanged();
}
