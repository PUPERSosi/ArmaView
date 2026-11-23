// Command-line automation, the ui-script host and the --selftest script, ui-script / palette commands, the core test:
// test infrastructure of the viewer, outside App (a friend: it drives the whole viewer).
#include "ViewerAutomation.h"
#include "App.h"
#include "Fuzz.h"
#include "Gltf.h"
#include "Lang.h"
#include "LocalPaths.h"
#include "Log.h"
#include "Perf.h"
#include "PerfMemory.h"
#include "Retarget.h"
#include "RunProgram.h"
#include "RenderSelfTest.h"
#include "SelfTest.h"
#include "ViewCoreTest.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace {
constexpr float kPi = 3.14159265f;


const ImVec4 kErr = { 1.0f, 0.4f, 0.4f, 1 };
const ImVec4 kWarn = { 1.0f, 0.8f, 0.3f, 1 };
const ImVec4 kGood = { 0.45f, 0.85f, 0.45f, 1 };

std::string Quote(const std::string& s) { return MovesEditor::Quote(s); }

} // namespace

void ViewerAutomation::Apply(const Automation& a)
{
    auto_ = a;
    auto& log = Log::Get();
    if (!a.view.empty())
    {
        std::string v = ToLower(a.view);
        if (v == "front") app_.SetView(kPi, 0);
        else if (v == "back") app_.SetView(0, 0);
        else if (v == "left") app_.SetView(kViewLeftYaw, 0);
        else if (v == "right") app_.SetView(kViewRightYaw, 0);
        else if (v == "top") app_.SetView(kPi, 1.55f);
        else log.Warn("--view {}: expected front|back|left|right|top", a.view);
    }
    if (!a.tab.empty())
    {
        const std::string t = ToLower(a.tab);
        app_.SetMode(t == "skeleton" || t == "animation" || t == "sources" ? UiMode::Animation : t == "validation" ? UiMode::Validation
                : t == "material" || t == "materials" ? UiMode::Materials : t == "crew" ? UiMode::Vehicle : UiMode::Model);
    }
    if (!a.normals.empty())
    {
        app_.display_.normalsMode = ToLower(a.normals) == "recomputed" ? NormalsMode::Recomputed : NormalsMode::FromFile;
        app_.scene_.RebuildLodGeometry();
    }
    if (a.skeletonOnly) app_.display_.skeletonOnly = true;
    if (a.graph) app_.showGraph_ = true;
    if (!a.graphSearch.empty()) app_.graph_.SetSearch(a.graphSearch);
    if (!a.uiScript.empty()) script_.Load(a.uiScript);
    if (a.crew) app_.SetMode(UiMode::Vehicle);
    if (a.hideVehicle) { app_.crew_.hideVehicle = true; app_.scene_.BuildDrawSections(); }
    if (a.noCrew) { app_.session_.attach.showCrew = false; if (app_.session_.hasModel) app_.scene_.RebuildLodGeometry(); }
    if (a.noSkeleton) app_.display_.showSkeleton = false;
    if (a.faceDiag >= 0) { app_.display_.faceDiag = FaceDiag(a.faceDiag); app_.scene_.RebuildLodGeometry(); }
    if (a.noSwizzle) { app_.display_.applySwizzle = false; app_.scene_.BuildDrawSections(); }
    if (!a.world.empty()) { if (app_.session_.game) app_.displayPanel_.SetWorld(a.world); else app_.pendingWorld_ = a.world; }
    if (a.damage >= 0) { app_.materials_.damageState = std::clamp(a.damage, 0, 2); app_.scene_.RebuildLodGeometry(); }
    if (a.onion) app_.animView_.onionSkin = true;
    if (a.trail) app_.animView_.showTrail = true;
    if (a.crewPlay) app_.session_.attach.playAll = true;
    if (!a.state.empty()) app_.pendingState_ = a.state; // needs the game configs: loaded in PollGameConfig / now if ready
    if (a.play) app_.playback_.playing = true;
    if (a.reflections >= 0) app_.display_.reflections = a.reflections != 0;
    if (a.shadows >= 0) app_.display_.shadows = a.shadows != 0;
    if (a.shadowQuality >= 0) app_.display_.shadowQuality = std::clamp(a.shadowQuality, 0, 2);
    if (!a.selftest.empty())
    {
        // Regression run on Arma 3 Samples (and synthetic files in coretest): every model must load without Validation
        // errors (an error there = viewer bug). The game data on P: is not used (App::Init skips the game configs).
        const LocalPaths& lp = GetLocalPaths();
        const std::string s = lp.samples.empty() ? std::string() : (fs::path(lp.samples) / "").string();
        std::string script = "wait_config\nwait 5\nperf_notes startup\n";
        if (lp.samples.empty())
        {
            // no Arma 3 Samples (CI, a machine without the game): the parsers and CfgMoves on files written here
            Log::Get().Info("selftest: no samples= in armaview.local.ini - core test only (parsers, CfgMoves, fuzzing)");
            script += "coretest\nfuzz 150\nperf_notes end\nreport selftest.txt\nexit\n";
            fs::path dir = a.selftest.is_relative() ? app_.paths_.Exe() / a.selftest : a.selftest;
            script_.LoadText(script, dir);
            goto selftestLoaded;
        }
        auto model = [&](const std::string& rel, const std::string& name, const std::string& extra) {
            script += "open " + s + rel + "\nwait 20\nperf_notes " + name + "\n" + extra + "perf_begin\nwait 60\nperf_end " + name +
                      " idle\nscreenshot " + name + ".png\nvalidation " + name + "\n";
        };
        // every mode, every docked panel brought to the front, the floating windows open, both languages: the ImGui ID
        // check sees all controls with this model's data (idconflicts at the end: 0 controls sharing an ID)
        auto windowWalk = [&]() {
            static constexpr const char* kModes[][8] = {
                { "Outliner", "Inspector", "Log" }, { "Timeline", "Moves", "Move editor", "Retarget", "Layers", "Sources" },
                { "Crew", "Sources" }, { "Materials" }, { "Validation" }, { "Reload sync" }, { "Optics" },
            };
            script += "show all\nwait 3\n";
            for (int m = 0; m < int(std::size(kModes)); ++m)
            {
                script += std::format("mode {}\nwait 3\n", m);
                for (const char* w : kModes[m])
                    if (w) script += std::format("focus_window {}\nwait 3\n", w);
            }
            script += "lang ru\nwait 3\nlang en\nshow all off\nmode 0\nwait 3\n";
        };
        model("Test_Tank_01\\Test_Tank_01.p3d", "Test_Tank_01", "");
        // a recording stopped by the user: no Error, no partial MP4 / ffmpeg log left (record cancel errs otherwise)
        {
            const fs::path recDir = (a.selftest.is_relative() ? app_.paths_.Exe() / a.selftest : a.selftest) / "record_cancel";
            std::error_code ec;
            fs::remove_all(recDir, ec); // this run's own folder
            script += "record turn mp4 320 240 10 5 dir=" + PathToUtf8(recDir) + "\nwait 8\nrecord cancel\nwait 3\n";
        }
        windowWalk();
        model("Test_Car_01\\Test_Car_01.p3d", "Test_Car_01", "skintest body\nwait 3\n");
        model("Test_Heli_01\\Test_Heli_01.p3d", "Test_Heli_01", "");
        model("Test_Character_01\\A3_character_example.p3d", "A3_character_example", "");
        // the Samples' RTMs as layers on the sample character
        script += "open " + s + "Test_Anims\\AmovPercMstpSrasWrflDnon.rtm\nopen " + s + "Test_Anims\\AmovPpneMstpSrasWrflDnon.rtm\n";
        script += "play\nwait 10\nperf_begin\nwait 120\nperf_end character playing (layers)\n";
        script += "screenshot A3_character_playing.png\nvalidation A3_character_example + RTM\npause\n";
        windowWalk();
        // weapon modes on the sample rifle: reload chain + windows + touch, the optics mode
        script += "open " + s + "Test_Weapon_01\\test_weapon_01_F.p3d\nwait 20\nweapon mode reload\nweapon time 0.9\nweapon report\n"
                  "weapon touch LeftHand magazine\nwait 5\nscreenshot Test_weapon_reload.png\nweapon mode optics\nwait 10\nweapon report\n"
                  "screenshot Test_weapon_optics.png\nkey ctrl+1\nwait 5\nvalidation Test_weapon_01\n";
        windowWalk();
        // glTF import: a file written here (mesh, UV, material, skin, one clip) opens as a model, its clip as a Timeline layer
        script += "gltf_sample\nwait_config\nwait 5\ngltf_sample check\nplay\nwait 30\npause\nscreenshot gltf_sample.png\nvalidation glTF sample\n";
        // the outfit of the Samples' man (no parts): preset saved (exe folder: outfits), put away, loaded again
        script += "unit outfit sample\nwait 20\nunit preset save selftest\nunit off\nunit preset load selftest\nwait 20\nunit report\nunit off\n";
        script += "lang stats\nlang tips\nidconflicts\ncoretest\nfuzz 100\nperf_notes end\nreport selftest.txt\nexit\n";
        fs::path dir = a.selftest.is_relative() ? app_.paths_.Exe() / a.selftest : a.selftest;
        script_.LoadText(script, dir);
    }
selftestLoaded:
    if (!a.rvmatTest.empty()) // RVMAT save round trip (use a copy: the file is rewritten)
    {
        RvMaterial o;
        if (!LoadRvMaterial(a.rvmatTest, app_.session_.projectRoot.empty() ? fs::path("P:\\") : app_.session_.projectRoot, o)) log.Error("rvmat-test: cannot load {}", PathToUtf8(a.rvmatTest));
        else
        {
            std::ifstream before(a.rvmatTest, std::ios::binary);
            std::string textBefore((std::istreambuf_iterator<char>(before)), std::istreambuf_iterator<char>());
            before.close();
            RvMaterial e = o;
            e.specularPower = o.specularPower + 7.5f;
            e.diffuse = glm::vec4(0.5f, 0.25f, 0.125f, 1.0f);
            e.forcedDiffuse = glm::vec4(0.01f, 0.02f, 0.03f, 0.0f);
            for (auto& st : e.stages)
                if (st.uv.source == 0 && !st.texture.empty()) { st.uv.aside.x *= 2.0f; st.uv.up.y *= 2.0f; break; }
            std::string err;
            int n = 0;
            bool ok = SaveRvMaterial(o, e, err, n);
            RvMaterial r;
            LoadRvMaterial(a.rvmatTest, app_.session_.projectRoot.empty() ? fs::path("P:\\") : app_.session_.projectRoot, r);
            bool same = r.specularPower == e.specularPower && r.diffuse == e.diffuse && r.forcedDiffuse == e.forcedDiffuse;
            for (size_t i = 0; i < r.stages.size() && i < e.stages.size(); ++i) same &= r.stages[i].uv.aside == e.stages[i].uv.aside && r.stages[i].uv.up == e.stages[i].uv.up;
            std::ifstream after(a.rvmatTest, std::ios::binary);
            std::string textAfter((std::istreambuf_iterator<char>(after)), std::istreambuf_iterator<char>());
            // changed lines
            auto split = [](const std::string& t) { std::vector<std::string> v; std::string cur; for (char ch : t) { if (ch == '\n') { v.push_back(cur); cur.clear(); } else if (ch != '\r') cur += ch; } if (!cur.empty()) v.push_back(cur); return v; };
            auto A = split(textBefore), B = split(textAfter);
            int diffLines = 0;
            for (size_t i = 0; i < std::max(A.size(), B.size()); ++i)
            {
                std::string x = i < A.size() ? A[i] : "", y = i < B.size() ? B[i] : "";
                if (x != y) { ++diffLines; log.Info("rvmat-test line {}: '{}' -> '{}'", i + 1, x, y); }
            }
            std::error_code ec;
            fs::path bak = a.rvmatTest;
            bak += ".bak";
            log.Info("rvmat-test: save {} ({} value(s){}), re-read {}, {} line(s) differ of {} -> {}, backup {} {}", ok ? "ok" : "FAILED", n,
                     err.empty() ? "" : ": " + err, same ? "matches the edit" : "DIFFERS", diffLines, A.size(), B.size(), PathToUtf8(bak.filename()),
                     fs::exists(bak, ec) ? "written" : "MISSING");
        }
    }
    if (!app_.pendingState_.empty() && app_.session_.game) { app_.playback_.LoadState(app_.pendingState_); app_.pendingState_.clear(); }
    // the options that act on the model: once the last model of the command line is applied (it is read in the
    // background: applied now, they were lost or reset by the model coming in)
    modelOptionsPending_ = !a.lastModel.empty();
    if (!modelOptionsPending_) ApplyModelOptions();
}

