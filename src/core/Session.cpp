#include "Session.h"
#include "Log.h"
#include "Gltf.h"
#include "Perf.h"

#include <optional>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <format>
#include <map>

#include "CoreGuard.h"

namespace fs = std::filesystem;

Session::Session(const IFileSystem& files, EventBus& events) : files_(files), events_(events)
{
    // the config editor's snapshots are the state; the stack orders them with the other edits
    movesDoc.onEdit = [this](const std::string& what) { commands.Record(std::make_unique<ConfigDocEdit>(movesDoc, what)); };
    movesDoc.onLoad = [this] { commands.Clear(); };
}

bool Session::ReadModel(const fs::path& path, bool requireComplete, P3DModel& out) { return ReadModel(DefaultFileSystem(), path, requireComplete, out); }

bool Session::ReadModel(const IFileSystem& files, const fs::path& path, bool requireComplete, P3DModel& out)
{
    auto& log = Log::Get();
    if (IsGltfFile(path))
    {
        PerfNoteScope ps("load: glTF read + import");
        std::string err;
        if (!LoadGltf(path, out, err))
        {
            log.Error("Model not loaded: '{}': {}", PathToUtf8(path), err);
            return false;
        }
        return true;
    }
    bool parsed;
    // a hot reload that will retry (requireComplete): a file still being written fails to read / parse on every attempt
    // but the last - its messages are Debug then (the caller warns once per retry); the last attempt reports Errors
    std::vector<LogEntry> held;
    std::optional<Log::CaptureScope> hold;
    if (requireComplete) hold.emplace(held);
    {
        std::vector<uint8_t> bytes;
        bool read;
        {
            PerfNoteScope ps("load:   P3D file read");
            read = files.Read(path, bytes);
        }
        PerfNoteScope ps("load: P3D read + parse");
        if (!read)
        {
            out = {};
            out.path = path;
            log.Error("Cannot read '{}'", PathToUtf8(path));
            parsed = false;
        }
        else
            parsed = LoadP3D(path, bytes, out);
    }
    if (!parsed) log.Error("Model not loaded: '{}'", PathToUtf8(path));
    if (hold)
    {
        hold.reset();
        for (auto& e : held) log.Add(!parsed && e.level == LogLevel::Error ? LogLevel::Debug : e.level, std::move(e.text));
    }
    if (!parsed) return false;
    if (requireComplete && !out.complete)
    {
        log.Warn("'{}' is incomplete (still being written?) - keeping the previous model", PathToUtf8(path));
        return false;
    }
    return true;
}

void Session::SetModel(P3DModel&& m, bool keepLod)
{
    const int prevLod = lod;
    model = std::move(m);
    hasModel = true;
    lod = keepLod ? std::clamp(prevLod, 0, std::max(0, int(model.lods.size()) - 1)) : 0;
    projectRoot = ArmaProjectRoot(model.path);
    LogModel();
    {
        PerfNoteScope ps("load: model.cfg + skeleton");
        LoadModelConfig();
    }
    anim.Bind(skeleton);
    events_.Publish(SessionEvent::ModelLoaded);
}

void Session::LogModel() const
{
    auto& log = Log::Get();
    if (model.format.empty()) log.Info("MLOD v{:#x}, {} LOD(s)", model.version, model.lods.size());
    else log.Info("{}, {} LOD(s)", model.format, model.lods.size());
    for (size_t i = 0; i < model.lods.size(); ++i)
    {
        const P3DLod& l = model.lods[i];
        std::map<std::string, int> textures;
        for (const auto& f : l.faces)
            if (!f.texture.empty()) ++textures[f.texture];
        log.Info("  LOD {} [{}]: points {}, normals {}, faces {} (tris {}), selections {}, properties {}, sharp edges {}, UV sets {}, textures {}{}{}",
                 i, l.Name(), l.points.size(), l.normals.size(), l.faces.size(), l.TriangleCount(), l.selections.size(),
                 l.properties.size(), l.sharpEdges.size(), l.uvSets.size(), textures.size(),
                 l.mass.empty() ? "" : std::format(", mass {:.2f}", l.TotalMass()),
                 l.animationFrames ? std::format(", #Animation# frames {}", l.animationFrames) : "");
        for (const auto& p : l.properties) log.Debug("    property {} = {}", p.name, p.value);
        for (const auto& s : l.selections)
            log.Debug("    selection '{}': points {} (partial weight {}), faces {}", s.name, s.pointCount, s.partialCount, s.faceCount);
    }
}

