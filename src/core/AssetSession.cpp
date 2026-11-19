#include "AssetSession.h"
#include "Log.h"
#include "RvMaterial.h"
#include "Text.h"
#include "Validation.h"

#include <algorithm>
#include <chrono>
#include <format>

#include "CoreGuard.h"

namespace fs = std::filesystem;

namespace {
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
} // namespace

void AssetSession::OpenFile(const fs::path& path)
{
    const std::string ext = ToLower(PathToUtf8(path.extension()));
    std::error_code ec;
    if (ext == ".p3d" || ext == ".gltf" || ext == ".glb")
    {
        // the same model again (e.g. Blender "Export & Preview"): reload keeping camera, bone, animation
        const bool same = s_.hasModel && fs::equivalent(path, s_.model.path, ec);
        view_.OpenModel(path, same);
        watcher.Restamp(path); // already reloaded: not again when the watcher notices the change
    }
    else if (ext == ".rtm")
    {
        if (rtmFolder != path.parent_path())
        {
            rtmFolder = path.parent_path();
            RunRtmCheck();
        }
        bool existing = false;
        for (const auto& l : s_.anim.layers) existing |= l.anim && fs::equivalent(l.anim->path, path, ec);
        if (existing) // update the layer instead of adding a duplicate
        {
            if (ReloadRtm(path)) Log::Get().Info("RTM '{}' updated in its existing layer(s)", PathToUtf8(path.filename()));
        }
        else
            pb_.AddLayer(path);
        watcher.Restamp(path);
    }
    else
        Log::Get().Warn("Unsupported file type: '{}'", PathToUtf8(path));
}

bool AssetSession::ReloadRtm(const fs::path& file)
{
    auto a = std::make_shared<RtmAnimation>();
    std::string err;
    std::vector<std::string> warnings;
    if (!LoadRtm(file, *a, err, warnings))
    {
        Log::Get().Warn("Hot reload: RTM '{}' not readable yet: {}", PathToUtf8(file.filename()), err);
        return false;
    }
    std::error_code ec;
    sim_.Replace(file, a); // a CfgMoves state playing this RTM gets the new version too
    for (auto& l : s_.anim.layers)
        if (l.anim && fs::equivalent(l.anim->path, file, ec))
        {
            l.anim = a;
            view_.LogLayerCoverage(l);
        }
    s_.anim.Bind(s_.skeleton);
    // only this file again (not the whole folder)
    std::error_code ec2;
    if (!rtmFolder.empty() && !s_.skeleton.Empty() && fs::equivalent(file.parent_path(), rtmFolder, ec2))
    {
        RtmBoneCheck c = CheckRtmFile(file, s_.skeleton);
        ++rtmFilesChecked;
        auto it = std::find_if(rtmChecks.begin(), rtmChecks.end(), [&](const RtmBoneCheck& x) { return fs::equivalent(x.file, file, ec2); });
        Log::Get().Info("RTM re-check '{}' vs skeleton {}", PathToUtf8(file.filename()), s_.skeleton.name);
        LogRtmCheck(c);
        if (it != rtmChecks.end()) *it = std::move(c);
        else rtmChecks.push_back(std::move(c));
    }
    return true;
}

void AssetSession::RunRtmCheck()
{
    CancelRtmCheck();
    rtmChecks.clear();
    if (rtmFolder.empty()) return;
    if (s_.skeleton.Empty())
    {
        Log::Get().Warn("RTM check: no skeleton loaded - open a model with a model.cfg skeleton first");
        return;
    }
    // on a worker with its own copy of the skeleton; the results are applied in PollRtmCheck (UI thread)
    rtmJob_.done = std::make_shared<std::atomic<int>>(0);
    rtmJob_.total = std::make_shared<std::atomic<int>>(0);
    rtmJob_.cancel = std::make_shared<std::atomic<bool>>(false);
    rtmJob_.folder = rtmFolder;
    rtmJob_.skeleton = s_.skeleton.name;
    rtmJob_.bones = s_.skeleton.bones.size();
    rtmJob_.result = std::async(std::launch::async, [folder = rtmFolder, sk = s_.skeleton, done = rtmJob_.done, total = rtmJob_.total, cancel = rtmJob_.cancel] {
        return CheckRtmFolder(folder, sk, done.get(), total.get(), cancel.get());
    });
}

void AssetSession::RtmCheckProgress(int& done, int& total) const
{
    done = total = 0;
    if (!rtmJob_.result.valid()) return;
    done = *rtmJob_.done;
    total = *rtmJob_.total;
}

void AssetSession::CancelRtmCheck()
{
    if (!rtmJob_.result.valid()) return;
    *rtmJob_.cancel = true;
    rtmJob_.result.wait();
    PollRtmCheck();
}

void AssetSession::WaitRtmCheck()
{
    if (!rtmJob_.result.valid()) return;
    rtmJob_.result.wait();
    PollRtmCheck();
}