void ViewerAutomation::ApplyModelOptions()
{
    const Automation& a = auto_;
    auto& log = Log::Get();
    if (a.lod >= 0 && app_.session_.hasModel)
    {
        if (a.lod < int(app_.session_.model.lods.size())) { app_.session_.lod = a.lod; app_.OnLodChanged(); app_.camera_.Frame(app_.scene_.boundsMin, app_.scene_.boundsMax); }
        else log.Warn("--lod {}: model has {} LOD(s)", a.lod, app_.session_.model.lods.size());
    }
    if (!a.select.empty() && app_.session_.hasModel)
    {
        const P3DLod& l = app_.session_.model.lods[app_.session_.lod];
        int found = -1;
        for (int i = 0; i < int(l.selections.size()); ++i)
            if (IEquals(l.selections[i].name, a.select)) found = i;
        if (found < 0) log.Warn("--select {}: no such selection in LOD {}", a.select, app_.session_.lod);
        else
        {
            app_.scene_.selection = found;
            app_.scene_.selectedBone = app_.session_.skeleton.Find(a.select);
            app_.scene_.highlight = SceneView::Highlight::Selection;
            app_.inspector_.what = InspectorPanel::Kind::Selection;
            app_.scene_.RebuildLodGeometry();
            app_.scene_.RebuildSkeletonLines();
        }
    }
    if (!a.highlight.empty())
    {
        std::string h = ToLower(a.highlight);
        app_.scene_.highlight = h == "bones4" ? SceneView::Highlight::TooManyBones : h == "weightsum" ? SceneView::Highlight::BadWeightSum : h == "nobone" ? SceneView::Highlight::NoBone : app_.scene_.highlight;
        if (app_.scene_.highlight != SceneView::Highlight::Selection) { app_.scene_.selection = app_.scene_.selectedBone = -1; }
        app_.scene_.RebuildLodGeometry();
        app_.scene_.RebuildSkeletonLines();
    }
    if (!a.isolate.empty() && app_.session_.hasModel)
    {
        const P3DLod& l = app_.session_.model.lods[app_.session_.lod];
        LodVisibility& v = app_.vis_.Lod(l.resolution);
        v.isolate = a.isolate;
        v.isolateFaces.clear();
        app_.scene_.RebuildLodGeometry(); // not saved: automation only
        glm::vec3 mn(1e30f), mx(-1e30f);
        for (size_t f = 0; f < l.faces.size(); ++f)
            if (!app_.scene_.faceHidden[f])
                for (uint32_t c = 0; c < l.faces[f].count; ++c)
                    if (l.faces[f].v[c].point < l.points.size())
                    {
                        mn = glm::min(mn, l.points[l.faces[f].v[c].point].pos);
                        mx = glm::max(mx, l.points[l.faces[f].v[c].point].pos);
                    }
        if (mn.x <= mx.x) app_.camera_.Frame(mn, mx);
        else Log::Get().Warn("--isolate {}: no faces in LOD {}", a.isolate, app_.session_.lod);
    }
    if (a.crewIk) { app_.session_.attach.ik = true; if (app_.session_.hasModel) { app_.session_.attach.ResolveCrew(app_.CrewConfig(), app_.session_.projectRoot); app_.scene_.RebuildLodGeometry(); } }
    if (!a.compareRtm.empty())
    {
        size_t before = app_.session_.anim.layers.size();
        app_.playback_.AddLayer(a.compareRtm);
        if (app_.session_.anim.layers.size() > before)
        {
            app_.playback_.compare.layers = { app_.session_.anim.layers.back() };
            app_.session_.anim.layers.pop_back();
            app_.playback_.compare.missing = app_.session_.anim.missing;
            app_.playback_.compare.Bind(app_.session_.skeleton);
            app_.playback_.compareMode = a.compareMode;
        }
    }
    if (a.turnedOut || !a.crewModel.empty() || a.crewFollowParent)
    {
        app_.session_.attach.turnedOut = a.turnedOut;
        if (a.crewFollowParent) app_.session_.attach.followParent = true; // the default; kept for old scripts
        if (!a.crewModel.empty()) app_.session_.attach.crewModel = a.crewModel;
        if (app_.session_.hasModel) { app_.session_.attach.ResolveCrew(app_.CrewConfig(), app_.session_.projectRoot); app_.scene_.RebuildLodGeometry(); }
    }
    if (!a.focusBone.empty())
    {
        if (int b = app_.session_.skeleton.Find(a.focusBone); b >= 0) app_.SelectBone(b, true);
        else log.Warn("--focus-bone {}: no such bone", a.focusBone);
    }
    if (!a.light.empty())
    {
        std::string l = ToLower(a.light);
        if (l == "noon") app_.light_.Apply(LightPreset::Noon);
        else if (l == "sunset") app_.light_.Apply(LightPreset::Sunset);
        else if (l == "night") app_.light_.Apply(LightPreset::Night);
        else if (l == "overcast") app_.light_.Apply(LightPreset::Overcast);
        else if (l == "headlight") app_.light_.headlight = true;
        else log.Warn("--light {}: expected noon|sunset|night|overcast|headlight", a.light);
    }
    if (a.timeOfDay >= 0.0f) { app_.light_.headlight = false; app_.light_.timeOfDay = a.timeOfDay; }
    if (a.skin >= -1 && a.skin < int(app_.session_.configSkins.size())) { app_.session_.activeSkin = a.skin; app_.scene_.RebuildLodGeometry(); }
    if (a.shading >= 0 && a.shading < int(ShadingMode::Count)) app_.display_.shading = ShadingMode(a.shading);
    if (a.section >= 0 && a.section < int(app_.scene_.meshSections.size())) app_.inspector_.section = a.section;
    if (a.stage >= -1) app_.inspector_.previewStage = a.stage;
    for (const auto& [name, value] : a.sources)
    {
        auto it = std::find_if(app_.session_.cfgSources.begin(), app_.session_.cfgSources.end(), [&](const CfgSource& s) { return IEquals(s.name, name); });
        if (it == app_.session_.cfgSources.end()) log.Warn("--source {}: no such model.cfg animation source", name);
        else { it->value = value; app_.poseDirty_ = true; }
    }
    if (!a.sources.empty()) app_.scene_.UpdatePose();
    if (a.time >= 0.0f)
    {
        app_.playback_.playing = false;
        if (int p = app_.PrimaryLayer(); p >= 0)
        {
            const AnimLayer& l = app_.session_.anim.layers[p];
            app_.playback_.time = double(a.time - l.offset) * l.duration / std::max(l.speed, 1e-4f);
        }
        app_.scene_.UpdatePose();
    }
    if (!a.view.empty())
    {
        // the camera again: the model coming in framed it
        const std::string v = ToLower(a.view);
        if (v == "front") app_.SetView(kPi, 0);
        else if (v == "back") app_.SetView(0, 0);
        else if (v == "left") app_.SetView(kViewLeftYaw, 0);
        else if (v == "right") app_.SetView(kViewRightYaw, 0);
        else if (v == "top") app_.SetView(kPi, 1.55f);
    }
}