fs::path Session::FindModelCfg(const fs::path& p3d) const
{
    std::error_code ec;
    for (fs::path dir = p3d.parent_path(); !dir.empty(); dir = dir.parent_path())
    {
        fs::path cfgPath = dir / "model.cfg";
        if (files_.Stamp(cfgPath).exists) return cfgPath;
        if (dir == dir.root_path()) break;
        if (!projectRoot.empty() && fs::equivalent(dir, projectRoot, ec)) break;
    }
    return {};
}

void Session::LoadModelConfig()
{
    auto& log = Log::Get();
    skeleton = {};
    modelInfo = {};
    pivots = {};
    cfgAnims.clear();
    cfgSources.clear();
    hasPivots = false;
    modelCfg.clear();
    cfg = ArmaConfig{}; // no model.cfg of a previous model stays (watch list, outliner)

    if (!model.importedBones.empty())
    {
        // an imported model (glTF): its skeleton travels with it; the joints of its Memory LOD are the pivots
        skeleton.name = model.importedSkeleton;
        for (const auto& [b, p] : model.importedBones) skeleton.bones.push_back({ b, p });
        for (size_t i = 0; i < skeleton.bones.size(); ++i)
        {
            Bone& bn = skeleton.bones[i];
            bn.parent = bn.parentName.empty() ? -1 : skeleton.Find(bn.parentName);
            if (bn.parent >= 0) skeleton.bones[size_t(bn.parent)].children.push_back(int(i));
        }
        modelInfo.modelClass = PathToUtf8(model.path.stem()) + " (" + model.format + ")";
        modelInfo.skeletonName = skeleton.name;
        pivots = {};
        pivots.path = model.path;
        for (const auto& l : model.lods)
            if (l.resolution > 1.0e14f) pivots.lods.push_back(l);
        hasPivots = !pivots.lods.empty();
        skeleton.pivotsPath = model.path;
        log.Info("{}: skeleton {} ({} bones), {} clip(s)", model.format, skeleton.name, skeleton.bones.size(), model.importedClips.size());
        return;
    }
    if (!model.format.empty()) return; // imported without joints: no rig
    modelCfg = FindModelCfg(model.path);
    if (modelCfg.empty())
    {
        log.Warn("No model.cfg in '{}' or its parents up to {}", PathToUtf8(model.path.parent_path()), PathToUtf8(projectRoot));
        return;
    }
    cfg.Load(modelCfg, projectRoot);
    log.Info("model.cfg: '{}' ({} file(s), {} error(s))", PathToUtf8(modelCfg), cfg.Files().size(), cfg.Errors());

    if (!BuildSkeleton(cfg, PathToUtf8(model.path.stem()), skeleton, modelInfo)) return;
    log.Info("Skeleton {} (CfgModels::{}): {} bones, isDiscrete={}, skeletonInherit=\"{}\", sections {}",
             skeleton.name, modelInfo.modelClass, skeleton.bones.size(), skeleton.isDiscrete ? 1 : 0, skeleton.inherit, modelInfo.sections.size());

    if (!skeleton.pivotsModel.empty())
    {
        fs::path p = ResolveArmaPath(skeleton.pivotsModel, projectRoot);
        if (!p.has_extension()) p += ".p3d";
        skeleton.pivotsPath = p;
        char sig[4] = {};
        const bool onDisk = files_.Stamp(p).exists;
        if (onDisk) std::ifstream(p, std::ios::binary).read(sig, 4);
        if (onDisk && std::string_view(sig, 4) != "MLOD")
            log.Warn("pivotsModel '{}': {} - using weighted selection centroids", PathToUtf8(p), kBinarizedNotSupported);
        else if (onDisk && LoadP3D(p, pivots))
        {
            hasPivots = true;
            log.Info("pivotsModel: '{}' loaded", PathToUtf8(p));
        }
        else
            log.Warn("pivotsModel '{}' not found or unreadable - using weighted selection centroids", PathToUtf8(p));
    }
    else
        log.Info("Skeleton {} has no pivotsModel - using weighted selection centroids", skeleton.name);

    BuildCfgAnimations(cfg, modelInfo, skeleton, cfgAnims, cfgSources);
}

float Session::RestFloor() const
{
    float y = 1e30f;
    if (!model.lods.empty())
        for (const auto& p : model.lods[0].points) y = std::min(y, p.pos.y);
    return y < 1e29f ? y : 0.0f;
}