bool AssetSession::PollRtmCheck()
{
    if (!rtmJob_.result.valid() || rtmJob_.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    rtmChecks = rtmJob_.result.get();
    rtmFilesChecked += int(rtmChecks.size());
    const bool cancelled = *rtmJob_.cancel;
    int clean = 0;
    for (const auto& c : rtmChecks) clean += c.error.empty() && c.missing.empty() && c.extra.empty();
    Log::Get().Info("RTM check '{}' vs skeleton {} ({} bones): {} file(s), {} match exactly{}", PathToUtf8(rtmJob_.folder), rtmJob_.skeleton, rtmJob_.bones,
                    rtmChecks.size(), clean, cancelled ? std::format(" - cancelled after {} of {}", int(*rtmJob_.done), int(*rtmJob_.total)) : "");
    for (const auto& c : rtmChecks) LogRtmCheck(c);
    return true;
}

void AssetSession::LogRtmCheck(const RtmBoneCheck& c) const
{
    auto& log = Log::Get();
    const Skeleton& sk = s_.skeleton;
    const std::string name = PathToUtf8(c.file.filename());
    if (!c.error.empty())
    {
        if (c.info.binarized) log.Warn("  {}: {}", name, c.error);
        else log.Error("  {}: {}", name, c.error);
        return;
    }
    if (c.missing.empty() && c.extra.empty())
    {
        log.Debug("  {}: {} frames, {} bones - matches the skeleton", name, c.info.frames, c.info.bones.size());
        return;
    }
    log.Info("  {}: {} frames, {} bones; skeleton bones missing in RTM: {}; RTM bones not in skeleton: {}", name, c.info.frames, c.info.bones.size(),
             c.missing.size(), c.extra.size());
    if (!c.extra.empty()) log.Warn("  {}: RTM bones not in skeleton (ignored): {}", name, JoinList(c.extra, 20));
    if (!c.missing.empty())
    {
        std::vector<int> bones;
        for (const auto& m : c.missing)
            if (int b = sk.Find(m); b >= 0) bones.push_back(b);
        view_.LogMissingBones(name, bones, true);
    }
}

void AssetSession::UpdateWatchList()
{
    using K = FileWatcher::Kind;
    std::vector<std::pair<fs::path, K>> files;
    if (s_.hasModel)
    {
        files.emplace_back(s_.model.path, K::Model);
        for (const auto& f : s_.cfg.Files()) files.emplace_back(f, K::ModelCfg);
        if (s_.hasPivots) files.emplace_back(s_.skeleton.pivotsPath, K::Pivots);
        for (const auto& f : s_.configCppFiles) files.emplace_back(f, K::ConfigCpp);
        for (const auto& [k, m] : mats_.loaded)
            if (m.loaded) files.emplace_back(m.path, K::Rvmat);
        for (const auto& f : view_.TextureFiles()) files.emplace_back(f, K::Texture);
        for (const auto& f : s_.attach.WatchFiles()) files.emplace_back(f, K::Attachment);
    }
    for (const auto& l : s_.anim.layers)
        if (l.anim && l.anim->info.signature != "glTF") files.emplace_back(l.anim->path, K::Rtm); // glTF clips: with the model
    watcher.Set(files);
    watchDirty = false;
}

void AssetSession::Poll(double clock)
{
    PollRtmCheck();
    if (watchDirty) UpdateWatchList();
    auto events = watcher.Poll(clock);
    if (events.empty()) return;
    auto& log = Log::Get();
    using K = FileWatcher::Kind;
    auto finish = [&](const FileWatcher::Event& e, bool ok, double t0) {
        const std::string name = PathToUtf8(e.path.filename());
        if (ok)
            log.Info("Hot reload: {} - done in {:.0f} ms{}", name, (Now() - t0) * 1000.0, e.attempt ? std::format(" (attempt {})", e.attempt + 1) : "");
        else if (watcher.Retry(e, clock))
            log.Warn("Hot reload: {} not ready (half-written or locked), retry {}/{}", name, e.attempt + 1, watcher.maxRetries);
        else
            log.Error("Hot reload: {} still broken after {} retries - keeping the previous state", name, watcher.maxRetries);
    };
    // a model / model.cfg / config.cpp / pivots change reloads the whole model once (the view is kept)
    const FileWatcher::Event* modelEvent = nullptr;
    for (const auto& e : events)
        if (e.kind == K::Model || e.kind == K::ModelCfg || e.kind == K::ConfigCpp || e.kind == K::Pivots)
        {
            modelEvent = &e;
            break;
        }
    if (modelEvent && s_.hasModel)
    {
        const double t0 = Now();
        log.Info("Hot reload: {} changed - reloading the model", PathToUtf8(modelEvent->path.filename()));
        const bool ok = view_.LoadModel(s_.model.path, true, modelEvent->attempt < watcher.maxRetries);
        finish(*modelEvent, ok, t0);
        for (const auto& e : events) // other files are re-read by the model reload anyway
            if (e.kind == K::Rtm) finish(e, ReloadRtm(e.path), t0);
        return;
    }
    for (const auto& e : events)
    {
        const double t0 = Now();
        bool ok = true;
        switch (e.kind)
        {
        case K::Rtm: ok = ReloadRtm(e.path); break;
        case K::Rvmat:
        {
            RvMaterial test;
            ok = LoadRvMaterial(e.path, s_.projectRoot, test) && test.errors == 0;
            if (ok)
            {
                mats_.DropFile(e.path);
                view_.GeometryChanged();
            }
            break;
        }
        case K::Attachment: // proxy model, crew model / its model.cfg, crew RTM
            s_.attach.Invalidate(e.path);
            view_.AttachmentsChanged();
            view_.GeometryChanged();
            break;
        case K::Texture: ok = view_.TextureChanged(e.path); break;
        default: break;
        }
        finish(e, ok, t0);
    }
    watchDirty = true;
}