bool ViewerAutomation::Ready()
{
    app_.movesLoader_.PollValidation();
    if (app_.survey_.Busy()) app_.survey_.Poll();
    app_.modCheck_.Poll();
    return !app_.gameLoad_.valid() && !app_.movesLoader_.Building() && !app_.movesLoader_.Validating() && !app_.survey_.Busy() && !app_.modCheck_.Busy() &&
           !app_.recorder_.Recording() && !app_.ModelLoading() && !app_.texCache_.Streaming() && !app_.assets_.RtmCheckRunning();
}

void ViewerAutomation::BeforeNewFrame()
{
    if (modelOptionsPending_ && !app_.ModelLoading() && app_.session_.hasModel)
    {
        std::error_code ec;
        if (fs::equivalent(app_.session_.model.path, auto_.lastModel, ec))
        {
            modelOptionsPending_ = false;
            ApplyModelOptions();
        }
    }
    if (!script_.Active()) return;
    {
        UiScript::Host host;
        host.findNode = [this](const std::string& n, float& x, float& y) { return app_.showGraph_ && app_.graph_.NodeScreenCenter(n, x, y); };
        host.findPoint = [this](const std::string& n, float& x, float& y) { return app_.PointScreenPos(n, x, y); };
        host.findControl = [](const std::string& n, float& x, float& y) { return ui::ControlCenter(n, x, y); };
        host.screenshot = [this](const fs::path& p) { pendingShot_ = p; };
        host.exit = [this]() { PostMessageW(app_.hwnd_, WM_CLOSE, 0, 0); };
        host.ready = [this]() { return Ready(); };
        host.open = [this](const fs::path& p) { app_.OpenFile(p); };
        host.loading = [this] { return app_.ModelLoading() || app_.texCache_.Streaming(); };
        host.state = [this](const std::string& s) { app_.playback_.LoadState(s); };
        host.play = [this](bool on) { app_.playback_.playing = on; };
        host.frameMs = [this]() { return app_.frameCpuMs_; };
        host.validation = [this](const std::string& label) { return ValidationSummary(label); };
        host.perfFrames = []() { return Perf::Get().FrameReport(); };
        host.perfReset = []() { Perf::Get().ResetFrames(); };
        host.move = [this](const std::string& m, bool sw) {
            // moves / gestures act on the graph: wait for a background build (the other commands never start it)
            if (m.starts_with("gesture:")) { app_.animation_.EnsureMoves(true); app_.animation_.PlayGesture(m.substr(8)); return app_.movesSim_.msg; }
            if (m.starts_with("x:")) return MovesCommand(m.substr(2));
            if (m == "survey:start") { app_.survey_.Start(app_.session_.projectRoot); return std::string("started"); }
            if (m == "survey:report") return "\n" + app_.survey_.text;
            if (m.starts_with("rtfoot:")) { app_.rt_.opt.footIK = m.substr(7) == "1"; return std::string("foot IK ") + m.substr(7); }
            if (m.starts_with("rt:"))
            {
                strncpy_s(app_.rt_.anim, m.substr(3).c_str(), _TRUNCATE);
                if (app_.session_.game && m.find('\\') == std::string::npos) // a state name
                {
                    const MovesStateInfo s = LookupMovesState(app_.session_.game.get(), m.substr(3));
                    if (s.found)
                    {
                        fs::path p = ResolveArmaPath(s.file, app_.session_.projectRoot.empty() ? fs::path("P:\\") : app_.session_.projectRoot);
                        if (!p.has_extension()) p += ".rtm";
                        app_.rt_.duration = DurationFromMovesSpeed(s.speed) > 0 ? DurationFromMovesSpeed(s.speed) : 1.0f;
                        strncpy_s(app_.rt_.anim, PathToUtf8(p).c_str(), _TRUNCATE);
                    }
                }
                app_.animation_.RetargetRun();
                return app_.rt_.msg;
            }
            if (m.starts_with("rtexport:") || m.starts_with("rtexport!:"))
            {
                const bool over = m[8] == '!';
                const size_t at = over ? 10 : 9;
                if (m.size() > at) { strncpy_s(app_.rt_.out, m.substr(at).c_str(), _TRUNCATE); app_.rt_.outAuto = false; }
                app_.animation_.RetargetExport(over);
                return app_.rt_.msg;
            }
            if (m.starts_with("heat:")) { app_.scene_.maskHeat = true; app_.scene_.maskView = m.substr(5); app_.scene_.RebuildLodGeometry(); return std::string("heat map ") + app_.scene_.maskView; }
            app_.animation_.EnsureMoves(true);
            app_.animation_.MovesPlay(m, sw);
            return app_.movesSim_.msg;
        };
        host.movesCheck = [this](bool start) {
            if (start) { app_.movesLoader_.StartValidation(false); return std::string(); }
            app_.movesLoader_.PollValidation();
            int shown = 0;
            std::string s = app_.movesLoader_.validation.summary;
            for (const auto& it : app_.movesLoader_.validation.items)
                if (it.error && shown++ < 12) s += "\n    " + it.text;
            return s;
        };
        host.perfNotes = [this]() {
            std::string s = Perf::Get().NotesReport() + "; " + MemoryReport(app_.renderer_.Adapter());
            Perf::Get().ClearNotes();
            return s;
        };
        script_.BeforeNewFrame(host);
    }
}

void ViewerAutomation::AfterRender()
{
    if (!pendingShot_.empty())
    {
        std::string err;
        std::error_code ec;
        fs::create_directories(pendingShot_.parent_path(), ec);
        if (app_.renderer_.SaveBackbufferPng(pendingShot_.native(), err)) Log::Get().Info("ui-script: screenshot '{}'", PathToUtf8(pendingShot_));
        else Log::Get().Error("ui-script: screenshot failed: {}", err);
        pendingShot_.clear();
    }
    // Command-line automation: --screenshot when the last model is applied with its options and nothing loads any more
    // (models, textures, game configs: the ui-script's ready), 8 frames later (layout settled), then exit. A minute at
    // most: then it is taken as it is, with a warning.
    ++frameCount_;
    if (auto_.screenshot.empty() || screenshotDone_) return;
    const bool ready = !modelOptionsPending_ && Ready();
    settledFrames_ = ready ? settledFrames_ + 1 : 0;
    const bool timeout = frameCount_ > 3600;
    if (settledFrames_ >= 8 || timeout)
    {
        screenshotDone_ = true;
        if (timeout) Log::Get().Warn("--screenshot: still loading after {} frames - taken as it is", frameCount_);
        std::string err;
        if (app_.renderer_.SaveBackbufferPng(auto_.screenshot.native(), err))
            Log::Get().Info("Screenshot saved: '{}' (model {}, LOD {}, {} texture(s), frame {})", PathToUtf8(auto_.screenshot),
                            app_.session_.hasModel ? PathToUtf8(app_.session_.model.path.filename()) : "-", app_.session_.lod, app_.texCache_.Count(), frameCount_);
        else
            Log::Get().Error("Screenshot failed: {}", err);
        PostMessageW(app_.hwnd_, WM_CLOSE, 0, 0);
    }
}

std::string ViewerAutomation::RawInput(const std::string& line)
{
    if (!line.starts_with("inputstate")) return app_.keyboard_.Command(app_.hwnd_, line);
        size_t hidden = 0;
        for (uint8_t h : app_.scene_.faceHidden) hidden += h ? 1 : 0;
        return std::format("states graph {}, model.cfg graph {}, help {}, palette {} '{}', hidden faces {} of {}, camera distance {:.3f}, control {} ({:.2f} {:.2f})",
                           app_.statesPanel_.open, app_.showGraph_, app_.helpWindow_.open, app_.palette_.open, app_.palette_.Filter(), hidden, app_.scene_.faceHidden.size(), app_.camera_.distance,
                           app_.control_.on, app_.control_.rootPos.x, app_.control_.rootPos.z);
}