bool Session::LoadAddonConfig()
{
    auto& log = Log::Get();
    const std::string prevClass = activeSkin >= 0 && activeSkin < int(configSkins.size()) ? configSkins[activeSkin].className : "";
    configSkins.clear();
    activeSkin = -1;
    configCpp.clear();
    configCppFiles.clear();
    std::error_code ec;
    for (fs::path dir = model.path.parent_path(); !dir.empty(); dir = dir.parent_path())
    {
        if (fs::exists(dir / "config.cpp", ec)) { configCpp = dir / "config.cpp"; break; }
        if (dir == dir.root_path() || (!projectRoot.empty() && fs::equivalent(dir, projectRoot, ec))) break;
    }
    if (configCpp.empty())
    {
        log.Info("No config.cpp above the model - no hiddenSelectionsTextures");
        return false;
    }
    modCfg = std::make_shared<ArmaConfig>();
    ArmaConfig& addon = *modCfg;
    addon.SetFallback(game); // the game's configs (P:\a3) when loaded: inheritance continues into them
    addon.Load(configCpp, projectRoot, false);
    configCppFiles = addon.Files();
    // The mod's folder under its prefix: a mod in P:\MyMods\MyMod names its files "\MyMod\anm\x.rtm" ($PBOPREFIX$, the
    // prefix set in the settings, the one its config's own paths agree on, else the folder name). When that path is not on
    // P:\ itself, the folder is mounted under the prefix (its paths resolve there, the model is named by it).
    MountInferredModPrefix(configCpp.parent_path(), &addon, projectRoot);
    std::string rel = ArmaPathOf(model.path, projectRoot);
    configSkins = FindConfigSkins(addon, rel);
    log.Info("config.cpp: '{}' ({} file(s), {} error(s), {}); {} class(es) re-texture this model", PathToUtf8(configCpp),
             addon.Files().size(), addon.Errors(),
             game ? std::format("over the game configs, {} base(s) not found", addon.MissingBases())
                   : std::format("{} game base classes not on disk", addon.MissingBases()),
             configSkins.size());
    for (int i = 0; i < int(configSkins.size()); ++i)
    {
        const ConfigSkin& s = configSkins[i];
        log.Info("  skin {}: {}::{} ({}): {} hidden selection(s)", i, s.cfgRoot, s.className, s.where, s.selections.size());
        if (s.className == prevClass) activeSkin = i;
        for (const auto& sel : s.selections)
        {
            bool inSections = false;
            for (const auto& sec : modelInfo.sections) inSections |= IEquals(sec, sel);
            if (!inSections)
                log.Warn("  {}::{}: hidden selection \"{}\" is not in CfgModels::{} sections[] - Arma will not re-texture it",
                         s.cfgRoot, s.className, sel, modelInfo.modelClass.empty() ? "?" : modelInfo.modelClass);
        }
        if (s.textures.size() > s.selections.size())
            log.Warn("  {}::{}: {} textures for {} hidden selections", s.cfgRoot, s.className, s.textures.size(), s.selections.size());
    }
    if (activeSkin < 0 && !configSkins.empty()) activeSkin = 0;
    return true;
}

std::shared_ptr<ArmaConfig> LoadGameConfigs(const fs::path& root, const fs::path& cacheFile)
{
    auto t0 = std::chrono::steady_clock::now();
    auto cfg = std::make_shared<ArmaConfig>();
    // warm start: binary snapshot keyed by the sources' paths, sizes and times
    if (cfg->LoadCache(cacheFile))
    {
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Perf::Get().Note("game configs (background thread)", s * 1000.0);
        Log::Get().Info("Game configs: {} file(s) from the cache in {:.2f} s", cfg->Files().size(), s);
        return cfg;
    }
    auto files = GameConfigFiles(root);
    if (files.empty()) return nullptr;
    cfg->LoadMerged(files, root, { "cfgpatches", "cfgvehicles", "cfgmoves*", "cfggestures*", "cfgnonaivehicles", "cfgworlds", "cfgweapons",
                                   "cfgmagazines", "cfgfaces", "cfgheads", "cfgglasses", "slotinfo", "cowsslot*", "muzzleslot*", "pointerslot*", "underbarrelslot*" });
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Perf::Get().Note("game configs (background thread)", s * 1000.0);
    Log::Get().Info("Game configs: {} file(s) from {} in {:.1f} s ({} parse error(s), {} base(s) not found)", files.size(), PathToUtf8(root), s,
                    cfg->Errors(), cfg->MissingBases());
    if (cfg->SaveCache(cacheFile)) Log::Get().Info("Game configs cached: {}", PathToUtf8(cacheFile));
    return cfg;
}
