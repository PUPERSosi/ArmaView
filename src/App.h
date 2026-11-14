#pragma once
// The application: creates the services (core document and jobs, platform, view, panels), wires them together through
// their interfaces and runs the frame (input, update, UI, render). Everything else lives in core/, platform/, render/,
// view/, panels/ and automation/.
#include "AnimationView.h"
#include "AssetSession.h"
#include "ClosePrompt.h"
#include "CommandPalette.h"
#include "CrewPanel.h"
#include "UnitPanel.h"
#include "StartScreen.h"
#include "DisplayPanel.h"
#include "GettingStarted.h"
#include "InfoWindows.h"
#include "Input.h"
#include "InspectorPanel.h"
#include "LayersPanel.h"
#include "LogPanel.h"
#include "LogSinks.h"
#include "MaterialsPanel.h"
#include "ModCheckPanel.h"
#include "ModelOutliner.h"
#include "MoveEditorPanel.h"
#include "MovesPanel.h"
#include "NodeGraph.h"
#include "OpticsPanel.h"
#include "Recorder.h"
#include "ReloadPanel.h"
#include "RetargetPanel.h"
#include "SceneInput.h"
#include "SceneOverlay.h"
#include "SceneView.h"
#include "ShaderSurveyPanel.h"
#include "SkeletonPanel.h"
#include "SourcesPanel.h"
#include "StatesPanel.h"
#include "TimelinePanel.h"
#include "LayoutStore.h"
#include "Settings.h"
#include "UiSettings.h"
#include "UiShell.h"
#include "ValidationPanel.h"
#include "ViewerAutomation.h"
#include "WeaponScene.h"
#include "WinFileSystem.h"
#include "WinPaths.h"
#include "WorldLight.h"

#include <chrono>
#include <functional>
#include <optional>
#include <future>
#include <thread>