std::string ViewerAutomation::MovesCommand(const std::string& line)
{
    if (line.starts_with("weapon:")) return app_.weaponScene_.Command(line.substr(7));
    if (line.starts_with("raw:")) return RawInput(line.substr(4));
    std::istringstream is(line);
    std::string cmd, op;
    std::getline(is, cmd, ':');
    is >> op;
    std::string err;
    // the move commands act on the graph as it is: wait for a background build (others never trigger it)
    if (cmd == "edit" || cmd.starts_with("app_.session_.moves") || cmd == "control" || cmd == "bonecheck" || cmd == "handanim" || cmd == "statesgraph") app_.animation_.EnsureMoves(true);
    // commands of the registry (RegisterScriptCommands); the rest below is migrated group by group
    if (const auto* e = app_.scriptCommands_.Find(cmd))
    {
        std::istringstream args(line.size() > cmd.size() ? line.substr(cmd.size() + 1) : std::string());
        return e->run(args);
    }
    // control on|off|reset|state|check|turncheck|start <state|auto>
    if (cmd == "control" && (op == "state" || op == "check"))
    {
        const int c = app_.movesSim_.player.Current();
        std::string out = std::format("state {}, previous {}, drive {}, key action {}, start: {}", c >= 0 ? app_.session_.moves.states[size_t(c)].name : "-",
                                      app_.movesSim_.player.Previous() >= 0 ? app_.session_.moves.states[size_t(app_.movesSim_.player.Previous())].name : "-", app_.movesSim_.drive,
                                      app_.control_.action.empty() ? "-" : app_.control_.action, app_.movesSim_.startWhy);
        if (op == "check")
        {
            // selftest: control mode must stand in an idle state, never in death
            const bool idle = c >= 0 && !app_.session_.moves.IsDeathLike(c) && app_.session_.moves.states[size_t(c)].fileExists;
            if (!idle) Log::Get().Error("control check: {}", out);
            out = (idle ? "OK " : "FAIL ") + out;
        }
        return out;
    }
    // control turncheck: Q turns the character to its left, E to its right. Left = the side of LeftHand in the bind pose,
    // forward = from LeftFoot to LeftToeBase (both independent of the yaw convention); an error otherwise (selftest)
    if (cmd == "control" && op == "turncheck")
    {
        const Skeleton& sk = app_.session_.skeleton;
        const int lh = sk.Find("LeftHand"), rh = sk.Find("RightHand"), lf = sk.Find("LeftFoot"), lt = sk.Find("LeftToeBase");
        if (lh < 0 || rh < 0) return std::string("no LeftHand / RightHand");
        auto flat = [](glm::vec3 v) { v.y = 0; return glm::length(v) > 1e-5f ? glm::normalize(v) : glm::vec3(0); };
        const glm::vec3 left = flat(sk.bones[size_t(lh)].pivot - sk.bones[size_t(rh)].pivot);
        glm::vec3 fwd = lf >= 0 && lt >= 0 ? flat(sk.bones[size_t(lt)].pivot - sk.bones[size_t(lf)].pivot) : glm::vec3(0, 0, 1);
        if (glm::length(fwd) < 0.5f) fwd = glm::vec3(0, 0, 1);
        MovesControl& c = app_.control_;
        const float yaw = c.rootYaw;
        const glm::vec3 pos = c.rootPos;
        auto turned = [&](bool q) {
            c.rootYaw = 0;
            c.Move(app_.movesSim_, app_.session_.moves, app_.playback_.time, 0.5f, 0.0f, q, !q); // 45 degrees, no root motion
            const glm::vec3 f = glm::vec3(c.RootMatrix() * glm::vec4(fwd, 0.0f));
            c.rootYaw = yaw;
            c.rootPos = pos;
            return glm::dot(f, left);
        };
        const float q = turned(true), e = turned(false);
        const bool ok = q > 0.3f && e < -0.3f;
        std::string out = std::format("{} Q: forward towards LeftHand {:+.2f}, E: {:+.2f} (left = LeftHand side, forward = LeftFoot -> LeftToeBase)",
                                      ok ? "OK" : "FAIL", q, e);
        if (!ok) Log::Get().Error("control turncheck: {}", out);
        return out;
    }
    if (cmd == "control" && op == "start")
    {
        std::string st;
        is >> st;
        app_.movesSim_.startPick[ToLower(PathToUtf8(app_.session_.model.path))] = st == "auto" ? "" : st;
        return std::format("start state for this model: {}", st);
    }
    if (cmd == "control")
    {
        if (op == "reset") { app_.animation_.MovesReset(); return app_.movesSim_.msg; }
        app_.animation_.SetControlMode(op == "on");
        const int c = app_.movesSim_.player.Current();
        return std::format("control {} (position {:.2f} {:.2f}, state {})", app_.control_.on ? "on" : "off", app_.control_.rootPos.x, app_.control_.rootPos.z,
                           c >= 0 ? app_.session_.moves.states[size_t(c)].name : "-");
    }
    if (cmd == "moves_tests")
    {
        fs::path own;
        if (!app_.movesEditor_.OwnFile(app_.CrewConfig(), own)) return "no own move config";
        std::string out;
        for (const auto& sc : LoadScenarios(own.parent_path() / "armaview_moves_tests.txt"))
        {
            const auto r = RunScenario(app_.session_.moves, sc);
            out += std::format("\n  {} {} -> {}\n{}", r.ok ? "PASS" : "FAIL", r.name, r.reached, r.log);
        }
        return out.empty() ? "no tests" : out;
    }
    if (cmd == "moves_traps")
    {
        const Skeleton* skel = !app_.session_.skeleton.Empty() && IEquals(app_.session_.skeleton.name, app_.session_.moves.skeletonName) ? &app_.session_.skeleton : nullptr;
        app_.movesEditor_.traps = FindMoveTraps(app_.session_.moves, true, skel, op != "all");
        std::string out = std::format("{} trap(s)", app_.movesEditor_.traps.size());
        for (size_t i = 0; i < app_.movesEditor_.traps.size() && i < 20; ++i) out += "\n    " + app_.movesEditor_.traps[i].text;
        return out;
    }
    if (cmd == "layers")
    {
        // layers src <rtm|#retarget> | layers offset <bone> <rx> <ry> <rz> [falloff] | layers mask <layer#> <armaMask> | layers clip
        // <mirror 0/1> <seam N> <align 0/1> | layers bake | layers write <file> | layers blender <blend> | layers save | layers load
        if (op == "src") { std::string p; std::getline(is, p); p.erase(0, p.find_first_not_of(' ')); strncpy_s(app_.layers_.src, p.c_str(), _TRUNCATE); return std::string("ok"); }
        if (op == "duration") { is >> app_.layers_.proj.duration; return std::string("ok"); }
        if (op == "offset")
        {
            std::string bone;
            CorrectionLayer l;
            float fo = 0;
            is >> bone >> l.keys[0].rotDeg.x >> l.keys[0].rotDeg.y >> l.keys[0].rotDeg.z >> fo;
            l.name = "offset " + bone;
            l.bones.push_back({ bone, 1.0f });
            l.falloff = fo;
            app_.layers_.proj.layers.push_back(l);
            app_.layersPanel_.selLayer = int(app_.layers_.proj.layers.size()) - 1;
            app_.animation_.ShowMaskHeat(LayerMaskWeights(l, app_.session_.skeleton, app_.session_.masks));
            return std::string("ok");
        }
        if (op == "clip") { int m = 0, a2 = 0; is >> m >> app_.layers_.proj.ops.seamFrames >> a2; app_.layers_.proj.ops.mirror = m != 0; app_.layers_.proj.ops.alignRoot = a2 != 0; return std::string("ok"); }
        if (op == "bake") { app_.animation_.LayersPreview(); return app_.layers_.msg; }
        if (op == "batchstart") // the stepped batch of the Layers window (one file per frame, progress bar)
        {
            std::string in, out;
            is >> in >> out;
            strncpy_s(app_.layers_.batchDir, in.c_str(), _TRUNCATE);
            strncpy_s(app_.layers_.batchOut, out.c_str(), _TRUNCATE);
            app_.layers_.BatchStart();
            return app_.layers_.msg;
        }
        if (op == "batch")
        {
            std::string in, out;
            is >> in >> out;
            strncpy_s(app_.layers_.batchDir, in.c_str(), _TRUNCATE);
            strncpy_s(app_.layers_.batchOut, out.c_str(), _TRUNCATE);
            App::BusyScope busy(app_.busyTick); // the whole folder at once (the window steps it one file per frame instead)
            app_.layers_.Batch();
            return app_.layers_.msg;
        }
        if (op == "write") { std::string p; std::getline(is, p); p.erase(0, p.find_first_not_of(' ')); app_.layers_.Write(fs::path(Utf8ToWide(p)), false); return app_.layers_.msg; }
        if (op == "blender")
        {
            std::string p;
            std::getline(is, p);
            p.erase(0, p.find_first_not_of(' '));
            strncpy_s(app_.layers_.blendFile, p.c_str(), _TRUNCATE);
            app_.layers_.OpenInBlender(true, RunProgram);
            return app_.layers_.msg;
        }
        if (op == "save" || op == "load")
        {
            const bool ok = op == "save" ? app_.layers_.SaveProject() : app_.layers_.LoadProject();
            return ok ? PathToUtf8(app_.layers_.ProjectFile()) + (op == "save" ? " saved, " : " loaded, ") + std::to_string(app_.layers_.proj.layers.size()) + " layer(s)" : app_.layers_.msg;
        }
        return std::string("unknown layers command");
    }
    app_.movesEditor_.Open(app_.CrewConfig());
    if (!app_.session_.movesDoc.Loaded()) return app_.movesEditor_.msg;
    if (op == "set")
    {
        std::string state, entry, value;
        is >> state >> entry;
        std::getline(is, value);
        value.erase(0, value.find_first_not_of(' '));
        const bool arr = value.starts_with("{");
        if (!app_.session_.movesDoc.SetEntry(app_.movesEditor_.StatePath(state), entry, value, arr, err)) return err;
        return "ok";
    }
    if (op == "link")
    {
        std::string a, b, kind;
        is >> a >> b >> kind;
        const int from = app_.session_.moves.Find(a), to = app_.session_.moves.Find(b);
        if (from < 0 || to < 0) return "no such state";
        app_.movesEditor_.Link(app_.CrewConfig(), from, to, kind != "connect");
        return app_.movesEditor_.msg;
    }
    if (op == "setc") // any class of the own file: edit setc <Cls/Sub> <entry> <value...>
    {
        std::string path, entry, value;
        is >> path >> entry;
        std::getline(is, value);
        value.erase(0, value.find_first_not_of(' '));
        if (!app_.session_.movesDoc.SetEntry(path, entry, value, value.starts_with("{"), err)) return err;
        return "ok";
    }
    if (op == "new")
    {
        std::string name, base;
        is >> name >> base;
        if (!app_.session_.movesDoc.AddClass(app_.session_.moves.cls + "/States", name, base, { "file = \"\";", "connectTo[] = {};", "interpolateTo[] = {};" }, err)) return err;
        return "ok";
    }
    if (op == "diff")
    {
        std::string out;
        for (const auto& d : app_.session_.movesDoc.Diff(1)) out += std::format("\n    {} {}", d.kind, d.text);
        return out;
    }
    if (op == "save")
    {
        if (!app_.session_.movesDoc.Save(err)) return err;
        app_.movesEditor_.pendingCheck = true; // the hot reload rebuilds the graph; check right after
        return "saved " + PathToUtf8(app_.session_.movesDoc.File()) + " (backup " + PathToUtf8(app_.session_.movesDoc.LastBackup().filename()) + ")";
    }
    if (op == "insert_after") // edit insert_after <Cls/Sub> <text file>: the file's text after that class (e.g. a new root class)
    {
        std::string path, file;
        is >> path;
        std::getline(is, file);
        file.erase(0, file.find_first_not_of(' '));
        std::ifstream f(fs::path(Utf8ToWide(file)), std::ios::binary);
        if (!f) return "cannot read " + file;
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!app_.session_.movesDoc.InsertAfterClass(path, text, err)) return err;
        return "ok";
    }
    if (op == "reindent") // edit reindent <state>: a state class with the indentation of its neighbours
    {
        std::string state;
        is >> state;
        if (!app_.session_.movesDoc.Reindent(app_.movesEditor_.StatePath(state), err)) return err;
        return "ok";
    }
    if (op == "undo") return app_.session_.commands.Undo() ? "undone" : "nothing to undo";
    if (op == "redo") return app_.session_.commands.Redo() ? "redone" : "nothing to redo";
    return "unknown edit command " + op;
}

