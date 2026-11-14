#include "App.h"
#include "Dialogs.h"
#include "Lang.h"
#include "RunProgram.h"
#include "LocalPaths.h"
#include "Gltf.h"
#include "Perf.h"
#include "PerfMemory.h"
#include "Log.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui-node-editor/imgui_node_editor.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <commdlg.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <map>
#include <tuple>

namespace fs = std::filesystem;

namespace {

constexpr float kPi = 3.14159265f;


bool ContainsNoCase(const std::string& hay, const std::string& needle)
{
    return needle.empty() || ToLower(hay).find(ToLower(needle)) != std::string::npos;
}

std::wstring LowerExt(const fs::path& p)
{
    std::wstring e = p.extension().native();
    for (auto& c : e) c = wchar_t(std::towlower(c));
    return e;
}



// Opens file:line in VS Code (or Notepad++), else the default app; also copies "file:line" to the clipboard.
void OpenFileAtLine(const fs::path& file, int line)
{
    ImGui::SetClipboardText(std::format("{}:{}", PathToUtf8(file), line).c_str());
    wchar_t code[MAX_PATH] = {}, npp[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Programs\\Microsoft VS Code\\Code.exe", code, MAX_PATH);
    ExpandEnvironmentStringsW(L"%ProgramFiles%\\Notepad++\\notepad++.exe", npp, MAX_PATH);
    std::wstring cmd;
    std::error_code ec;
    if (fs::exists(code, ec)) cmd = std::format(L"\"{}\" -g \"{}:{}\"", code, file.native(), line);
    else if (fs::exists(npp, ec)) cmd = std::format(L"\"{}\" -n{} \"{}\"", npp, line, file.native());
    if (!cmd.empty())
    {
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return;
        }
    }
    ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

const ImVec4 kLevelColors[] = { { 0.6f, 0.6f, 0.6f, 1 }, { 0.9f, 0.9f, 0.9f, 1 }, { 1.0f, 0.8f, 0.3f, 1 }, { 1.0f, 0.4f, 0.4f, 1 } };
const ImVec4 kOk = { 0.45f, 0.85f, 0.45f, 1 };

} // namespace

void App::OpenInEditorAt(const fs::path& file, int line) { OpenFileAtLine(file, line); }

// ============================================================================================ lifecycle

bool App::Init(HWND hwnd, float dpiScale)
{
    hwnd_ = hwnd;
    dpi_ = dpiScale;
    logWindowReg_ = ScopedLogSink(&logWindow_);
    logFile_ = std::make_unique<FileLogSink>(paths_.Log());
    logFileReg_ = ScopedLogSink(logFile_.get());
    // the document announces a new / reloaded model: the watched files follow it
    events_.Subscribe([this](SessionEvent e) {
        if (e == SessionEvent::ModelLoaded) assets_.watchDirty = true;
        if (e == SessionEvent::AnimationChanged) { assets_.watchDirty = true; poseDirty_ = true; } // gestures, layers
        if (e == SessionEvent::LayerAdded && !session_.anim.layers.empty()) // Playback::AddLayer
        {
            scene_.LogLayerCoverage(session_.anim.layers.back());
            assets_.watchDirty = true;
            poseDirty_ = true;
            if (session_.anim.layers.size() == 1)
            {
                scene_.UpdatePose();
                camera_.Frame(scene_.boundsMin, scene_.boundsMax); // RTMs usually lift the model so the feet stand on y = 0
            }
        }
    });
    automation_.RegisterCommands();
    rt_.SetPresetDir(paths_.Presets()); // bone mapping presets per skeleton pair
    movesLoader_.SetCacheDir(paths_.Cache()); // CfgMoves graphs
    layers_.MarkProjectSaved(); // the default project: nothing to save yet

    std::string err;
    if (!renderer_.Init(hwnd, err))
    {
        Log::Get().Error("Renderer init failed: {}", err);
        MessageBoxA(hwnd, err.c_str(), "ArmaView", MB_ICONERROR);
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
#ifdef ARMAVIEW_DEV
    io.ConfigDragClickToInputText = true; // a click on a drag field (without moving) types a value
    io.ConfigDebugHighlightIdConflicts = true; // dev builds: a hovered control that shares its ID shows a red frame + tooltip
#endif
    // one settings file (IPaths::Data()): the ui values, the layout, named layouts, per-model visibility
    settings_.Load(); // false: first start, defaults
    Log::Get().Info("Settings: {} ({})", PathToUtf8(settings_.File()), paths_.Why());
    io.IniFilename = nullptr; // ImGui's layout goes into the settings file (LayoutStore)
    uiSettings_.Load(settings_);
    layouts_.LoadInitial();
    firstLayout_ = !layouts_.HasSavedLayout();

    shell_.systemDpi = dpiScale;
    ApplyUiScale();
    const char* fontPath = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (fs::exists(fontPath))
    {
        io.Fonts->AddFontFromFileTTF(fontPath, std::round(16.0f * dpi_), nullptr, io.Fonts->GetGlyphRangesCyrillic());
        // toolbar icons: Segoe Fluent Icons (Windows 11) or Segoe MDL2 Assets (Windows 10), merged into the UI font
        for (const char* iconPath : { "C:\\Windows\\Fonts\\SegoeIcons.ttf", "C:\\Windows\\Fonts\\segmdl2.ttf" })
            if (fs::exists(iconPath))
            {
                static const ImWchar iconRanges[] = { 0xE700, 0xF8FF, 0 };
                ImFontConfig cfg;
                cfg.MergeMode = true;
                cfg.PixelSnapH = true;
                cfg.GlyphOffset.y = std::round(2.0f * dpi_);
                shell_.hasIcons = io.Fonts->AddFontFromFileTTF(iconPath, std::round(15.0f * dpi_), &cfg, iconRanges) != nullptr;
                break;
            }
    }
    else
        io.Fonts->AddFontDefault();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(renderer_.Device(), renderer_.Context());

    scene_.RebuildGrid();
    RegisterHotkeys();
    texCache_.Init(renderer_.Device(), "P:\\");
    Log::Get().Info("ArmaView started: D3D11, Dear ImGui {}", IMGUI_VERSION);

    // Crew preview: BI's sample man (Arma 3 Samples, ArmaMan skeleton = the skeleton of the game's RTMs) if installed
    {
        // machine paths: armaview.local.ini (next to the exe or in the source tree; not in the repository)
        const LocalPaths& lp = GetLocalPaths();
        Log::Get().Info("Local paths: {}", lp.file.empty() ? std::string("armaview.local.ini not found (samples / Blender unset)") : PathToUtf8(lp.file));
        const std::string sample = lp.SampleMan();
        std::error_code ec;
        if (!sample.empty() && fs::exists(sample, ec)) session_.attach.crewModel = sample;
        if (!rt_.srcModel[0]) strncpy_s(rt_.srcModel, sample.c_str(), _TRUNCATE);
        if (!layers_.blenderExe[0]) strncpy_s(layers_.blenderExe, lp.blender.c_str(), _TRUNCATE);
        AddStandardMounts(lp); // \Samples_f\..., \a3\... on P:
        Log::Get().Info("Crew model: {}", session_.attach.crewModel);
    }

    // Game configs (unpacked game data on P:) in the background: base classes for config.cpp (Tank_F, HitPoints,
    // CfgMovesMaleSdr, ...). Only the root classes the viewer reads are kept.
    // --selftest runs on Arma 3 Samples and synthetic files only: the game data on P: is not read then.
    if (std::wcsstr(GetCommandLineW(), L"--selftest"))
        Log::Get().Info("selftest: game configs on P: not loaded (Samples and synthetic files only)");
    else
        gameLoad_ = std::async(std::launch::async, [cache = paths_.Cache() / "gameconfigs.bin"]() -> std::shared_ptr<ArmaConfig> {
            return LoadGameConfigs("P:\\", cache);
        });
    return true;
}

void App::PollGameConfig()
{
    if (!gameLoad_.valid() || gameLoad_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto cfg = gameLoad_.get();
    if (!cfg)
    {
        Log::Get().Warn("No unpacked game data (P:\\a3) - config.cpp inheritance stops at game classes");
        return;
    }
    session_.game = cfg;
    if (session_.hasModel) // re-resolve the model's config.cpp over the game configs
    {
        LoadConfigSkins();
        RebuildAttachments();
        scene_.RebuildLodGeometry();
    }
    if (!pendingState_.empty()) { playback_.LoadState(pendingState_); pendingState_.clear(); }
    movesLoader_.Rebuild();
    worlds_ = WorldLighting::Worlds(*session_.game);
    if (!pendingWorld_.empty()) { displayPanel_.SetWorld(pendingWorld_); pendingWorld_.clear(); }
}

void App::Shutdown()
{
    if (ImGui::GetCurrentContext()) layouts_.SaveNow(); // layout + ui values into the settings file
    graph_.Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    renderer_.Shutdown();
}

glm::mat4 App::ViewProj() const { return camera_.Projection(sceneW_ / std::max(1.0f, sceneH_)) * camera_.View(); }

void App::Frame()
{
    const auto frameStart = std::chrono::steady_clock::now();
    if (pendingW_ && pendingH_)
    {
        renderer_.Resize(pendingW_, pendingH_);
        pendingW_ = pendingH_ = 0;
    }
    logWindow_.Flush(); // entries from the background loader
    PollGameConfig();
    animation_.PollMovesBuild(false);
    PollModelLoad();                                            // a model read on the worker: applied now
    if (texCache_.PollStreamed()) { scene_.BuildDrawSections(); assets_.watchDirty = true; } // textures arriving
    if (layers_.batch.running) layers_.BatchStep(); // one RTM per frame: the Layers window shows the progress

    if (shell_.uiScaleDirty)
    {
        shell_.uiScaleDirty = false;
        ApplyUiScale();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    automation_.BeforeNewFrame();
    layouts_.BeforeNewFrame(); // a named / imported layout
    ImGui::NewFrame();
    ui::BeginFrame();
    recorder_.FrameSetup();
    automation_.AfterNewFrame();
    if (!firstRunChecked_)
    {
        // first start (no Started=1 in the settings, which are read by the first NewFrame): setup and paths, then the
        // start screen; later starts without a model: the start screen (unless turned off)
        firstRunChecked_ = true;
        start_.showAtStart = uiSettings_.startScreen;
        if (!automation_.Active())
        {
            if (!uiSettings_.startedSeen)
            {
                start_.setupOpen = true;
                uiSettings_.startedSeen = true; // automation runs leave it for the user's first start
                ImGui::MarkIniSettingsDirty();
            }
            else if (start_.showAtStart && !session_.hasModel && !ModelLoading())
                start_.open = true;
        }
    }
    lit_ = light_.Evaluate(camera_.Forward(), camera_.Up(), camera_.Right());
    if (world_.Loaded() && !light_.headlight) world_.Apply(lit_, light_.overcast, light_.sunIntensity, light_.ambientIntensity);
    {
        Perf::Scope ui("ui (incl. graph)");
        DrawUI();
    }
    Perf::Clock::time_point tInput = Perf::Clock::now();
    input_.Frame();
    const bool animating = session_.hasModel && !playback_.restPose && session_.anim.Active();
    if ((animating || movesSim_.drive) && playback_.playing) playback_.time += double(ImGui::GetIO().DeltaTime) * playback_.speed;
    {
        Perf::Scope ps("input:   moves / gesture / retarget preview");
        if (input_.Control(ImGui::GetIO().DeltaTime, control_, movesSim_, playback_, animation_)) poseDirty_ = true;
        { Perf::Scope a("input:     moves playback"); animation_.UpdateMovesPlayback(); }
        { Perf::Scope b("input:     gesture"); if (gesturePlayer_.Update(playback_.time)) poseDirty_ = true; }
        { Perf::Scope c("input:     retarget preview"); animation_.UpdateRetargetPreview(); }
        { Perf::Scope w("input:     weapon modes"); weaponScene_.Update(ImGui::GetIO().DeltaTime); }
        if (session_.anim.overrideRelative != (session_.blendMode == 1)) { session_.anim.overrideRelative = session_.blendMode == 1; poseDirty_ = true; }
    }
    sourceClock_ += ImGui::GetIO().DeltaTime;
    if (session_.hasModel && session_.attach.showCrew && session_.attach.Advance(ImGui::GetIO().DeltaTime)) poseDirty_ = true; // crew playback
    clock_ += ImGui::GetIO().DeltaTime;
    {
        Perf::Scope ps("input:   hot reload");
        assets_.Poll(clock_);
    }
    for (auto& s : session_.cfgSources)
        if (s.animate)
        {
            float k = 0.5f - 0.5f * std::cos(float(sourceClock_) * 2.0f * kPi / std::max(0.1f, sources_.oscillatePeriod));
            s.value = s.minValue + (s.maxValue - s.minValue) * k;
            poseDirty_ = true;
        }
    Perf::Get().Add("input + hot reload + playback", std::chrono::duration<double, std::milli>(Perf::Clock::now() - tInput).count());
    {
        Perf::Scope pose("pose: skinning + attachments");
        // re-pose only when something changed (a paused animation keeps its pose)
        const uint64_t sig = scene_.PoseSignature();
        if (poseDirty_ || animating != wasAnimating_ || (animating && (playback_.time != scene_.posedTime || sig != scene_.posedSig)))
        {
            scene_.UpdatePose();
            scene_.posedTime = playback_.time;
            scene_.posedSig = sig;
        }
    }
    wasAnimating_ = animating;
    poseDirty_ = false;
    {
        Perf::Scope ov("overlay + ImGui::Render");
        DrawSceneOverlay();
        ImGui::Render();
    }

    float clear[4] = { display_.background[0], display_.background[1], display_.background[2], 1.0f };
    if (display_.backgroundFromSky && !light_.headlight)
        for (int k = 0; k < 3; ++k) clear[k] = lit_.background[k];
    renderer_.BeginFrame(clear);

    SceneDraw s;
    s.viewProj = ViewProj();
    s.cameraPos = camera_.Position();
    s.lightDir = lit_.lightDir;
    s.lightColor = lit_.lightColor;
    s.ambientColor = lit_.skyColor;
    s.groundColor = lit_.groundColor;
    s.exposure = lit_.exposure;
    s.viewportX = sceneX_;
    s.viewportY = sceneY_;
    s.viewportW = sceneW_;
    s.viewportH = sceneH_;
    s.shading = display_.shading;
    s.wire = display_.wire;
    s.cull = display_.cull;
    s.twoSided = display_.twoSided;
    s.flipNormalGreen = display_.flipNormalGreen;
    s.reflections = display_.reflections;
    s.shadows = display_.shadows && session_.hasModel && !display_.skeletonOnly;
    const float floorY = scene_.FloorY();
    if (floorY != scene_.gridFloor) scene_.RebuildGrid(); // animations started / stopped: the grid moves to the floor
    if (s.shadows)
    {
        // orthographic sun camera around the posed model + attachments, down to the floor
        glm::vec3 lo = scene_.shadowMin, hi = scene_.shadowMax;
        lo.y = std::min(lo.y, floorY);
        const glm::vec3 c = (lo + hi) * 0.5f;
        // the size only changes when the pose leaves it or it is far too big (a refit every frame makes the shadow shimmer)
        const float need = std::max(0.5f, glm::length(hi - lo) * 0.5f) * 1.1f;
        if (need > scene_.shadowR || need < scene_.shadowR * 0.7f) scene_.shadowR = std::ceil(need * 1.1f / 0.25f) * 0.25f;
        const float r = scene_.shadowR;
        glm::vec3 d = glm::normalize(lit_.lightDir);
        if (d.y > -0.05f) d = glm::normalize(glm::vec3(d.x, -0.05f, d.z)); // sun at the horizon: keep it above
        const glm::vec3 up = std::abs(d.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        // the centre snapped to whole shadow-map texels across the sun direction: a moving model does not shimmer
        static const int kSizes[3] = { 1024, 2048, 4096 };
        const float texel = 2.0f * r / float(kSizes[std::clamp(display_.shadowQuality, 0, 2)]);
        const glm::mat4 rot = glm::lookAtLH(glm::vec3(0), d, up);
        glm::vec3 lc = glm::vec3(rot * glm::vec4(c, 1.0f));
        lc.x = std::floor(lc.x / texel) * texel;
        lc.y = std::floor(lc.y / texel) * texel;
        const glm::vec3 cs = glm::vec3(glm::inverse(rot) * glm::vec4(lc, 1.0f));
        const glm::mat4 view = glm::lookAtLH(cs - d * r * 2.0f, cs, up);
        s.lightViewProj = glm::orthoLH_ZO(-r, r, -r, r, 0.0f, r * 4.0f) * view;
        s.shadowSoftness = display_.shadowSoftness;
        s.shadowQuality = display_.shadowQuality;
        s.groundY = floorY;
        s.groundSize = r;
        s.groundCenter = { c.x, c.z };
    }
    s.drawMesh = session_.hasModel && !display_.skeletonOnly;
    s.drawLayer[size_t(LineLayer::Grid)] = display_.showGrid && !display_.skeletonOnly;
    s.drawLayer[size_t(LineLayer::Normals)] = display_.showNormals && session_.hasModel;
    s.drawLayer[size_t(LineLayer::Skeleton)] = display_.showSkeleton && session_.hasModel;
    s.drawLayer[size_t(LineLayer::Trails)] = animView_.showTrail && session_.hasModel;
    s.drawLayer[size_t(LineLayer::LodOverlay)] = session_.hasModel && !vis_.overlays.empty();
    s.drawLayer[size_t(LineLayer::Retarget)] = rt_.previewOn && rt_.previewMode > 0;
    s.drawLayer[size_t(LineLayer::Footsteps)] = control_.on;
    s.drawLayer[size_t(LineLayer::Weapon)] = weaponScene_.Active() && session_.hasModel;
    s.layerOnTop[size_t(LineLayer::Weapon)] = true;
    s.layerOnTop[size_t(LineLayer::Retarget)] = true;
    s.layerOnTop[size_t(LineLayer::Skeleton)] = display_.skeletonOnTop;
    renderer_.DrawScene(s);
    if (points_.WantsDepth()) renderer_.CaptureDepth(); // read by the point overlay next frame
    if (recorder_.Recording()) recorder_.FrameCapture(s, clear);

    {
        Perf::Scope im("render: ImGui draw data");
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    uiSettings_.startScreen = start_.showAtStart;
    layouts_.SaveIfWanted(); // ImGui asked to save the layout: the settings file
    if (logFile_) logFile_->Flush();

    automation_.AfterRender();
    frameCpuMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
    Perf::Get().Add("FRAME TOTAL (CPU, without Present)", frameCpuMs_);
    Perf::Get().EndFrame();
    if (!firstFrameLogged_)
    {
        firstFrameLogged_ = true;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime_).count();
        Perf::Get().Note("startup: process start -> first frame", ms);
        Log::Get().Info("Startup: first frame after {:.0f} ms", ms);
    }
    if (profileLog_ && ++profileFrames_ >= 120)
    {
        Log::Get().Info("profile: {}", Perf::Get().FrameReport());
        Perf::Get().ResetFrames();
        profileFrames_ = 0;
    }
    renderer_.Present(true);
}


// ============================================================================================ loading


// Opening a model: the file is read and parsed on a worker (a 100 MB MLOD: ~1 s, the window keeps drawing); the rest
// (model.cfg, skeleton, view) is applied on the UI thread when it is ready. Textures then stream in.
void App::OpenModel(const fs::path& path, bool keepView)
{
    if (modelJob_.valid()) modelJob_.wait(); // one at a time (the previous one is applied first)
    PollModelLoad();
    logWindow_.Flush();
    logMark_ = logWindow_.Entries().size();
    Log::Get().Info("Loading '{}'", PathToUtf8(path));
    modelJobPath_ = path;
    modelJobKeep_ = keepView;
    modelJobStart_ = std::chrono::steady_clock::now();
    const int lod = keepView ? session_.lod : 0;
    modelJob_ = std::async(std::launch::async, [this, path, lod]() -> LoadedModel {
        LoadedModel r;
        r.model = std::make_unique<P3DModel>();
        if (!Session::ReadModel(files_, path, false, *r.model))
        {
            r.model.reset();
            return r;
        }
        // the proxy models of the LOD that will be shown, loaded here too (merged into the attachments' cache)
        PerfNoteScope ps("load:   proxy models (worker)");
        const P3DModel& m = *r.model;
        const P3DLod& l = m.lods[size_t(std::clamp(lod, 0, int(m.lods.size()) - 1))];
        const fs::path root = ArmaProjectRoot(path);
        for (const auto& f : ModelCache::ProxyFiles(l, root)) r.proxies.Get(f, false, root);
        return r;
    });
}

bool App::PollModelLoad()
{
    if (!modelJob_.valid() || modelJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    LoadedModel r = modelJob_.get();
    if (!r.model) return false; // logged; the previous model stays on screen
    session_.attach.Cache().Merge(std::move(r.proxies));
    ApplyModel(std::move(*r.model), modelJobPath_, modelJobKeep_);
    lastLoadMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - modelJobStart_).count();
    return true;
}

// hot reload: read here (it needs the result: a half-written file is retried)
bool App::LoadModel(const fs::path& path, bool keepView, bool requireComplete)
{
    logWindow_.Flush();
    logMark_ = logWindow_.Entries().size();
    const auto loadStart = std::chrono::steady_clock::now();
    Log::Get().Info("Loading '{}'", PathToUtf8(path));
    P3DModel m;
    if (!Session::ReadModel(files_, path, requireComplete, m)) return false; // keep the previous model on screen
    ApplyModel(std::move(m), path, keepView);
    lastLoadMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStart).count();
    return true;
}

void App::ApplyModel(P3DModel&& m, const fs::path& path, bool keepView)
{
    start_.open = false; // a model is open: the start screen has done its job

    // State kept across F5 / hot reload
    const std::string keepBone = keepView && scene_.selectedBone >= 0 && scene_.selectedBone < int(session_.skeleton.bones.size()) ? session_.skeleton.bones[scene_.selectedBone].name : "";
    const bool keepHighlightSel = scene_.highlight == SceneView::Highlight::Selection;
    std::vector<CfgSource> keepSources = keepView ? session_.cfgSources : std::vector<CfgSource>{};
    const int keepSection = inspector_.section;

    session_.SetModel(std::move(m), keepView); // model, model.cfg, skeleton, animations bound; logged
    // glTF clips: layers of their model (a reload replaces them); the first one plays, the others wait switched off
    std::erase_if(session_.anim.layers, [](const AnimLayer& l) { return l.anim && l.anim->info.signature == "glTF"; });
    for (size_t i = 0; i < session_.model.importedClips.size(); ++i)
    {
        const ImportedClip& c = session_.model.importedClips[i];
        if (playback_.AddLayer(ImportedClipAnimation(session_.model, c), std::max(c.duration, 1.0f / 30.0f)) && i > 0)
            session_.anim.layers.back().enabled = false;
    }
    session_.anim.Bind(session_.skeleton);
    // the floor without animations (FloorY): the lowest rest point of LOD 0, not the posed bounds (they bob with a walk)
    scene_.restFloor = session_.RestFloor();
    scene_.shadowR = 0.0f;
    scene_.RebuildGrid();
    texCache_.SetProjectRoot(session_.projectRoot);
    // textures / materials whose files are unchanged stay loaded (reload, hot reload, next model of the same addon)
    if (int dropped = texCache_.DropChanged()) Log::Get().Debug("Texture cache: {} changed / missing entr(y/ies) dropped", dropped);
    mats_.DropChanged();
    inspector_.section = -1;
    if (!keepView)
    {
        scene_.selection = scene_.selectedBone = -1;
        scene_.highlight = SceneView::Highlight::None;
    }

    {
        PerfNoteScope ps("load: config.cpp (skins, hitpoints)");
        LoadConfigSkins();
    }
    pendingCoverageLog_ = true;
    for (const auto& ks : keepSources)
        for (auto& s : session_.cfgSources)
            if (IEquals(s.name, ks.name)) { s.value = std::clamp(ks.value, s.minValue, s.maxValue); s.animate = ks.animate; }
    if (!keepBone.empty())
        if (int b = session_.skeleton.Find(keepBone); b >= 0)
        {
            scene_.selectedBone = b;
            scene_.highlight = keepHighlightSel ? SceneView::Highlight::Selection : scene_.highlight;
        }

    // Validate every visual/view LOD once for the log; the window shows the current LOD.
    LogAllLodValidation(session_);

    SetWindowTextW(hwnd_, (L"ArmaView — " + path.native()).c_str());
    if (!keepView) scene_.pickFaces.clear();
    vis_.Load(settings_, path);
    if (movesSim_.drive && !keepView) animation_.SetMovesDrive(false);
    movesLoader_.Rebuild();
    {
        PerfNoteScope ps("load: LOD geometry, materials, proxies, crew (incl. textures)");
        OnLodChanged();
    }
    if (pendingCoverageLog_)
    {
        for (const auto& l : session_.anim.layers) scene_.LogLayerCoverage(l);
        pendingCoverageLog_ = false;
    }
    scene_.LogCfgAnimations();
    scene_.LogMaterials();
    if (!keepView) camera_.Frame(scene_.boundsMin, scene_.boundsMax);
    else if (keepSection >= 0 && keepSection < int(scene_.meshSections.size())) inspector_.section = keepSection;
    if (!assets_.rtmFolder.empty()) assets_.RunRtmCheck();
    assets_.watchDirty = true;

}

// ============================================================================================ hot reload





void App::OpenAnimInEditor(const CfgAnimation& a) const
{
    if (fs::path f = session_.cfg.FileOf(a.file); !f.empty()) OpenInEditor(f, a.line);
}

void App::RebuildGraph()
{
    graphDirty_ = false;
    std::vector<AnimStatus> status;
    status.reserve(session_.cfgAnims.size());
    for (const auto& a : session_.cfgAnims) status.push_back(AnimStatusOf(a));
    std::vector<size_t> pts(session_.skeleton.bones.size(), 0);
    for (size_t b = 0; b < pts.size() && b < scene_.boneSelection.size(); ++b)
        if (scene_.boneSelection[b] >= 0) pts[b] = session_.model.lods[session_.lod].selections[scene_.boneSelection[b]].pointCount;
    graph_.Rebuild(paths_.Graphs() / (PathToUtf8(session_.model.path.stem()) + ".graph"), session_.skeleton, session_.cfgAnims, session_.cfgSources, status, pts);
}


void App::OnLodChanged()
{
    if (!session_.hasModel) return;
    PerfNoteScope psPrep("load:   OnLodChanged (all of the below + pivots, validation)");
    graphDirty_ = true;
    inspector_.LodChanged(); // Inspector LOD summary
    scene_.pickFaces.clear();
    points_.Invalidate();
    scene_.PrepareLod(sources_.memoryAxisFallback); // pivots, axes, bone selections, validation, mesh / proxy points
    {
        PerfNoteScope ps("load:   proxies + crew (models, RTM)");
        RebuildAttachments();
    }
    {
        PerfNoteScope ps("load:   LOD mesh, materials, GPU buffers");
        scene_.RebuildLodGeometry();
    }
    scene_.RebuildSkeletonLines();
    scene_.RebuildLodOverlay();
}

void App::RebuildAttachments()
{
    if (!session_.hasModel) return;
    struct DamageReload { App* a; ~DamageReload() { a->scene_.LoadDamageMaterials(); } } damageReload{ this }; // after the vehicle class is known
    std::error_code ec;
    {
        PerfNoteScope ps("load:     proxy slots");
        session_.attach.BuildSlots(session_.model.lods[session_.lod], CrewConfig(), session_.projectRoot);
    }
    const P3DLod* memory = nullptr;
    for (const auto& l : session_.model.lods)
        if (LodResolutionName(l.resolution) == "Memory") memory = &l;
    session_.attach.SetMemoryLod(memory);
    {
        PerfNoteScope ps("load:     crew seats");
        session_.attach.BuildCrew(CrewConfig(), ArmaPathOf(session_.model.path, session_.projectRoot), session_.projectRoot);
    }
    weaponScene_.ApplySlotItems(); // Optics mode: chosen items on the weapon slot proxies
    ApplyUnit();
    assets_.watchDirty = true;
}

// ============================================================================================ unit

void App::ApplyUnit()
{
    Unit& u = session_.unit;
    session_.attach.unitParts.clear();
    if (!u.Active() || !session_.hasModel) return;
    const ArmaConfig* cfg = CrewConfig();
    std::error_code ec;
    if (u.Outfit())
    {
        // the body is the open model; another model opened: the outfit is put away
        if (!fs::equivalent(u.OutfitBody(), session_.model.path, ec))
        {
            Log::Get().Info("Unit: outfit put away - {} was opened, the body is {}", PathToUtf8(session_.model.path.filename()), PathToUtf8(u.OutfitBody().filename()));
            u.Clear();
            pendingOutfit_.reset();
            return;
        }
        if (pendingOutfit_) { u.ApplyPresetParts(cfg, *pendingOutfit_); pendingOutfit_.reset(); }
        static const ArmaConfig kNone;
        PerfNoteScope ps("load:     unit parts");
        u.Load(cfg ? *cfg : kNone, session_.skeleton, session_.projectRoot);
        u.Publish(session_.attach);
        return;
    }
    if (!cfg) return;
    if (NormalizeArmaPath(u.ModelPath()) != NormalizeArmaPath(ArmaPathOf(session_.model.path, session_.projectRoot)))
    {
        Log::Get().Info("Unit: {} put away - {} was opened, its model is {}", u.Class(), PathToUtf8(session_.model.path.filename()), u.ModelPath());
        u.Clear();
        return;
    }
    // the open model's addon config may differ from the one the class was read from (its model opened since)
    if (cfg != unitCfg_ && FindConfigClass(*cfg, "CfgVehicles", u.Class()))
    {
        u.Read(*cfg, u.Class());
        unitCfg_ = cfg;
    }
    if (pendingOutfit_) { u.ApplyPresetParts(cfg, *pendingOutfit_); pendingOutfit_.reset(); }
    {
        PerfNoteScope ps("load:     unit parts");
        u.Load(*cfg, session_.skeleton, session_.projectRoot);
    }
    u.Publish(session_.attach);
    // the uniform's hidden selections: the class's skin on the body
    const ConfigSkin& skin = u.UniformSkin();
    if (!skin.selections.empty() && (!skin.textures.empty() || !skin.materials.empty()))
    {
        int i = 0;
        while (i < int(session_.configSkins.size()) && !IEquals(session_.configSkins[size_t(i)].className, skin.className)) ++i;
        if (i == int(session_.configSkins.size())) session_.configSkins.push_back(skin);
        session_.activeSkin = i;
    }
}

void App::AssembleUnit(const std::string& cls)
{
    auto& log = Log::Get();
    Unit& u = session_.unit;
    if (cls.empty() || cls == "off")
    {
        u.Clear();
        pendingOutfit_.reset();
        session_.attach.unitParts.clear();
        if (session_.hasModel) scene_.RebuildLodGeometry();
        return;
    }
    const ArmaConfig* cfg = CrewConfig();
    if (!cfg)
    {
        log.Error("Unit: no config loaded (open a model of a mod, or wait for the game configs)");
        return;
    }
    if (!u.Read(*cfg, cls)) return;
    unitCfg_ = cfg;
    std::string m = u.ModelPath();
    if (!ToLower(m).ends_with(".p3d")) m += ".p3d";
    const fs::path file = ResolveArmaPath(m, session_.projectRoot.empty() ? fs::path(L"P:\\") : session_.projectRoot);
    std::error_code ec;
    if (!session_.hasModel || !fs::equivalent(file, session_.model.path, ec))
    {
        if (!fs::exists(file, ec))
        {
            log.Error("Unit: the model of {} is not on disk: {}", u.Class(), PathToUtf8(file));
            u.Clear();
            return;
        }
        log.Info("Unit: opening {} (model of {})", PathToUtf8(file), u.Class());
        OpenModel(file, false); // dressed once loaded (RebuildAttachments -> ApplyUnit)
        return;
    }
    ApplyUnit();
    scene_.RebuildLodGeometry();
}

void App::ReplaceUnitPart(UnitSlot slot, const std::string& cls)
{
    const ArmaConfig* cfg = CrewConfig();
    if (!cfg || !session_.unit.Active()) return;
    session_.unit.Replace(*cfg, slot, cls);
    ApplyUnit();
    scene_.RebuildLodGeometry();
}

// ---- outfit (try-on): any P3D as the body, P3D files as parts, presets in <settings>\outfits

void App::OutfitBody(const fs::path& p3d)
{
    std::error_code ec;
    if (p3d.empty() || !fs::exists(p3d, ec))
    {
        Log::Get().Error("Outfit: body {} not on disk", PathToUtf8(p3d));
        return;
    }
    Unit::Preset keep;
    const bool had = session_.unit.Active();
    if (had) keep = session_.unit.MakePreset(); // the parts stay on the new body
    session_.unit.StartOutfit(fs::absolute(p3d, ec));
    if (had && !keep.parts.empty()) pendingOutfit_ = keep;
    if (!session_.hasModel || !fs::equivalent(p3d, session_.model.path, ec))
    {
        OpenModel(p3d, false); // dressed once loaded (RebuildAttachments -> ApplyUnit)
        return;
    }
    ApplyUnit();
    scene_.RebuildLodGeometry();
}

fs::path App::SamplesArmaMan() const
{
    const LocalPaths& lp = GetLocalPaths();
    if (lp.samples.empty()) return {};
    return fs::path(Utf8ToWide(lp.samples)) / L"Test_Character_01" / L"A3_character_example.p3d";
}

void App::AddUnitFile(UnitSlot slot, const fs::path& p3d)
{
    Unit& u = session_.unit;
    if (!u.Active())
    {
        if (!session_.hasModel) { Log::Get().Error("Outfit: open a body first"); return; }
        u.StartOutfit(session_.model.path);
    }
    u.AddFile(slot, p3d);
    ApplyUnit();
    scene_.RebuildLodGeometry();
}

void App::RemoveUnitPart(size_t index)
{
    session_.unit.RemovePart(index);
    UnitPartsChanged();
}

fs::path App::PickP3d()
{
    return OpenFileDialog(hwnd_, L"P3D models (*.p3d)\0*.p3d\0All files\0*.*\0", session_.hasModel ? session_.model.path.parent_path() : fs::path(L"P:\\"));
}

fs::path App::OutfitFolder() const { return paths_.Data() / L"outfits"; }

std::vector<std::string> App::OutfitPresets() const
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(OutfitFolder(), ec))
        if (e.path().extension() == L".outfit") out.push_back(PathToUtf8(e.path().stem()));
    std::sort(out.begin(), out.end());
    return out;
}

static bool OutfitNameOk(const std::string& name)
{
    return !name.empty() && name.find_first_of("\\/:*?\"<>|") == std::string::npos && name != "." && name != "..";
}

bool App::SaveOutfitPreset(const std::string& name)
{
    if (!OutfitNameOk(name) || !session_.unit.Active()) { Log::Get().Error("Outfit: cannot save \"{}\" (a name without \\ / : * ? \" < > |, and an outfit)", name); return false; }
    std::error_code ec;
    fs::create_directories(OutfitFolder(), ec);
    const fs::path f = OutfitFolder() / (Utf8ToWide(name) + L".outfit");
    if (fs::exists(f, ec)) fs::copy_file(f, fs::path(f).concat(L".bak"), fs::copy_options::overwrite_existing, ec); // the previous one
    std::ofstream o(f, std::ios::binary);
    o << Unit::SavePreset(session_.unit.MakePreset());
    if (!o) { Log::Get().Error("Outfit: cannot write {}", PathToUtf8(f)); return false; }
    Log::Get().Info("Outfit: preset {} saved ({})", name, PathToUtf8(f));
    return true;
}

bool App::LoadOutfitPreset(const std::string& name)
{
    const fs::path f = OutfitFolder() / (Utf8ToWide(name) + L".outfit");
    std::ifstream i(f, std::ios::binary);
    if (!i) { Log::Get().Error("Outfit: no preset {}", PathToUtf8(f)); return false; }
    const std::string text((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
    Unit::Preset p;
    std::string err;
    if (!Unit::ParsePreset(text, p, err)) { Log::Get().Error("Outfit: {}: {}", PathToUtf8(f), err); return false; }
    Log::Get().Info("Outfit: preset {} - body {}, {} part(s)", name, p.bodyClass.empty() ? PathToUtf8(p.bodyFile) : p.bodyClass, p.parts.size());
    pendingOutfit_ = p;
    if (!p.bodyClass.empty())
    {
        AssembleUnit(p.bodyClass); // dresses at once if the model is open, else once it is loaded
        if (!session_.unit.Active()) pendingOutfit_.reset();
        return session_.unit.Active();
    }
    session_.unit.StartOutfit(p.bodyFile);
    std::error_code ec;
    if (!session_.hasModel || !fs::equivalent(p.bodyFile, session_.model.path, ec))
    {
        if (!fs::exists(p.bodyFile, ec)) { Log::Get().Error("Outfit: body {} not on disk", PathToUtf8(p.bodyFile)); session_.unit.Clear(); pendingOutfit_.reset(); return false; }
        OpenModel(p.bodyFile, false);
        return true;
    }
    ApplyUnit();
    scene_.RebuildLodGeometry();
    return true;
}

// the recycle bin, not a hard delete (the user can get it back)
static bool MoveToRecycleBin(const fs::path& f)
{
    std::error_code ec;
    if (!fs::exists(f, ec)) return false;
    std::wstring from = fs::absolute(f, ec).native();
    from.push_back(L'\0'); // double-terminated list
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

bool App::DeleteOutfitPreset(const std::string& name)
{
    const fs::path f = OutfitFolder() / (Utf8ToWide(name) + L".outfit");
    if (!OutfitNameOk(name) || !MoveToRecycleBin(f)) { Log::Get().Error("Outfit: cannot delete {}", PathToUtf8(f)); return false; }
    Log::Get().Info("Outfit: preset {} moved to the recycle bin", name);
    return true;
}

void App::SetControl(bool on)
{
    if (on) animation_.EnsureMoves(true);
    animation_.SetControlMode(on);
}

// ============================================================================================ setup and paths

IStartHost::Setup App::SetupState() const
{
    Setup s;
    s.dataDir = PathToUtf8(paths_.Data());
    s.dataWhy = paths_.Why();
    std::error_code ec;
    s.pDrive = fs::exists(L"P:\\", ec);
    if (wchar_t target[MAX_PATH] = {}; QueryDosDeviceW(L"P:", target, MAX_PATH))
    {
        std::wstring t = target;
        if (t.starts_with(L"\\??\\")) t = t.substr(4); // SUBST: \??\C:\folder
        s.pTarget = PathToUtf8(fs::path(t));
    }
    s.a3 = fs::is_directory(L"P:\\a3", ec);
    s.gameConfigs = session_.game ? std::format("{} config file(s) read", session_.game->Files().size())
                  : GameConfigsLoading() ? std::string(T("reading the game configs...")) : std::string();
    const LocalPaths& lp = GetLocalPaths();
    s.localIni = lp.file.empty() ? "" : PathToUtf8(lp.file);
    s.samples = lp.samples;
    s.samplesOk = !lp.samples.empty() && fs::is_directory(fs::path(Utf8ToWide(lp.samples)), ec);
    s.blender = layers_.blenderExe;
    s.blenderOk = !s.blender.empty() && fs::exists(fs::path(Utf8ToWide(s.blender)), ec);
    s.ffmpeg = record_.settings.ffmpeg;
    if (!s.ffmpeg.empty())
    {
        const std::wstring f = Utf8ToWide(s.ffmpeg);
        wchar_t found[MAX_PATH] = {};
        s.ffmpegOk = fs::exists(fs::path(f), ec) ||
                     (f.find_first_of(L"\\/") == std::wstring::npos && SearchPathW(nullptr, f.c_str(), L".exe", MAX_PATH, found, nullptr) > 0); // on PATH
    }
    return s;
}

void App::PickSamples()
{
    fs::path dir = PickFolderDialog(hwnd_, L"Arma 3 Samples folder (...\\Arma 3 Samples or its Addons folder)");
    if (dir.empty()) return;
    std::error_code ec;
    if (fs::is_directory(dir / "Addons", ec)) dir /= "Addons"; // the samples' models live under Addons
    const fs::path ini = paths_.Data() / "armaview.local.ini";
    if (SaveLocalPath(ini, "samples", PathToUtf8(dir))) Log::Get().Info("Setup: Arma 3 Samples = {} (saved in {}; used after a restart)", PathToUtf8(dir), PathToUtf8(ini));
    else Log::Get().Error("Setup: cannot write {}", PathToUtf8(ini));
}

void App::PickBlender()
{
    const fs::path exe = OpenFileDialog(hwnd_, L"blender.exe\0blender.exe\0Programs (*.exe)\0*.exe\0", fs::path(L"C:\\Program Files"));
    if (exe.empty()) return;
    strncpy_s(layers_.blenderExe, PathToUtf8(exe).c_str(), _TRUNCATE);
    const fs::path ini = paths_.Data() / "armaview.local.ini";
    if (SaveLocalPath(ini, "blender", PathToUtf8(exe))) Log::Get().Info("Setup: Blender = {} (saved in {})", PathToUtf8(exe), PathToUtf8(ini));
    else Log::Get().Error("Setup: cannot write {}", PathToUtf8(ini));
}

void App::DrawUnitWindow()
{
    // a layout saved before this window existed: it joins the Log's tabs instead of floating
    if (ImGuiWindow* log = ImGui::FindWindowByName(WinName("Log").c_str()); log && log->DockId)
        ImGui::SetNextWindowDockID(log->DockId, ImGuiCond_FirstUseEver);
    unit_.Draw(WinName("Unit"));
}

void App::UnitPartsChanged()
{
    session_.unit.Publish(session_.attach);
    scene_.RebuildLodGeometry();
}


void App::ShowOpenDialog()
{
    const fs::path f = OpenFileDialog(hwnd_, L"Supported files (*.p3d;*.rtm;*.gltf;*.glb)\0*.p3d;*.rtm;*.gltf;*.glb\0P3D models (*.p3d)\0*.p3d\0glTF (*.gltf;*.glb)\0*.gltf;*.glb\0All files\0*.*\0",
                                      session_.hasModel ? session_.model.path.parent_path() : fs::path(L"P:\\"));
    if (!f.empty()) OpenFile(f);
}

std::wstring App::PickFolder(const wchar_t* title) { return PickFolderDialog(hwnd_, title).native(); }

// ============================================================================================ closing





// ============================================================================================ recording






void App::DrawRecordWindow() { record_.Draw(); }

// ============================================================================================ Mod check



void App::DrawModCheckWindow() { modCheck_.Draw(dpi_); }

void App::ShowFolderDialog()
{
    if (fs::path p = PickFolderDialog(hwnd_, L"Folder with RTM files"); !p.empty())
    {
        assets_.rtmFolder = p;
        assets_.RunRtmCheck();
    }
}

// ============================================================================================ geometry


const RvMaterial* App::GetRvMaterial(const std::string& name) { return mats_.Get(name); }


std::string App::PickTexture(const std::string& current)
{
    const fs::path f = OpenFileDialog(hwnd_, L"Textures (*.paa;*.tga;*.png)\0*.paa;*.tga;*.png\0All files\0*.*\0",
                                      current.empty() ? session_.projectRoot : ResolveArmaPath(current, session_.projectRoot).parent_path());
    return f.empty() ? std::string() : ArmaPathOf(f, session_.projectRoot); // Arma path (relative to P:\ or a mount)
}



void App::LoadConfigSkins()
{
    if (!session_.LoadAddonConfig())
    {
        points_.SetHitPoints({}, "", session_.model);
        return;
    }
    std::string vehicle;
    auto hits = FindHitPoints(*session_.modCfg, ArmaPathOf(session_.model.path, session_.projectRoot), vehicle);
    points_.SetHitPoints(std::move(hits), vehicle, session_.model);
}





// ============================================================================================ animation






int App::PrimaryLayer() const { return playback_.PrimaryLayer(); }


// Everything besides the time that changes the RTM pose (layer list and settings, missing-bone mode, rest pose).


// Onion skin (previous / next frame of the primary layer) and the B pose of the A/B compare, as tinted ghosts.

// Path of the selected bone over one cycle of the primary layer; segments much longer than usual = jumps (red).


void App::DrawAnimationPanel() { timeline_.Draw(WinName("Timeline")); }

void App::DrawSourcesPanel() { sources_.Draw(WinName("Sources")); }

void App::DrawMaterialPanel() { materials_.Draw(WinName("Materials"), [this] { surveyPanel_.Draw(); }); }

void App::SelectSection(int section)
{
    inspector_.section = section;
    inspector_.previewStage = -1;
    inspector_.what = InspectorPanel::Kind::Section;
}




void App::SetView(float yaw, float pitch) { input_.SetView(yaw, pitch); }

// ============================================================================================ UI

void App::DrawUI()
{
    DrawMenu();
    DrawModeBar();   // mode tabs + toolbar (top side bar)
    DrawStatusBar(); // bottom side bar
    const ImGuiID dock = DrawDockHost();
    if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dock))
    {
        ImVec2 origin = ImGui::GetMainViewport()->Pos;
        sceneX_ = central->Pos.x - origin.x;
        sceneY_ = central->Pos.y - origin.y;
        sceneW_ = std::max(1.0f, central->Size.x);
        sceneH_ = std::max(1.0f, central->Size.y);
    }
    showCrewWindow_ = shell_.mode == UiMode::Vehicle;

    {
    Perf::Scope panelsScope("ui:   panels of the mode + inspector");
    switch (shell_.mode)
    {
    case UiMode::Model:
        DrawOutliner();
        DrawUnitWindow();
        DrawLogPanel();
        break;
    case UiMode::Animation:
        DrawOutliner();
        DrawAnimationPanel();
        DrawMovesPanel();
        DrawMovesEditor();
        DrawRetargetPanel();
        DrawLayersPanel();
        DrawSourcesPanel();
        DrawUnitWindow();
        DrawLogPanel();
        break;
    case UiMode::Vehicle:
        DrawCrewWindow();
        DrawSourcesPanel();
        DrawLogPanel();
        break;
    case UiMode::Materials:
        DrawMaterialPanel();
        DrawLogPanel();
        break;
    case UiMode::Validation:
        DrawValidationPanel();
        DrawLogPanel();
        break;
    case UiMode::Reload:
        DrawOutliner();
        DrawReloadPanel();
        DrawSourcesPanel();
        DrawLogPanel();
        break;
    case UiMode::Optics:
        DrawOutliner();
        DrawOpticsPanel();
        DrawLogPanel();
        break;
    default: break;
    }
    DrawInspector();
    }
    if (shell_.focusFirstTab > 0)
    {
        // freshly built layout: the first tab of the bottom panel in front, then no window keeps the keyboard
        if (--shell_.focusFirstTab == 1)
            ImGui::SetWindowFocus(WinName(shell_.mode == UiMode::Animation ? "Timeline" : shell_.mode == UiMode::Vehicle ? "Crew" : shell_.mode == UiMode::Reload ? "Reload sync"
                                          : shell_.mode == UiMode::Optics ? "Optics" : "Log").c_str());
        else if (shell_.focusFirstTab == 0)
            ImGui::SetWindowFocus(nullptr);
    }
    DrawHelpWindow();
    DrawAboutWindow();
    DrawGettingStarted();
    start_.Draw(dpi_);
    DrawModCheckWindow();
    DrawRecordWindow();
    closePrompt_.Draw();

    // model.cfg graph (not processed at all while closed)
    if (showGraph_)
    {
        Perf::Scope graphScope("ui:   model.cfg graph");
        ImGui::SetNextWindowSize(ImVec2(1150 * dpi_, 720 * dpi_), ImGuiCond_FirstUseEver);
        if (ui::Begin("model.cfg graph", &showGraph_))
        {
            ui::HelpMarker("model.cfg graph: sources -> animations -> bones.\n"
                           "1. Left: animation sources (sliders move them); middle: CfgModels animations; right: skeleton bones.\n"
                           "2. Click a bone: it is selected in the viewport; double click a node: only its links (depth 1-3), Esc leaves.\n"
                           "3. Open in model.cfg jumps to the animation in the editor.\n"
                           "4. Wheel zooms, right drag pans, the minimap jumps.");
            ImGui::SameLine();
            if (!session_.hasModel || session_.cfgAnims.empty())
                ui::EmptyState(session_.hasModel ? "This model has no CfgModels animations (model.cfg class of the model without Animations)." : "Open a model with a model.cfg (Ctrl+O): its animations, sources and bones are drawn here.");
            else
            {
                if (graphDirty_) RebuildGraph();
                CfgGraph::Actions act = graph_.Draw(session_.cfgSources, scene_.selectedBone);
                if (act.selectBone >= 0 && act.selectBone != scene_.selectedBone) SelectBone(act.selectBone);
                if (act.sourcesChanged) poseDirty_ = true;
                if (act.openAnim >= 0 && act.openAnim < int(session_.cfgAnims.size())) OpenAnimInEditor(session_.cfgAnims[act.openAnim]);
            }
        }
        ImGui::End();
    }
    DrawStatesWindow();
    DrawCommandPalette();
}



void App::DrawSkeletonPanel() { skeletonPanel_.Draw(); } // Outliner of the Skeleton & Anim mode


void App::HighlightProblem(PointProblem p)
{
    static constexpr SceneView::Highlight kMap[] = { SceneView::Highlight::None, SceneView::Highlight::TooManyBones, SceneView::Highlight::BadWeightSum, SceneView::Highlight::NoBone };
    scene_.highlight = kMap[int(p)];
    if (p != PointProblem::None) scene_.selection = scene_.selectedBone = -1;
    scene_.RebuildLodGeometry();
    scene_.RebuildSkeletonLines();
}

IValidationView::PointProblem App::HighlightedProblem() const
{
    switch (scene_.highlight)
    {
    case SceneView::Highlight::TooManyBones: return PointProblem::TooManyBones;
    case SceneView::Highlight::BadWeightSum: return PointProblem::BadWeightSum;
    case SceneView::Highlight::NoBone: return PointProblem::NoBone;
    default: return PointProblem::None;
    }
}

void App::DrawValidationPanel() { validationPanel_.Draw(WinName("Validation"), [this] { movesPanel_.DrawValidation(); }); }

void App::DrawCrewWindow() { crew_.Draw(WinName("Crew")); }

void App::ApplyCrewChange(bool resolve, bool rebuild) { crew_.Apply(resolve, rebuild); }


void App::DrawLogPanel() { logPanel_.Draw(WinName("Log")); }

void App::DrawSceneOverlay()
{
    weaponScene_.DrawOverlay(); // crosshair of the sight view
    overlay_.Draw();
}


void App::RegisterHotkeys()
{
    input_.Bind(ImGuiKey_F1, "Windows", "Help (this window), works everywhere", nullptr); // handled before the keyboard capture check
    input_.Bind(ImGuiMod_Ctrl | ImGuiKey_O, "File", "open a model / RTM", [this] { ShowOpenDialog(); });
    input_.Bind(ImGuiKey_F5, "File", "reload the model (keeps view, bone, sources)", [this] { LoadModel(session_.model.path, true); }, true);
    input_.Bind(ImGuiMod_Ctrl | ImGuiKey_Z, "Edit", "undo the last edit (config editors)", [this] { session_.commands.Undo(); });
    input_.Bind(ImGuiMod_Ctrl | ImGuiKey_Y, "Edit", "redo", [this] { session_.commands.Redo(); });
    input_.Bind(ImGuiMod_Ctrl | ImGuiKey_G, "Windows", "model.cfg graph", [this] { showGraph_ = !showGraph_; });
    input_.Bind(ImGuiMod_Ctrl | ImGuiKey_M, "Windows", "CfgMoves states graph", [this] { statesPanel_.Toggle(); });
    input_.Bind(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P, "Windows", "command palette", nullptr); // handled before the keyboard capture check
    input_.Bind(ImGuiMod_Ctrl | ImGuiKey_R, "Windows", "Vehicle & Crew mode and back (crew preview)", [this] { SetMode(shell_.mode == UiMode::Vehicle ? crewReturnMode_ : UiMode::Vehicle); });
    static const char* const kModeCmd[] = { "mode: Model", "mode: Skeleton & Anim", "mode: Vehicle & Crew", "mode: Materials", "mode: Validation",
                                            "mode: Reload sync (weapon)", "mode: Optics (weapon)" };
    for (int i = 0; i < int(UiMode::Count); ++i)
        input_.Bind(ImGuiMod_Ctrl | ImGuiKey(ImGuiKey_1 + i), "Windows", kModeCmd[i], [this, i] { SetMode(UiMode(i)); });
    input_.Bind(ImGuiKey_F, "Camera", "frame the model", [this] { camera_.Frame(scene_.boundsMin, scene_.boundsMax); }, true);
    input_.Bind(ImGuiKey_1, "Camera", "front", [this] { SetView(kPi, 0); });
    input_.Bind(ImGuiKey_2, "Camera", "back", [this] { SetView(0, 0); });
    input_.Bind(ImGuiKey_3, "Camera", "left", [this] { SetView(kViewLeftYaw, 0); });
    input_.Bind(ImGuiKey_4, "Camera", "right", [this] { SetView(kViewRightYaw, 0); });
    input_.Bind(ImGuiKey_5, "Camera", "top", [this] { SetView(kPi, 1.55f); });
    input_.Info("LMB drag", "Camera", "orbit (scene)");
    input_.Info("RMB / MMB drag", "Camera", "pan (scene)");
    input_.Info("Wheel", "Camera", "zoom (scene)");
    input_.Info("LMB click", "Points", "pick a point / hit sphere: name and coordinates");
    input_.Bind(ImGuiKey_G, "View", "floor grid", [this] { display_.showGrid = !display_.showGrid; });
    input_.Bind(ImGuiKey_N, "View", "normal lines", [this] { display_.showNormals = !display_.showNormals; scene_.UpdatePose(); });
    input_.Bind(ImGuiKey_B, "View", "skeleton", [this] { display_.showSkeleton = !display_.showSkeleton; });
    input_.Bind(ImGuiKey_W, "View", "wireframe overlay", [this] { display_.wire = display_.wire == WireMode::Overlay ? WireMode::Off : WireMode::Overlay; });
    input_.Bind(ImGuiKey_E, "Materials", "reflections A/B (env sky x fresnel)", [this] { display_.reflections = !display_.reflections; });
    input_.Bind(ImGuiKey_M, "Materials", "RVMAT A/B: original vs edited", [this] {
        if (mats_.edits.empty()) return;
        mats_.showOriginal = !mats_.showOriginal;
        scene_.BuildDrawSections();
    });
    input_.Bind(ImGuiKey_H, "Model", "hide the selected selection / proxy / picked faces", [this] { HideSelected(); }, true);
    input_.Bind(ImGuiMod_Shift | ImGuiKey_H, "Model", "isolate the selected selection / proxy / picked faces (again: back)", [this] { IsolateSelected(); }, true);
    input_.Bind(ImGuiMod_Alt | ImGuiKey_H, "Model", "show everything hidden in this LOD", [this] { ShowAllHidden(); }, true);
    input_.Info("Ctrl+LMB click", "Model", "pick / unpick a face (then H, Shift+H or save as a temporary set)");
    input_.Bind(ImGuiKey_PageDown, "Model", "next LOD", [this] { if (session_.lod + 1 < int(session_.model.lods.size())) { ++session_.lod; OnLodChanged(); } }, true);
    input_.Bind(ImGuiKey_PageUp, "Model", "previous LOD", [this] { if (session_.lod > 0) { --session_.lod; OnLodChanged(); } }, true);
    input_.Bind(ImGuiKey_Space, "Animation", "play / pause", [this] { playback_.playing = !playback_.playing; });
    input_.Bind(ImGuiKey_LeftArrow, "Animation", "previous frame", [this] { playback_.StepFrame(-1); });
    input_.Bind(ImGuiKey_RightArrow, "Animation", "next frame", [this] { playback_.StepFrame(1); });
    input_.Bind(ImGuiKey_Comma, "Animation", "previous frame", [this] { playback_.StepFrame(-1); });
    input_.Bind(ImGuiKey_Period, "Animation", "next frame", [this] { playback_.StepFrame(1); });
    input_.Info("LMB on timeline", "Animation", "scrub (Timeline panel)");
    // palette-only commands (no key)
    input_.Bind(ImGuiKey_None, "Windows", "about ArmaView (version, libraries)", [this] { aboutWindow_.open = true; });
    input_.Bind(ImGuiKey_None, "Windows", "reset layout of this mode", [this] { shell_.resetLayout[int(shell_.mode)] = true; });
    input_.Bind(ImGuiKey_None, "Windows", "reset layout of all modes", [this] { for (auto& r : shell_.resetLayout) r = true; });
    static const char* const kScaleCmd[] = { "UI scale: system DPI", "UI scale: 100%", "UI scale: 125%", "UI scale: 150%", "UI scale: 175%", "UI scale: 200%" };
    static const float kScales[] = { 0.0f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };
    for (int i = 0; i < 6; ++i)
        input_.Bind(ImGuiKey_None, "View", kScaleCmd[i], [this, i] { shell_.uiScale = kScales[i]; shell_.uiScaleDirty = true; ImGui::MarkIniSettingsDirty(); });
    input_.Bind(ImGuiKey_None, "File", "RTM folder (check RTM bones against the skeleton)", [this] { ShowFolderDialog(); });
    input_.Bind(ImGuiKey_None, "File", "hot reload on / off", [this] { assets_.watcher.enabled = !assets_.watcher.enabled; });
    input_.Bind(ImGuiKey_None, "View", "sun shadows on / off", [this] { display_.shadows = !display_.shadows; });
    input_.Bind(ImGuiKey_None, "View", "skeleton only (hide mesh, floor)", [this] { display_.skeletonOnly = !display_.skeletonOnly; });
    input_.Bind(ImGuiKey_None, "View", "show axes gizmo", [this] { display_.showAxes = !display_.showAxes; });
    input_.Bind(ImGuiKey_None, "View", "axis labels Arma (Y-up) / Blender (Z-up)", [this] { display_.axisLabelsBlender = !display_.axisLabelsBlender; });
    input_.Bind(ImGuiKey_None, "View", "show proxy triangles", [this] { display_.showProxies = !display_.showProxies; scene_.RebuildLodGeometry(); }, true);
    input_.Bind(ImGuiKey_None, "Light", "noon", [this] { light_.Apply(LightPreset::Noon); });
    input_.Bind(ImGuiKey_None, "Light", "sunset", [this] { light_.Apply(LightPreset::Sunset); });
    input_.Bind(ImGuiKey_None, "Light", "night", [this] { light_.Apply(LightPreset::Night); });
    input_.Bind(ImGuiKey_None, "Light", "overcast", [this] { light_.Apply(LightPreset::Overcast); });
    input_.Bind(ImGuiKey_None, "Light", "sun / headlight", [this] { light_.headlight = !light_.headlight; });
    input_.Bind(ImGuiKey_None, "Crew", "show crew on proxies", [this] { session_.attach.showCrew = !session_.attach.showCrew; scene_.RebuildLodGeometry(); }, true);
    input_.Bind(ImGuiKey_None, "Crew", "hide vehicle (only proxies and crew)", [this] { crew_.hideVehicle = !crew_.hideVehicle; scene_.BuildDrawSections(); }, true);
    input_.Bind(ImGuiKey_None, "Crew", "turned out (*Action instead of *InAction)", [this] { session_.attach.turnedOut = !session_.attach.turnedOut; ApplyCrewChange(true, true); }, true);
    input_.Bind(ImGuiKey_None, "Crew", "play all seats", [this] { session_.attach.playAll = !session_.attach.playAll; });
    input_.Bind(ImGuiKey_None, "Animation", "rest pose", [this] { playback_.restPose = !playback_.restPose; });
    input_.Bind(ImGuiKey_None, "Animation", "reset model.cfg sources", [this] {
        for (auto& s : session_.cfgSources) { s.value = std::clamp(0.0f, s.minValue, s.maxValue); s.animate = false; }
        poseDirty_ = true;
    });
    input_.Info("Double click node", "Graph", "focus: only its links (depth slider 1-3)");
    input_.Info("Esc", "Graph", "leave focus");
    input_.Info("Minimap click / drag", "Graph", "jump to the nearest node");
    input_.Bind(ImGuiKey_None, "Windows", "Getting started (scenarios step by step)", [this] { guidePanel_.open = true; });
    input_.Bind(ImGuiKey_None, "File", "Mod check (validate every file of a mod folder)", [this] { modCheck_.open = true; });
    input_.Bind(ImGuiKey_None, "File", "Record video / GIF of the view", [this] { record_.open = true; });
    input_.Bind(ImGuiKey_None, "View", "language: Russian", [this] { SetUiLang(UiLang::Ru); ImGui::MarkIniSettingsDirty(); });
    input_.Bind(ImGuiKey_None, "View", "language: English", [this] { SetUiLang(UiLang::En); ImGui::MarkIniSettingsDirty(); });
    input_.PublishKeys();
}

void App::DrawHelpWindow() { helpWindow_.Draw(dpi_); }



// ============================================================================================ layers preview (view side)
// The bake is core/LayersJob; the preview takes over what plays: moves drive and retarget preview off, the base as B.




// ============================================================================================ CfgMoves (view side)
// Loading, game mode, control mode and gestures are core (MovesLoader, MovesSim, MovesControl, GesturePlayer); these
// add what the view does around them (pose, watch list, States graph, grid, footsteps).













// ============================================================================================ retargeting (view side)

void App::DrawRetargetPanel() { retargetPanel_.Draw(WinName("Retarget"), WinName("Timeline")); }

// ============================================================================================ Inspector (view side)

void App::DrawInspector() { inspector_.Draw(WinName("Inspector"), shell_.mode == UiMode::Materials); }

void App::ClearHighlight()
{
    scene_.selection = scene_.selectedBone = -1;
    scene_.highlight = SceneView::Highlight::None;
    inspector_.what = InspectorPanel::Kind::None;
    scene_.RebuildLodGeometry();
    scene_.RebuildSkeletonLines();
}

bool App::PickedPoint(std::string& info) const
{
    if (!points_.HasPick()) return false;
    info = points_.PickInfo(session_.model, session_.lod);
    return true;
}


void App::HighlightSelection(int sel)
{
    const P3DLod& l = session_.model.lods[size_t(session_.lod)];
    if (sel < 0 || sel >= int(l.selections.size()))
    {
        scene_.selection = scene_.selectedBone = -1;
        scene_.highlight = SceneView::Highlight::None;
        inspector_.what = InspectorPanel::Kind::None;
    }
    else
    {
        scene_.selection = sel;
        scene_.selectedBone = session_.skeleton.Find(l.selections[size_t(sel)].name);
        scene_.highlight = SceneView::Highlight::Selection;
        inspector_.what = InspectorPanel::Kind::Selection;
    }
    scene_.RebuildLodGeometry();
    scene_.RebuildSkeletonLines();
}

// ============================================================================================ hot reload (view side)

bool App::TextureChanged(const fs::path& file)
{
    texCache_.Invalidate(file);
    scene_.RebuildLodGeometry();
    const bool ok = !texCache_.AnyError(file);
    if (!ok) texCache_.Invalidate(file); // drop the broken decode so the retry reads it again
    return ok;
}

// ui-script raw keyboard: the keys are platform/Input; inputstate is what the view did with them

// ============================================================================================ modes, outliner, small windows

void App::SetMode(UiMode m)
{
    if (m == shell_.mode) return;
    if (m == UiMode::Vehicle) crewReturnMode_ = shell_.mode;
    shell_.mode = m;
    showCrewWindow_ = m == UiMode::Vehicle; // stub labels, crew-only overlays
    // the Inspector follows what the mode is about
    if (m == UiMode::Materials) inspector_.what = InspectorPanel::Kind::Section;
    else if (m == UiMode::Vehicle && crew_.selected >= 0) inspector_.what = InspectorPanel::Kind::Seat;
    else if (inspector_.what == InspectorPanel::Kind::Section || inspector_.what == InspectorPanel::Kind::Seat) inspector_.what = scene_.selectedBone >= 0 ? InspectorPanel::Kind::Bone : InspectorPanel::Kind::None;
}

void App::DrawOutliner()
{
    if (ui::Begin(WinName("Outliner").c_str()))
    {
        if (shell_.mode == UiMode::Animation) DrawSkeletonPanel();
        else outliner_.Draw();
    }
    ImGui::End();
}

void App::DrawGettingStarted()
{
    if (!pendingFocus_.empty())
    {
        // a docked panel of the new mode: bring its tab to the front once the mode has been drawn
        if (ImGuiWindow* w = ImGui::FindWindowByName(WinName(pendingFocus_.c_str()).c_str()); w && w->WasActive)
        {
            ImGui::FocusWindow(w);
            pendingFocus_.clear();
        }
    }
    guidePanel_.Draw(dpi_);
}

void App::DrawCommandPalette() { palette_.Draw(dpi_); }

void App::DrawAboutWindow() { aboutWindow_.Draw(dpi_); }

// ============================================================================================ settings export / import

void App::ExportSettings()
{
    const fs::path f = SaveFileDialog(hwnd_, L"ArmaView settings (*.txt)\0*.txt\0All files\0*.*\0", L"txt", paths_.Data() / "ArmaView settings.txt");
    if (!f.empty()) layouts_.Export(f);
}

void App::ImportSettings()
{
    const fs::path f = OpenFileDialog(hwnd_, L"ArmaView settings (*.txt)\0*.txt\0All files\0*.*\0", paths_.Data());
    if (!f.empty()) layouts_.Import(f);
}

std::string App::LoadProgress(float& fraction) const
{
    if (modelJob_.valid())
    {
        fraction = -1.0f;
        return std::format("loading {}", PathToUtf8(modelJobPath_.filename()));
    }
    if (texCache_.Streaming())
    {
        fraction = texCache_.StreamTotal() ? float(texCache_.StreamDone()) / float(texCache_.StreamTotal()) : 0.0f;
        return std::format("textures {} / {}", texCache_.StreamDone(), texCache_.StreamTotal());
    }
    return {};
}
