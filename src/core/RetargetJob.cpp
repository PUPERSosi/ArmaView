#include "RetargetJob.h"
#include "Log.h"

#include <format>

#include "CoreGuard.h"

namespace fs = std::filesystem;

fs::path RetargetJob::PresetFile(const Skeleton& target) const
{
    return presetDir_ / std::format("{}__{}.txt", src.skel.name, target.name);
}

void RetargetJob::LoadSource(const Session& s)
{
    msg.clear();
    if (!LoadRetargetRig(fs::path(Utf8ToWide(srcModel)), s.projectRoot.empty() ? fs::path("P:\\") : s.projectRoot, src))
    {
        msg = "Source rig: " + src.info;
        return;
    }
    map = AutoMapBones(src.skel, s.skeleton);
    if (LoadMappingPreset(PresetFile(s.skeleton), src.skel, s.skeleton, map)) msg = "mapping preset loaded: " + PathToUtf8(PresetFile(s.skeleton));
    Log::Get().Info("Retarget source: {} ({})", srcModel, src.info);
}

bool RetargetJob::Prepare(const Session& s)
{
    if (s.skeleton.Empty()) { msg = "Load the target model (with a model.cfg skeleton) first"; return false; }
    if (!src.ok) LoadSource(s);
    if (!src.ok) return false;
    if (map.srcOf.size() != s.skeleton.bones.size()) map = AutoMapBones(src.skel, s.skeleton);
    fs::path file = fs::path(Utf8ToWide(anim));
    if (!file.has_extension()) file += ".rtm";
    auto a = std::make_shared<RtmAnimation>();
    std::string err;
    std::vector<std::string> warn;
    if (!LoadRtm(file, *a, err, warn)) { msg = "Source animation: " + err; return false; }
    std::shared_ptr<RtmAnimation> ms = a;
    res = RetargetAnimation(*ms, src.skel, s.skeleton, map, opt);
    if (!res.anim) { msg = res.report; return false; }
    res.anim->path = file;
    msg = std::format("{} -> {}: {}", PathToUtf8(file.filename()), s.skeleton.name, res.report);
    Log::Get().Info("Retarget: {}", msg);
    Log::Get().Debug("Retarget frame 0:{}", res.debug);
    // default output: next to the target model, never an existing file
    if (!out[0] || outAuto)
    {
        fs::path dir = s.model.path.parent_path() / "retarget";
        fs::path next = dir / std::format("{}_{}.rtm", PathToUtf8(file.stem()), s.skeleton.name);
        std::error_code ec;
        for (int k = 2; fs::exists(next, ec); ++k) next = dir / std::format("{}_{}_{}.rtm", PathToUtf8(file.stem()), s.skeleton.name, k);
        strncpy_s(out, PathToUtf8(next).c_str(), _TRUNCATE);
        outAuto = true;
    }
    return true;
}

void RetargetJob::StartPreview(Session& s)
{
    if (!res.anim) return;
    if (!previewOn) { saved_ = std::move(s.anim.layers); previewOn = true; }
    s.anim.layers.clear();
    AnimLayer l;
    l.anim = res.anim;
    l.tag = 2;
    l.duration = duration;
    s.anim.layers.push_back(l);
    s.anim.Bind(s.skeleton);
}

void RetargetJob::StopPreview(Session& s)
{
    if (!previewOn) return;
    s.anim.layers = std::move(saved_);
    saved_.clear();
    s.anim.Bind(s.skeleton);
    previewOn = false;
}

bool RetargetJob::Export(bool overwrite)
{
    if (!res.anim) { msg = "Retarget first"; return false; }
    const fs::path target = fs::path(Utf8ToWide(out));
    std::string err;
    std::string backup;
    if (std::error_code ec; overwrite && fs::exists(target, ec))
    {
        fs::path bak = target;
        bak += ".bak";
        for (int k = 2; fs::exists(bak, ec) && k < 1000; ++k) { bak = target; bak += ".bak" + std::to_string(k); }
        fs::rename(target, bak, ec);
        if (ec) { msg = "Export: cannot move the old file to " + PathToUtf8(bak) + ": " + ec.message(); return false; }
        backup = " (old file: " + PathToUtf8(bak.filename()) + ")";
    }
    if (!WriteRtm0101(*res.anim, target, err)) { msg = "Export: " + err; Log::Get().Error("Retarget export: {}", err); return false; }
    // check: read the written file back and compare with the preview
    RtmAnimation back;
    std::vector<std::string> warn;
    if (!LoadRtm(target, back, err, warn)) { msg = "Export written, but re-reading failed: " + err; return false; }
    float maxPos = 0, maxElem = 0;
    std::string note;
    CompareAnimations(back, *res.anim, maxPos, maxElem, note);
    const bool ok = note.empty() && maxPos < 1e-4f && maxElem < 1e-4f;
    msg = std::format("Exported {}{} ({} frames, {} bones). Re-read: max position difference {:.2g} m, matrix {:.2g} - {}{}", PathToUtf8(target), backup,
                         back.Frames(), back.info.bones.size(), maxPos, maxElem, ok ? "same as the preview" : "DIFFERS", note.empty() ? "" : " (" + note + ")");
    (ok ? Log::Get().Info("Retarget: {}", msg) : Log::Get().Error("Retarget: {}", msg));
    outAuto = true; // next run proposes a new free name
    return ok;
}