void ViewerAutomation::RegisterCommands()
{
    auto& reg = app_.scriptCommands_;
    // lang ru|en|stats|missing|tips: UI language and what was shown without a translation / an own tooltip
    reg.Add("lang", "ru|en|stats|missing|tips", "UI language; strings shown without a translation / controls without a tooltip", [this](std::istringstream& is) -> std::string {
        std::string op, err;
        is >> op;
        if (op == "ru" || op == "en") { SetUiLang(op == "ru" ? UiLang::Ru : UiLang::En); return "language " + op; }
        const auto [rows, tips] = TableStats();
        if (op == "stats")
            return std::format("string table: {} rows, {} tooltips; shown this session without a translation: {}, controls without an own tooltip: {}", rows, tips,
                               MissingTranslations().size(), MissingTips().size());
        const auto list = op == "tips" ? MissingTips() : MissingTranslations();
        std::string out = std::format("{} {}", list.size(), op == "tips" ? "control(s) without an own tooltip" : "string(s) without a translation");
        for (const auto& s : list) out += "\n    " + s;
        return out;
    });
    // modcheck <folder> (then wait_config) | modcheck result: Mod check from a script
    reg.Add("modcheck", "<folder>|last", "check a mod folder (configs, models, RTMs, textures)", [this](std::istringstream& is) -> std::string {
        std::string op, err;
        is >> op;
        std::string rest;
        std::getline(is, rest);
        const std::string arg = op + rest;
        if (arg != "result") { app_.modCheck_.Start(arg); return "started " + arg; }
        app_.modCheck_.Poll();
        if (!app_.modCheck_.HasReport()) return std::string("no report");
        const ModReport& r = app_.modCheck_.Report();
        std::string out = std::format("{}: {} error(s), {} warning(s), files", PathToUtf8(r.folder), r.Count(ModIssue::Sev::Error), r.Count(ModIssue::Sev::Warning));
        for (const auto& [k, n] : r.files) out += std::format(" {} {}", k, n);
        if (r.hadPrevious)
            out += std::format("; against {}: {} new, {} fixed", r.previousWhen, std::count_if(r.issues.begin(), r.issues.end(), [](const ModIssue& i) { return i.isNew; }), r.fixed.size());
        out += "; html " + PathToUtf8(r.html);
        int n = 0;
        for (const auto& i : r.issues)
            if (n++ < 25) out += std::format("\n    {} {} {}{}: {}", i.sev == ModIssue::Sev::Error ? "E" : "W", i.kind, PathToUtf8(i.file.filename()), i.line ? std::format("({})", i.line) : "", i.text);
        return out;
    });
    // record anim|turn mp4|gif|png <w> <h> <fps> [turn seconds] (then wait_config) | record result
    reg.Add("record", "anim|turn mp4|gif|png <w> <h> <fps> [turn s] [dir=<folder>] | result", "record frames to MP4 / GIF / PNG", [this](std::istringstream& is) -> std::string {
        std::string op, err;
        is >> op;
        if (op == "result") return app_.recorder_.Recording() ? std::string("recording") : app_.recorder_.RecordMessage();
        // record cancel: the Stop button while recording (qt-req 1.4): no Error in the log, the partial MP4 / GIF and the
        // ffmpeg log are gone from the output folder (an error otherwise)
        if (op == "cancel")
        {
            if (!app_.recorder_.Recording()) return std::string("not recording");
            const int errorsBefore = Log::Get().Errors();
            app_.recorder_.StopRecording(RecordStop::Cancelled); // what the Stop button does
            std::error_code ec;
            std::string left;
            for (const auto& e : fs::directory_iterator(fs::path(Utf8ToWide(app_.record_.settings.outDir)), ec))
            {
                const std::string ext = ToLower(PathToUtf8(e.path().extension()));
                if (ext == ".mp4" || ext == ".gif" || ext == ".log") left += " " + PathToUtf8(e.path().filename());
            }
            if (ec) left += " (cannot list the folder: " + ec.message() + ")";
            const bool errorLogged = Log::Get().Errors() > errorsBefore;
            const std::string out = std::format("{} cancel: {}{}{}", left.empty() && !errorLogged ? "OK" : "FAIL", app_.recorder_.RecordMessage(),
                                                left.empty() ? "" : "; left on disk:" + left, errorLogged ? "; an Error was logged" : "");
            if (!left.empty() || errorLogged) Log::Get().Error("record cancel: {}", out);
            return out;
        }
        std::string fmt;
        is >> fmt >> app_.record_.settings.width >> app_.record_.settings.height >> app_.record_.settings.fps;
        // [turn seconds] [dir=<output folder>]
        for (std::string tok; is >> tok;)
        {
            if (tok.starts_with("dir="))
            {
                std::string rest;
                std::getline(is, rest);
                strncpy_s(app_.record_.settings.outDir, (tok.substr(4) + rest).c_str(), _TRUNCATE);
                break;
            }
            app_.record_.settings.turnSeconds = float(std::atof(tok.c_str()));
        }
        app_.record_.settings.what = op == "turn" ? 1 : 0;
        app_.record_.settings.format = fmt == "gif" ? 1 : fmt == "png" ? 2 : 0;
        app_.recorder_.StartRecording();
        return app_.recorder_.RecordMessage();
    });
    // gltf_sample | gltf_sample check: writes the selftest glTF (Gltf.h WriteGltfSample) and opens it; check: the model,
    // its skeleton and the clip layer arrived (an error otherwise)
    reg.Add("gltf_sample", "[check]", "open the generated glTF sample / check what was imported", [this](std::istringstream& is) -> std::string {
        std::string op;
        is >> op;
        const fs::path file = app_.paths_.Data() / L"selftest_gltf" / L"sample.gltf";
        if (op != "check")
        {
            if (!WriteGltfSample(file))
            {
                Log::Get().Error("gltf_sample: cannot write {}", PathToUtf8(file));
                return std::string("write failed");
            }
            app_.assets_.OpenFile(file);
            return "opening " + PathToUtf8(file);
        }
        const Session& s = app_.session_;
        int clips = 0, matched = 0;
        for (const auto& l : s.anim.layers)
            if (l.anim && l.anim->info.signature == "glTF") { ++clips; matched = l.matched; }
        const bool ok = s.hasModel && s.model.format == "glTF" && s.skeleton.bones.size() == 2 && s.hasPivots && clips == 1 && matched == 2 &&
                        !s.model.lods.empty() && s.model.lods[0].faces.size() == 20;
        const std::string r = std::format("glTF sample: model {}, {} bone(s), {} clip layer(s), {} bone(s) matched, {} triangle(s)", s.model.format,
                                          s.skeleton.bones.size(), clips, matched, s.model.lods.empty() ? 0 : s.model.lods[0].faces.size());
        if (!ok) Log::Get().Error("gltf_sample: {}", r);
        return r;
    });
    // fuzz <iterations>: the parsers on mutated copies of seed files (Arma 3 Samples / the core test files) - selftest
    reg.Add("fuzz", "<n>", "mutate the sample files n times each through every parser", [this](std::istringstream& is) -> std::string {
        std::string op, err;
        is >> op;
        const int n = std::max(1, std::atoi(op.c_str()));
        std::vector<fs::path> seeds;
        std::error_code ec;
        const LocalPaths& lp = GetLocalPaths();
        auto add = [&](const fs::path& p) { if (fs::exists(p, ec)) seeds.push_back(p); };
        if (!lp.samples.empty())
            for (const char* rel : { "Test_Weapon_01\\test_weapon_01_F.p3d", "Test_Weapon_01\\config.cpp", "Test_Weapon_01\\model.cfg",
                                     "Test_Anims\\AmovPercMstpSrasWrflDnon.rtm", "Test_scope3D_01\\Data\\holoreticle.rvmat" })
                add(fs::path(Utf8ToWide(lp.samples)) / rel);
        for (const auto& e : fs::directory_iterator(app_.paths_.Data() / "selftest_core", ec)) seeds.push_back(e.path()); // coretest's files
        int exceptions = 0;
        const std::string r = FuzzRun(seeds, n, 12345u, &exceptions, app_.busyTick);
        if (exceptions) Log::Get().Error("fuzz: {}", r);
        return r;
    });
    // bonecheck [label]: in the current pose
    //  - no bone leaves its parent further than its bind length + tolerance (3 cm + 10 %); bones without a pivot skipped;
    //  - a bone no layer has data for keeps its bind rotation relative to the parent (deformation matrices: identity);
    //  - a bone the Override layers (gesture, handAnim) do not move keeps its base rotation relative to the parent
    //    (the pose without the overrides): otherwise fingers / face bones twist away from a hand / head that moved.
    // An error per bad bone in the parent-relative blending (selftest); model space is reported only.
    reg.Add("bonecheck", "[label]", "no bone of the current pose leaves its parent (error per stretched bone)", [this](std::istringstream& is) -> std::string {
        std::string op, err;
        is >> op;
        if (app_.session_.skeleton.Empty() || app_.scene_.boneMats.size() != app_.session_.skeleton.bones.size()) return std::string("no posed skeleton");
        struct Bad { float d; std::string text; };
        std::vector<Bad> bad;
        int checked = 0;
        for (size_t b = 0; b < app_.session_.skeleton.bones.size(); ++b)
        {
            const Bone& bn = app_.session_.skeleton.bones[b];
            if (bn.parent < 0 || bn.pivotSource == PivotSource::None || app_.session_.skeleton.bones[size_t(bn.parent)].pivotSource == PivotSource::None) continue;
            // bones the engine places itself (the weapon in the hands, the launcher on the back, the camera): not a limb
            const std::string ln = ToLower(bn.name);
            if (ln == "weapon" || ln == "launcher" || ln == "camera" || ln == "pistol" || ln == "binocular") continue;
            const Bone& pn = app_.session_.skeleton.bones[size_t(bn.parent)];
            const float bind = glm::length(bn.pivot - pn.pivot);
            const glm::vec3 a = glm::vec3(app_.scene_.boneMats[b] * glm::vec4(bn.pivot, 1.0f)), p = glm::vec3(app_.scene_.boneMats[size_t(bn.parent)] * glm::vec4(pn.pivot, 1.0f));
            const float now = glm::length(a - p);
            ++checked;
            const float tol = 0.03f + 0.1f * bind;
            if (std::abs(now - bind) > tol)
                bad.push_back({ std::abs(now - bind), std::format("{} from {}: {:.3f} m, bind {:.3f} m", bn.name, pn.name, now, bind) });
        }
        // rotation relative to the parent where there is no data
        const Skeleton& sk = app_.session_.skeleton;
        const auto& M = app_.scene_.boneMats;
        const size_t nb = sk.bones.size();
        const std::vector<bool> covered = app_.session_.anim.Covered(nb), overridden = app_.session_.anim.Overridden(nb);
        std::vector<glm::mat4> base;
        {
            AnimStack noOverrides = app_.session_.anim;
            bool any = false;
            for (auto& l : noOverrides.layers)
                if (l.mode == LayerMode::Override && l.enabled) { l.enabled = false; any = true; }
            if (any) noOverrides.Evaluate(sk, app_.playback_.time, base);
        }
        auto angle = [](const glm::mat4& m) {
            const glm::quat q = glm::normalize(glm::quat_cast(glm::mat3(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])), glm::normalize(glm::vec3(m[2])))));
            return glm::degrees(2.0f * std::acos(std::min(1.0f, std::abs(q.w))));
        };
        const size_t stretched = bad.size();
        int rotChecked = 0;
        for (size_t b = 0; b < nb; ++b)
        {
            const int p = sk.bones[b].parent;
            if (p < 0) continue;
            const std::string ln = ToLower(sk.bones[b].name);
            if (ln == "weapon" || ln == "launcher" || ln == "camera" || ln == "pistol" || ln == "binocular") continue;
            const glm::mat4 local = glm::inverse(M[size_t(p)]) * M[b];
            float d = -1.0f;
            const char* why = "";
            if (!covered[b]) { d = angle(local); why = "no data, rotated from bind"; }
            else if (base.size() == nb && !overridden[b])
            {
                d = angle(glm::inverse(glm::inverse(base[size_t(p)]) * base[b]) * local);
                why = "not overridden, rotated from its base pose";
            }
            if (d < 0.0f) continue;
            ++rotChecked;
            if (d > 3.0f) bad.push_back({ d / 30.0f, std::format("{} under {}: {} by {:.1f} deg", sk.bones[b].name, sk.bones[size_t(p)].name, why, d) });
        }
        const size_t twisted = bad.size() - stretched;
        std::sort(bad.begin(), bad.end(), [](const Bad& x, const Bad& y) { return x.d > y.d; });
        std::string out = std::format("{} ({}, missing bones {}): {} bone(s) checked, {} stretched; {} rotation(s) checked, {} twisted", op.empty() ? "pose" : op,
                                      app_.session_.blendMode ? "parent-relative" : "model space",
                                      app_.session_.anim.missing == MissingBones::Engine ? "engine" : "follow parent", checked, stretched, rotChecked, twisted);
        for (size_t i = 0; i < bad.size() && i < 12; ++i) out += "\n    " + bad[i].text;
        // an error in the default parent-relative blending; model space is the approximation (reported only)
        if (!bad.empty() && app_.session_.blendMode == 1) Log::Get().Error("bonecheck: {}", out);
        else if (!bad.empty()) Log::Get().Warn("bonecheck: {}", out);
        return out;
    });
    // missing engine|follow: Blend layer bones without data - identity (engine, not confirmed for Arma 3) or follow the parent
    reg.Add("missing", "engine|follow", "bones without data in a Blend layer: identity (engine) or follow the parent (default)", [this](std::istringstream& is) -> std::string {
        std::string op;
        is >> op;
        app_.session_.anim.missing = op == "engine" ? MissingBones::Engine : MissingBones::FollowParent;
        app_.poseDirty_ = true;
        return std::string(op == "engine" ? "engine (identity)" : "follow parent");
    });
    // handanim <file.rtm> [mask] | handanim off: a weapon's handAnim over the current pose (Override layer, frame 0, the
    // mask's weights, default handsWeapon), as the game holds a weapon
    reg.Add("handanim", "<file.rtm> [mask] | off", "a handAnim RTM over the pose (Override, frame 0, mask default handsWeapon)", [this](std::istringstream& is) -> std::string {
        std::string file, mask;
        is >> file >> mask;
        auto& layers = app_.session_.anim.layers;
        std::erase_if(layers, [](const AnimLayer& l) { return l.tag == 2; });
        if (file.empty() || file == "off")
        {
            app_.session_.Events().Publish(SessionEvent::AnimationChanged);
            app_.poseDirty_ = true;
            return std::string("handAnim off");
        }
        auto anim = std::make_shared<RtmAnimation>();
        std::string err;
        std::vector<std::string> warn;
        if (!LoadRtm(fs::path(Utf8ToWide(file)), *anim, err, warn)) { Log::Get().Error("handanim {}: {}", file, err); return "cannot load: " + err; }
        if (mask.empty()) mask = "handsWeapon";
        AnimLayer l;
        l.anim = anim;
        l.tag = 2;
        l.mode = LayerMode::Override;
        l.speed = 0.0f;
        l.loop = false;
        l.boneWeight = app_.gesturePlayer_.MaskWeights(mask);
        const auto masked = std::count_if(l.boneWeight.begin(), l.boneWeight.end(), [](float w) { return w > 0; });
        if (masked == 0) l.boneWeight.clear(); // no such mask (no CfgMoves): every bone
        layers.push_back(l);
        app_.session_.anim.Bind(app_.session_.skeleton);
        app_.session_.Events().Publish(SessionEvent::AnimationChanged);
        app_.poseDirty_ = true;
        return std::format("handAnim {}: mask {} ({} bones), {} of {} skeleton bones in the RTM", PathToUtf8(fs::path(Utf8ToWide(file)).filename()), masked ? mask : "-",
                           masked, layers.back().matched, app_.session_.skeleton.bones.size());
    });
    // blend model|parent: gesture / handAnim blending mode (both panels)
    reg.Add("blend", "model|parent", "gesture / handAnim blending: model space or parent-relative", [this](std::istringstream& is) -> std::string {
        std::string op, err;
        is >> op;
        { app_.session_.blendMode = op == "model" ? 0 : 1; app_.poseDirty_ = true; return std::string(app_.session_.blendMode ? "parent-relative" : "model space"); }
        return {};
    });
    // coretest: the parsers without game data (CI): a model.cfg, a config.cpp with CfgMoves and an RTM written by the
    // viewer itself are read back and checked
    reg.Add("coretest", "", "parsers, CfgMoves, command stack on generated files", [this](std::istringstream& is) -> std::string {
        std::string op, err;
        is >> op;
        return CoreTest();
    });
    // crashtest: an access violation on purpose (checks the crash report); unsaved: what closing would ask about
    reg.Add("crashtest", "", "access violation on purpose (checks the crash report)", [this](std::istringstream& is) -> std::string {
        std::string op, err;
        is >> op;
        Log::Get().Warn("crashtest: writing to a null pointer on purpose");
        volatile int* p = nullptr;
        *p = 1;
        return std::string("not reached");
    });
    reg.Add("unsaved", "[prompt]", "what closing would ask about", [this](std::istringstream& is) -> std::string {
        std::string op, err;
        is >> op;
        if (op == "prompt") app_.closePrompt_.Show(); // the dialog closing would show (screenshot)
        std::string out;
        for (const auto& u : app_.closePrompt_.UnsavedEdits()) out += "\n    " + u;
        return out.empty() ? std::string("nothing unsaved") : out;
    });
    // mode <0..n | name>: switch the UI mode (as Ctrl+1..); show <window> [off]: open / close a floating window
    reg.Add("mode", "<n|name>", "switch the UI mode", [this](std::istringstream& is) -> std::string {
        std::string op, err;
        is >> op;
        for (int i = 0; i < int(UiMode::Count); ++i)
            if (op == std::to_string(i) || IEquals(op, UiShell::ModeName(UiMode(i)))) { app_.SetMode(UiMode(i)); return std::format("mode {}", UiShell::ModeName(UiMode(i))); }
        return "unknown mode " + op;
    });
    reg.Add("show", "<window|all> [off]", "open / close a floating window", [this](std::istringstream& is) -> std::string {
        std::string op, err;
        is >> op;
        std::string w = op, onoff;
        is >> onoff;
        const bool on = onoff != "off";
        const std::pair<const char*, bool*> wins[] = {
            { "help", &app_.helpWindow_.open }, { "about", &app_.aboutWindow_.open }, { "start", &app_.guidePanel_.open }, { "modcheck", &app_.modCheck_.open },
            { "record", &app_.record_.open }, { "graph", &app_.showGraph_ }, { "states", &app_.statesPanel_.open }, { "movesdiff", &app_.moveEditorPanel_.showDiff },
            { "startscreen", &app_.start_.open }, { "setup", &app_.start_.setupOpen },
        };
        std::string names;
        for (const auto& [n, flag] : wins)
        {
            if (w == "all" || IEquals(w, n)) *flag = on;
            names += std::string(" ") + n;
        }
        return std::format("show {} {} (windows:{})", w, on ? "on" : "off", names);
    });
    // idconflicts: controls of the windows drawn so far that shared an ImGui ID in a frame (an error each - selftest)
    // camera on the current (posed) model: frame all, or a bone and the points it moves
    // layouts save|use|delete <name> | list | export|import <file> | where: the settings file and its named layouts
    reg.Add("layouts", "save|use|delete <name> | list | export|import <file> | where", "named layouts, settings export / import", [this](std::istringstream& is) -> std::string {
        std::string op, arg;
        is >> op;
        std::getline(is, arg);
        arg.erase(0, arg.find_first_not_of(' '));
        LayoutStore& l = app_.layouts_;
        if (op == "save") { l.SaveAs(arg); return "saved " + arg; }
        if (op == "use") return l.Use(arg) ? "using " + arg : "no layout " + arg;
        if (op == "delete") { l.Remove(arg); return "deleted " + arg; }
        if (op == "export") return l.Export(fs::path(Utf8ToWide(arg))) ? "exported" : "export failed";
        if (op == "import") return l.Import(fs::path(Utf8ToWide(arg))) ? "imported" : "import failed";
        if (op == "where") return PathToUtf8(app_.settings_.File()) + " (" + app_.paths_.Why() + ")";
        std::string out;
        for (const auto& n : l.Names()) out += (out.empty() ? "" : ", ") + n;
        return out.empty() ? std::string("no named layouts") : out;
    });
    // unit <class> | off | part <slot> <class|none> | hide|show <slot> | report: a soldier class assembled (Unit window)
    // unit outfit sample|<p3d> | file <slot> <p3d> | remove <index> | preset save|load|delete <name>: the Outfit tab
    reg.Add("unit", "<class> | off | part <slot> <class|none> | hide|show <slot> | report | outfit sample|<p3d> | file <slot> <p3d> | remove <i> | preset save|load|delete <name>",
            "assemble a soldier class from CfgVehicles, or an outfit on any body", [this](std::istringstream& is) -> std::string {
        std::string op;
        is >> op;
        Unit& u = app_.session_.unit;
        auto rest = [&is] { std::string r; std::getline(is, r); r.erase(0, r.find_first_not_of(' ')); return r; };
        if (op == "report") return u.Report();
        if (op == "tab") { std::string t; is >> t; app_.unit_.SelectTab(t == "outfit" ? 1 : 0); return "tab " + t; }
        if (op == "outfit")
        {
            const std::string what = rest();
            app_.OutfitBody(what == "sample" ? app_.SamplesArmaMan() : fs::path(Utf8ToWide(what)));
            return u.Outfit() ? "outfit on " + PathToUtf8(u.OutfitBody().filename()) : std::string("no outfit");
        }
        if (op == "file")
        {
            std::string slotName;
            is >> slotName;
            UnitSlot slot;
            if (!ParseUnitSlot(slotName, slot)) return "no slot " + slotName;
            app_.AddUnitFile(slot, fs::path(Utf8ToWide(rest())));
            return u.Report();
        }
        if (op == "remove")
        {
            int i = -1;
            is >> i;
            app_.RemoveUnitPart(size_t(std::max(i, 0)));
            return u.Report();
        }
        if (op == "preset")
        {
            std::string what;
            is >> what;
            const std::string name = rest();
            if (what == "save") return app_.SaveOutfitPreset(name) ? "saved " + name : "save failed";
            if (what == "load") return app_.LoadOutfitPreset(name) ? "loading " + name : "load failed";
            if (what == "delete") return app_.DeleteOutfitPreset(name) ? "deleted " + name : "delete failed";
            return std::string("preset save|load|delete <name>");
        }
        if (op == "part" || op == "hide" || op == "show")
        {
            std::string slotName, cls;
            is >> slotName >> cls;
            UnitSlot slot;
            if (!ParseUnitSlot(slotName, slot)) return "no slot " + slotName;
            if (op == "part")
            {
                app_.ReplaceUnitPart(slot, cls == "none" ? "" : cls);
                return u.Report();
            }
            for (auto& p : u.Parts())
                if (p.slot == slot) p.enabled = op == "show";
            app_.UnitPartsChanged();
            return op + " " + slotName;
        }
        app_.AssembleUnit(op);
        return op == "off" ? std::string("unit put away") : "assembling " + op;
    });
    // statesgraph view states|actions | class <actions class|all> | action <action|none>: the CfgMoves states graph's
    // actions filter (selftest: counts into the report)
    reg.Add("statesgraph", "view states|actions | class <cls|all> | action <a|none>", "states graph: view, actions class filter, action highlight", [this](std::istringstream& is) -> std::string {
        std::string op, v;
        is >> op >> v;
        StatesGraph& sg = app_.statesPanel_.graph;
        const MovesGraph& g = app_.session_.moves;
        if (op == "view") sg.view = v == "actions" ? 1 : 0;
        else if (op == "class") sg.actionsClass = v == "all" ? std::string() : ToLower(v);
        else if (op == "action") sg.action = v == "none" ? std::string() : ToLower(v);
        else return std::string("view|class|action");
        int used = 0, bad = 0;
        for (const auto& s : g.states) used += !sg.actionsClass.empty() && IEquals(s.actions, sg.actionsClass);
        if (auto it = g.actions.find(sg.actionsClass); it != g.actions.end())
            for (const auto& [a, s] : it->second) bad += !s.empty() && g.Find(s) < 0;
        int targets = 0, missing = 0;
        if (!sg.action.empty())
            for (const auto& [cls, k] : sg.ActionTargets(g, sg.action)) (k >= 0 ? targets : missing)++;
        return std::format("view {}, class {} ({} state(s), {} action(s) to missing states), action {} in {} class(es) to a state, {} to a missing state",
                           sg.view ? "actions" : "states", sg.actionsClass.empty() ? "all" : sg.actionsClass, used, bad,
                           sg.action.empty() ? "none" : sg.action, targets, missing);
    });
    // optstate: what the command-line options left in the viewer (tools/tests/cli_options.py checks it)
    reg.Add("optstate", "", "the state the command-line options set (LOD, selection, highlight, isolate, bone, compare, skin, section)",
            [this](std::istringstream&) -> std::string {
        const Session& s = app_.session_;
        int bound = 0, compareBones = 0;
        if (!app_.playback_.compare.layers.empty())
            for (int m : app_.playback_.compare.layers[0].boneMap) { ++compareBones; bound += m >= 0; }
        size_t hidden = 0;
        for (uint8_t h : app_.scene_.faceHidden) hidden += h ? 1 : 0;
        std::string isolate;
        if (s.hasModel) isolate = app_.vis_.Lod(s.model.lods[size_t(s.lod)].resolution).isolate;
        const std::string sel = s.hasModel && app_.scene_.selection >= 0 && app_.scene_.selection < int(s.model.lods[size_t(s.lod)].selections.size())
                                    ? s.model.lods[size_t(s.lod)].selections[size_t(app_.scene_.selection)].name : "-";
        return std::format("model={} lod={} selection={} highlight={} bone={} isolate={} hiddenFaces={} compare={}/{} skin={} section={}",
                           s.hasModel ? PathToUtf8(s.model.path.filename()) : "-", s.lod, sel, int(app_.scene_.highlight),
                           app_.scene_.selectedBone >= 0 && app_.scene_.selectedBone < int(s.skeleton.bones.size()) ? s.skeleton.bones[size_t(app_.scene_.selectedBone)].name : "-",
                           isolate.empty() ? "-" : isolate, hidden, bound, compareBones, s.activeSkin, app_.inspector_.section);
    });
    // a config skin hiding one selection ("" texture), on the open model: its faces leave the drawn sections (and
    // the picking), the rest stays; a shorter array / material "" change nothing. Errors when not so.
    reg.Add("skintest", "<selection>", "hiddenSelectionsTextures \"\" hides that selection's faces (an Error otherwise)", [this](std::istringstream& is) -> std::string {
        std::string sel;
        is >> sel;
        Session& s = app_.session_;
        SceneView& v = app_.scene_;
        if (!s.hasModel) return "no model";
        const P3DLod& l = s.model.lods[size_t(s.lod)];
        size_t selFaces = 0;
        for (const auto& x : l.selections)
            if (IEquals(x.name, sel)) selFaces = x.faceCount;
        if (!selFaces) return "no selection " + sel;
        auto drawn = [&] {
            size_t n = 0;
            for (size_t i = 0; i < v.vehicleSections && i < v.meshSections.size(); ++i) n += v.meshSections[i].faces;
            return n;
        };
        const auto keepSkins = s.configSkins;
        const int keepActive = s.activeSkin;
        auto withSkin = [&](std::vector<std::string> texs, std::vector<std::string> mats) {
            ConfigSkin k;
            k.cfgRoot = "CfgTest";
            k.className = "AV_SkinTest";
            k.selections = { sel };
            k.textures = std::move(texs);
            k.materials = std::move(mats);
            s.configSkins = { k };
            s.activeSkin = 0;
            v.RebuildLodGeometry();
            return std::make_pair(drawn(), v.skinHiddenFaces);
        };
        s.configSkins.clear();
        s.activeSkin = -1;
        v.RebuildLodGeometry();
        const size_t before = drawn();
        const auto [hiddenDrawn, hidden] = withSkin({ "" }, {});
        const auto [shortDrawn, shortHidden] = withSkin({}, { "" });
        s.configSkins = keepSkins;
        s.activeSkin = keepActive;
        v.RebuildLodGeometry();
        const bool ok = hidden > 0 && hidden <= selFaces && hiddenDrawn + hidden == before && shortDrawn == before && shortHidden == 0;
        const std::string msg = std::format("skintest {}: {} face(s) drawn, texture \"\" -> {} drawn, {} hidden (selection {} faces); no texture + material \"\" -> {} drawn",
                                            sel, before, hiddenDrawn, hidden, selFaces, shortDrawn);
        if (!ok) Log::Get().Error("{}", msg);
        return msg;
    });
    reg.Add("frame", "", "frame the model in its current pose", [this](std::istringstream&) -> std::string {
        app_.camera_.Frame(app_.scene_.boundsMin, app_.scene_.boundsMax);
        return "framed";
    });
    reg.Add("focusbone", "<bone>", "select a bone and frame the points it moves (posed)", [this](std::istringstream& is) -> std::string {
        std::string name;
        is >> name;
        const int b = app_.session_.skeleton.Find(name);
        if (b < 0) return "no bone " + name;
        app_.SelectBone(b, false);
        app_.input_.FocusBone(b);
        return "focused " + app_.session_.skeleton.bones[size_t(b)].name;
    });

    reg.Add("idconflicts", "", "controls drawn so far with one ImGui ID in a frame (an error if any)", [this](std::istringstream& is) -> std::string {
        std::string op, err;
        is >> op;
        const auto list = IdConflicts();
        std::string out = std::format("{} ID conflict(s) among {} controls checked", list.size(), IdsChecked());
        for (const auto& c : list) out += "\n    " + c;
        if (!list.empty()) Log::Get().Error("{}", out);
        return out;
    });
    // tip: the tooltip shown in the last frame and the status bar hint
    reg.Add("tip", "", "tooltip shown in the last frame and the status bar hint", [this](std::istringstream& is) -> std::string {
        std::string op, err;
        is >> op;
        return std::format("tooltip [{}] | status bar [{}]", ui::LastTip(), ui::HoverHint());
    });
    // guide <n> [step]: Getting started scenario n (0-based), its highlighted control
    reg.Add("guide", "...", "Getting started guide steps", [this](std::istringstream& is) -> std::string {
        std::string op, err;
        is >> op;
        if (op == "off") { app_.guidePanel_.open = false; app_.guidePanel_.Go(-1, 0); return std::string("guide off"); }
        int step = 0;
        is >> step;
        app_.guidePanel_.Go(std::atoi(op.c_str()), step);
        return std::format("guide {} step {}: highlight [{}]", app_.guidePanel_.Scenario(), app_.guidePanel_.Step(), ui::Highlight());
    });
}