class App : private IViewHost, private IValidationView, private IMaterialsView, private ITimelineView, private IWeaponSceneHost, private IAssetView,
            private ISceneHost, private IInspectorView, private IShellHost, private IOutlinerView, private IUnitHost,
            private IStartHost
{
public:
    friend class ViewerAutomation; // test infrastructure (automation/): drives the whole viewer
    bool Init(HWND hwnd, float dpiScale);
    void Shutdown();
    void Frame();
    void OnResize(UINT width, UINT height) { pendingW_ = width; pendingH_ = height; }
    void OpenFile(const std::filesystem::path& path) { assets_.OpenFile(path); } // .p3d model, .rtm layer
    void ApplyAutomation(const Automation& a) { automation_.Apply(a); }
    bool ScriptActive() const { return automation_.Active(); }
    std::filesystem::path LogFile() const { return paths_.Log(); } // copied into a crash report
    bool AllowClose() { return closePrompt_.AllowClose(automation_.Active()); } // unsaved edits: asks first
    static void OpenInEditorAt(const std::filesystem::path& file, int line);   // VS Code / Notepad++ at the line
    std::function<void()> busyTick; // set by main: a long command on the UI thread is alive (hang watchdog)
    struct BusyScope                // a known long operation on the UI thread: tells the watchdog every second
    {
        explicit BusyScope(const std::function<void()>& tick)
            : t_(tick ? std::jthread([tick](std::stop_token st) {
                  while (!st.stop_requested())
                  {
                      for (int i = 0; i < 10 && !st.stop_requested(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                      tick();
                  }
              })
                      : std::jthread()) {}
        std::jthread t_;
    };

private:
    // ---- frame and model
    bool LoadModel(const std::filesystem::path& path, bool keepView, bool requireComplete = false) override; // hot reload (here, needs the result)
    void OpenModel(const std::filesystem::path& path, bool keepView) override;  // in the background (open, F5)
    bool PollModelLoad();
    void ApplyModel(P3DModel&& m, const std::filesystem::path& path, bool keepView);
    bool ModelLoading() const { return modelJob_.valid(); }
    void OnLodChanged();
    void RebuildAttachments() override;
    void LoadConfigSkins();
    void RebuildGraph();
    void PollGameConfig();
    void RegisterHotkeys();
    void DrawUI();
    void DrawSceneOverlay();
    void SetMode(UiMode m) override;
    void SetView(float yaw, float pitch) override;
    void SelectBone(int bone, bool focus = false) { input_.SelectBone(bone, focus, shell_.mode == UiMode::Materials); }
    void ShowOpenDialog() override;
    void ShowFolderDialog() override;
    std::wstring PickFolder(const wchar_t* title);
    std::string PickTexture(const std::string& current) override;
    std::string WinName(const char* title) const { return shell_.WinName(title); }
    glm::mat4 ViewProj() const override;
    const ArmaConfig* CrewConfig() const { return session_.CrewConfig(); }
    bool PointScreenPos(const std::string& name, float& x, float& y) const { return points_.PointScreenPos(name, x, y); }
    void OpenAnimInEditor(const CfgAnimation& a) const;
    const RvMaterial* GetRvMaterial(const std::string& name);
    void ApplyCrewChange(bool resolve, bool rebuild);
    void HighlightSelection(int selection) override;

    // ---- windows (each a panel; these only name them for the mode)
    void DrawOutliner();
    void DrawInspector();
    void DrawSkeletonPanel();
    void DrawAnimationPanel();
    void DrawSourcesPanel();
    void DrawMaterialPanel();
    void DrawValidationPanel();
    void DrawCrewWindow();
    // IUnitHost (Unit window, ui-script `unit`)
    void AssembleUnit(const std::string& cls) override;
    void ReplaceUnitPart(UnitSlot slot, const std::string& cls) override;
    void UnitPartsChanged() override;
    const ArmaConfig* UnitConfig() const override { return CrewConfig(); }
    void OutfitBody(const std::filesystem::path& p3d) override;
    std::filesystem::path SamplesArmaMan() const override;
    void AddUnitFile(UnitSlot slot, const std::filesystem::path& p3d) override;
    void RemoveUnitPart(size_t index) override;
    std::filesystem::path PickP3d() override;
    std::vector<std::string> OutfitPresets() const override;
    bool SaveOutfitPreset(const std::string& name) override;
    bool LoadOutfitPreset(const std::string& name) override;
    bool DeleteOutfitPreset(const std::string& name) override;
    bool ControlOn() const override { return control_.on; }
    void SetControl(bool on) override;
    std::filesystem::path OutfitFolder() const;
    void DrawUnitWindow();
    // IStartHost (start screen, setup and paths)
    void StartTask(int mode, const char* panel) override { ShowMode(mode, panel); }
    void OpenGuide() override { guidePanel_.open = true; }
    Setup SetupState() const override;
    void PickSamples() override;
    void PickBlender() override;
    void ApplyUnit(); // after the model / LOD / attachments: the unit's parts loaded, skinned, published (or put away)
    void DrawLogPanel();
    void DrawRetargetPanel();
    void DrawRecordWindow();
    void DrawModCheckWindow();
    void DrawHelpWindow();
    void DrawAboutWindow();
    void DrawGettingStarted();
    void DrawCommandPalette();
    void DrawMenu() { shell_.DrawMenu({ &modCheck_.open, &record_.open, &guidePanel_.open, &helpWindow_.open, &aboutWindow_.open, &showGraph_, &start_.open, &start_.setupOpen }); }
    void DrawModeBar() { shell_.DrawModeBar(); }
    void DrawStatusBar() { shell_.DrawStatusBar(); }
    ImGuiID DrawDockHost() { return shell_.DrawDockHost(); }
    void ApplyUiScale() { dpi_ = shell_.ApplyUiScale(); }
    void DrawMovesPanel() { movesPanel_.Draw(WinName("Moves"), WinName("Timeline")); }
    void DrawMovesEditor() { moveEditorPanel_.Draw(WinName("Move editor"), WinName("Timeline")); }
    void DrawStatesWindow() { statesPanel_.Draw(); }
    void DrawLayersPanel() { layersPanel_.Draw(WinName("Layers"), WinName("Timeline")); }
    void DrawReloadPanel() { reloadPanel_.Draw(WinName("Reload sync")); }
    void DrawOpticsPanel() { opticsPanel_.Draw(WinName("Optics")); }

    // ---- interface glue (what the panels / view / core ask of the viewer)
    // IViewHost
    float Dpi() const override { return dpi_; }
    void RebuildGeometry() override { scene_.RebuildLodGeometry(); }
    void RebuildDrawSections() override { scene_.BuildDrawSections(); }
    void PoseChanged() override { poseDirty_ = true; }
    void WatchedFilesChanged() override { assets_.watchDirty = true; }
    void InspectSeat() override { inspector_.what = InspectorPanel::Kind::Seat; }
    void OpenInEditor(const std::filesystem::path& file, int line) const override { OpenInEditorAt(file, line); }
    void PickBone(int bone, bool focusCamera) override { SelectBone(bone, focusCamera); }
    int SelectedBone() const override { return scene_.selectedBone; }
    void LodChanged() override { OnLodChanged(); }
    const std::vector<int>& BoneSelection() const override { return scene_.boneSelection; }
    void ShowMode(int mode, const char* panel) override { SetMode(UiMode(mode)); if (panel && panel[0]) pendingFocus_ = panel; }
    void SceneRect(float& x, float& y, float& w, float& h) const override { x = sceneX_; y = sceneY_; w = sceneW_; h = sceneH_; }
    // IValidationView
    const ValidationReport& LodReport() const override { return scene_.validation; }
    PointProblem HighlightedProblem() const override;
    void HighlightProblem(PointProblem p) override;
    const std::vector<SeamJump>& LoopSeam() const override { return playback_.seam; }
    const std::filesystem::path& RtmFolder() const override { return assets_.rtmFolder; }
    const std::vector<RtmBoneCheck>& RtmChecks() const override { return assets_.rtmChecks; }
    void PickRtmFolder() override { ShowFolderDialog(); }
    void RecheckRtm() override { assets_.RunRtmCheck(); }
    BoneUsage UsageOfBone(int bone) const override { return scene_.UsageOfBone(bone); }
    int PrimaryLayer() const override;
    AnimStatus AnimStatusOf(const CfgAnimation& a) const { return scene_.AnimStatusOf(a); }
    // IMaterialsView
    const std::vector<MeshSectionInfo>& Sections() const override { return scene_.meshSections; }
    size_t CachedTextures() const override { return texCache_.Count(); }
    size_t DamageMaterials() const override { return scene_.damageMat.size(); }
    size_t DamageTextures() const override { return scene_.damageTex.size(); }
    int SelectedSection() const override { return inspector_.section; }
    void SelectSection(int section) override;
    // IInspectorView
    int HighlightedSelection() const override { return scene_.selection; }
    void ClearHighlight() override;
    bool PickedPoint(std::string& info) const override;
    glm::vec3 BoundsSize() const override { return scene_.boundsMax - scene_.boundsMin; }
    const std::vector<std::string>& ProxyNames() const override { return scene_.proxyNames; }
    const FaceDiagInfo& FaceInfo() override { return scene_.FaceInfo(); }
    ShadingMode Shading() const override { return display_.shading; }
    void SetShading(ShadingMode m) override { display_.shading = m; }
    void MaterialsEdited() override { scene_.BuildDrawSections(); }
    // IAssetView
    bool TextureChanged(const std::filesystem::path& file) override;
    void GeometryChanged() override { scene_.RebuildLodGeometry(); }
    void AttachmentsChanged() override { RebuildAttachments(); }
    std::vector<std::filesystem::path> TextureFiles() const override { return texCache_.ResolvedFiles(); }
    void LogLayerCoverage(const AnimLayer& layer) const override { scene_.LogLayerCoverage(layer); }
    void LogMissingBones(const std::string& rtmName, const std::vector<int>& bones, bool indent) const override { scene_.LogMissingBones(rtmName, bones, indent); }
    // ISceneHost
    int DamageState() const override { return materials_.damageState; }
    bool HideVehicle() const override { return crew_.hideVehicle; }
    void ExtraGhosts(std::vector<MeshVertex>& gv, std::vector<Renderer::Ghost>& ghosts) override { weaponScene_.Ghosts(gv, ghosts); }
    void SectionsRebuilt() override { if (inspector_.section >= int(scene_.meshSections.size())) inspector_.section = -1; }
    void ApplyRootMotion(std::vector<glm::mat4>& bones) override { scene_.ApplyRootMotion(control_, bones); }
    // IOutlinerView
    ModelVisibility& Visibility() override { return vis_; }
    std::vector<uint32_t>& PickedFaces() override { return scene_.pickFaces; }
    const std::vector<uint8_t>& FaceHidden() const override { return scene_.faceHidden; }
    void VisibilityChanged() override { scene_.VisibilityChanged(); }
    void HideSelected() override { scene_.HideSelected(); }
    void IsolateSelected() override { scene_.IsolateSelected(); }
    void ShowAllHidden() override { scene_.ShowAllHidden(); }
    void RebuildLodOverlay() override { scene_.RebuildLodOverlay(); }
    // IShellHost
    void ReloadModel() override { if (session_.hasModel) OpenModel(session_.model.path, true); }
    void FrameModel() override { camera_.Frame(scene_.boundsMin, scene_.boundsMax); }
    void RequestClose() override { PostMessageW(hwnd_, WM_CLOSE, 0, 0); }
    double FrameCpuMs() const override { return frameCpuMs_; }
    void ExportSettings() override;
    void ImportSettings() override;
    std::string LoadProgress(float& fraction) const override;
    bool GameConfigsLoading() const override { return gameLoad_.valid(); }
    // IWeaponSceneHost
    UiMode Mode() const override { return shell_.mode; }
    Renderer& SceneRenderer() override { return renderer_; }
    OrbitCamera& SceneCamera() override { return camera_; }
    const std::vector<MeshVertex>& BaseVerts() const override { return scene_.baseVerts; }
    const std::vector<uint32_t>& VertPoint() const override { return scene_.vertPoint; }
    size_t AttachFirst() const override { return scene_.attachFirst; }
    const SkinData& Skin() const override { return scene_.skin; }
    std::string ValidationSummary(const std::string& label) override { return automation_.ValidationSummary(label); }

    // ---- platform, window, renderer
    HWND hwnd_ = nullptr;
    float dpi_ = 1.0f;
    UINT pendingW_ = 0, pendingH_ = 0;
    WinPaths paths_;              // the exe folder, the viewer's own files (IPaths)
    Settings settings_{ paths_.Data() / "settings.txt" }; // ui values, layouts, per-model visibility: one file
    WinFileSystem files_;         // the disk as the core sees it
    KeyboardInput keyboard_;      // ui-script raw keyboard
    Renderer renderer_;
    OrbitCamera camera_;
    TextureCache texCache_;
    LogWindowSink logWindow_;     // the Log window's entries and ArmaView.log (registered in Init)
    std::unique_ptr<FileLogSink> logFile_;
    ScopedLogSink logWindowReg_, logFileReg_;

    // ---- the document and its services (core)
    EventBus events_;
    Session session_{ files_, events_ };
    MaterialSet mats_{ session_.projectRoot };
    Playback playback_{ session_ };
    MovesSim movesSim_{ session_ };
    MovesLoader movesLoader_{ session_, movesSim_ };
    GesturePlayer gesturePlayer_{ session_, movesSim_ };
    MovesEditor movesEditor_{ session_.movesDoc, session_.moves };
    MovesControl control_;
    RetargetJob rt_{ {} };        // preset folder set in Init
    LayersJob layers_{ session_, rt_ };
    AssetSession assets_{ session_, playback_, mats_, movesSim_, files_, *this }; // open files, hot reload, RTM check
    ShaderSurvey survey_;
    struct LoadedModel
    {
        std::unique_ptr<P3DModel> model;
        ModelCache proxies; // its proxy models, loaded on the worker too
    };
    std::future<LoadedModel> modelJob_; // the model being read on a worker
    std::filesystem::path modelJobPath_;
    bool modelJobKeep_ = false;
    std::chrono::steady_clock::time_point modelJobStart_;
    std::future<std::shared_ptr<ArmaConfig>> gameLoad_; // game configs from P:, loaded in the background
    std::string pendingState_, pendingWorld_;           // --state / --world before the game configs are loaded

    // ---- view (render data, input, view-side commands)
    DisplaySettings display_;
    AnimViewOptions animView_;
    Lighting light_;
    LightingResult lit_;
    WorldLighting world_;
    std::vector<std::string> worlds_;
    ModelVisibility vis_;         // hide / isolate per model (settings.txt)
    PointOverlay points_;
    SceneView scene_{ session_, playback_, mats_, texCache_, renderer_, camera_, display_, animView_, gesturePlayer_, vis_, *this, *this };
    ReloadSync reload_{ session_, scene_.skin };
    OpticsFit optics_{ session_, reload_ };
    WeaponScene weaponScene_{ session_, reload_, optics_, animView_, *this, *this };
    std::vector<Hotkey> hotkeys_; // bound in RegisterHotkeys (each one a registry command), listed by Help
    CommandRegistry scriptCommands_;
    AboutWindow aboutWindow_;
    HelpWindow helpWindow_{ hotkeys_ };
    CommandPalette palette_{ hotkeys_, [this] { return session_.hasModel; } };
    SceneOverlay overlay_{ session_, scene_, display_, light_, lit_, camera_, points_, renderer_, *this, *this, showCrewWindow_ };
    CfgGraph graph_;              // model.cfg graph window
    bool showGraph_ = false, graphDirty_ = true;

    // ---- panels
    LogPanel logPanel_{ logWindow_ };
    CrewPanel crew_{ session_, *this };
    UnitPanel unit_{ session_, *this };
    StartScreen start_{ *this };
    const ArmaConfig* unitCfg_ = nullptr; // config the unit was read from
    std::optional<Unit::Preset> pendingOutfit_; // a preset whose body is loading: its parts once the model is open
    SourcesPanel sources_{ session_, *this };
    SkeletonPanel skeletonPanel_{ session_, *this };
    GettingStarted guidePanel_{ *this };
    ValidationPanel validationPanel_{ session_, *this, *this };
    MaterialsPanel materials_{ session_, *this, *this };
    ShaderSurveyPanel surveyPanel_{ session_, survey_, *this };
    InspectorPanel inspector_{ session_, mats_, texCache_, crew_, animView_, *this, *this };
    SceneInput input_{ session_, scene_, camera_, points_, inspector_, palette_, helpWindow_.open, control_, scriptCommands_, hotkeys_, *this };
    TimelinePanel timeline_{ session_, playback_, animView_, *this, *this };
    StatesPanel statesPanel_{ session_, movesLoader_, movesSim_, movesEditor_, *this, animation_ };
    MovesPanel movesPanel_{ session_, movesLoader_, movesSim_, control_, gesturePlayer_, movesEditor_, playback_, *this, animation_ };
    MoveEditorPanel moveEditorPanel_{ session_, movesEditor_, movesLoader_, movesSim_, playback_, statesPanel_, *this, animation_ };
    LayersPanel layersPanel_{ session_, layers_, animView_, *this, animation_ };
    AnimationView animation_{ session_, playback_, movesSim_, movesLoader_, gesturePlayer_, control_, layers_, rt_, scene_, renderer_, camera_, statesPanel_, paths_,
                              *this, [this] { return gameLoad_.valid(); } };
    RetargetPanel retargetPanel_{ session_, rt_, *this, [this] { return animation_.RetargetRun(); }, [this] { animation_.RetargetStopPreview(); } };
    ReloadPanel reloadPanel_{ session_, reload_, animView_, *this, weaponScene_ };
    OpticsPanel opticsPanel_{ session_, optics_, *this, weaponScene_ };
    ModelOutliner outliner_{ session_, inspector_, shell_.hasIcons, *this, *this };
    ModCheckPanel modCheck_{ session_, [this](const wchar_t* title) { return PickFolder(title); },
                             [](const std::filesystem::path& f, int line) { OpenInEditorAt(f, line); } };
    RecordPanel record_{ *this, recorder_ };
    Recorder recorder_{ session_, playback_, reload_, movesSim_, renderer_, camera_, record_.settings, [this] { return shell_.mode; }, *this,
                        [this](const wchar_t* title) { return PickFolder(title); } };
    DisplayPanel displayPanel_{ session_, display_, light_, lit_, world_, worlds_, camera_, points_, *this };
    UiShell shell_{ session_, playback_, display_, displayPanel_, palette_, assets_.watcher, layouts_, *this, *this };
    UiSettings uiSettings_{ shell_, record_.settings, movesSim_, points_ };
    LayoutStore layouts_{ settings_, uiSettings_ };
    ClosePrompt closePrompt_{ session_, mats_, reload_, weaponScene_, layers_, movesEditor_, [this] { PostMessageW(hwnd_, WM_CLOSE, 0, 0); } };
    ViewerAutomation automation_{ *this };

    // ---- frame state
    float sceneX_ = 0, sceneY_ = 0, sceneW_ = 1, sceneH_ = 1; // scene rectangle (central dock node)
    bool poseDirty_ = false, wasAnimating_ = false, pendingCoverageLog_ = false;
    bool showCrewWindow_ = false, firstLayout_ = false, firstRunChecked_ = false, firstFrameLogged_ = false;
    UiMode crewReturnMode_ = UiMode::Model; // Ctrl+R toggles Vehicle & Crew and back
    std::string pendingFocus_, focusTab_;   // a panel to bring to front once its mode is drawn
    double clock_ = 0.0, sourceClock_ = 0.0, frameCpuMs_ = 0.0, lastLoadMs_ = 0.0;
    size_t logMark_ = 0;                    // log entries before the last LoadModel (selftest: problems of that model)
    int profileFrames_ = 0;
    bool profileLog_ = GetEnvironmentVariableA("ARMAVIEW_PROFILE", nullptr, 0) > 0; // per-part frame times in the log
    std::chrono::steady_clock::time_point startTime_ = std::chrono::steady_clock::now();
};