std::string ViewerAutomation::CoreTest()
{
    std::string r = RunCoreTest(app_.paths_.Data() / "selftest_core");
    r += "\nview coretest: " + RunViewCoreTest(app_.paths_.Data() / "selftest_view");
    // the point overlay's settings change through its setters alone (no ImGui widget, no frame)
    PointOverlay p;
    PointOverlaySettings s = p.Settings();
    s.labels = false;
    s.occludeByMesh = true;
    s.hitpointsView = true;
    s.filter = "wheel";
    p.SetSettings(s);
    const bool ok = p.Settings() == s && p.WantsDepth();
    s.hitpointsView = false;
    p.SetSettings(s);
    const bool off = !p.WantsDepth(); // occlusion needs labels or hitpoints shown
    r += std::format("\n    {} point overlay: SetSettings changes what is shown without an ImGui widget (depth wanted {} -> {})", ok && off ? "ok  " : "FAIL", ok, !off);
    if (!(ok && off)) Log::Get().Error("coretest: point overlay setters");
    // render: texture streaming under stress (qt-req 1.1)
    int renderFailures = 0;
    r += "\nrender coretest:" + RunTextureStreamTest(app_.renderer_.Device(), &renderFailures);
    if (renderFailures) Log::Get().Error("coretest: render: {} check(s) failed", renderFailures);
    return r;
}

// the selftest summary of the open model: checks, loop seam, log since it was loaded, orientation in the Front view
std::string ViewerAutomation::ValidationSummary(const std::string& label)
{
    app_.assets_.WaitRtmCheck(); // the RTM folder check runs in the background: its results belong to the summary
    app_.logWindow_.Flush();
    std::string s = std::format("=== {} ===\n", label);
    if (!app_.session_.hasModel) return s + "no model\n";
    s += std::format("model {} - {} LOD(s), load {:.0f} ms\n", PathToUtf8(app_.session_.model.path), app_.session_.model.lods.size(), app_.lastLoadMs_);
    const ModelChecks mc = CheckModel(app_.session_, app_.scene_.validation, app_.scene_.boneSelection);
    s += mc.text;
    int errors = mc.errors, warnings = mc.warnings;
    if (!app_.playback_.seam.empty()) s += std::format("loop seam: {} bone(s), worst {} {:.3f} m\n", app_.playback_.seam.size(), app_.playback_.seam[0].bone, app_.playback_.seam[0].distance);
    // log since this model was loaded
    const auto& entries = app_.logWindow_.Entries();
    for (size_t i = app_.logMark_; i < entries.size(); ++i)
    {
        if (entries[i].level == LogLevel::Error) { ++errors; s += "  log ERROR: " + entries[i].text.substr(0, 300) + "\n"; }
        else if (entries[i].level == LogLevel::Warn) ++warnings;
    }
    // orientation: in the Front view (as Blender Numpad 1) a character faces the camera and its left arm is on the right
    {
        auto sel = [&](const char* n, glm::vec3& c) {
            const P3DLod& l = app_.session_.model.lods[0];
            for (const auto& sl : l.selections)
                if (IEquals(sl.name, n))
                {
                    glm::vec3 sum(0.0f);
                    int k = 0;
                    for (size_t p = 0; p < sl.pointBytes.size() && p < l.points.size(); ++p)
                        if (sl.Weight(p) > 0.5f) { sum += l.points[p].pos; ++k; }
                    if (k) { c = sum / float(k); return true; }
                }
            return false;
        };
        glm::vec3 la, ra, toe, foot;
        if (sel("LeftArm", la) && sel("RightArm", ra) && sel("LeftToeBase", toe) && sel("LeftFoot", foot))
        {
            OrbitCamera cam = app_.camera_;
            cam.yaw = kViewFrontYaw;
            cam.pitch = 0.0f;
            cam.target = (la + ra) * 0.5f;
            const glm::mat4 vp = cam.Projection(1.0f) * cam.View();
            auto ndc = [&](glm::vec3 p) { glm::vec4 c = vp * glm::vec4(p, 1.0f); return glm::vec3(c) / c.w; };
            const bool leftOnRight = ndc(la).x > ndc(ra).x;
            const bool facesCamera = ndc(toe).z < ndc(foot).z; // toes nearer to the Front camera than the heel
            s += std::format("orientation (Front view): LeftArm on the {} side, toes {} the camera - {}\n", leftOnRight ? "right" : "LEFT",
                             facesCamera ? "towards" : "AWAY FROM", leftOnRight && facesCamera ? "ok (as Blender)" : "WRONG");
            if (!(leftOnRight && facesCamera)) ++errors;
        }
    }
    s += std::format("RESULT {}: {} error(s), {} warning(s) in the log\n", label, errors, warnings);
    return s;
}
