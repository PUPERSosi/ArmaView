#include "LayersJob.h"
#include "Log.h"
#include "Retarget.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>

#include "CoreGuard.h"

namespace fs = std::filesystem;

std::shared_ptr<RtmAnimation> LayersJob::LoadModelSpace(const std::string& path)
{
    if (path == "#retarget") return rt_.res.anim;
    fs::path p = fs::path(Utf8ToWide(path));
    if (!p.has_extension()) p += ".rtm";
    auto a = std::make_shared<RtmAnimation>();
    std::string err;
    std::vector<std::string> warn;
    if (!LoadRtm(p, *a, err, warn)) { msg = PathToUtf8(p) + ": " + err; return nullptr; }
    return a;
}

bool LayersJob::Bake()
{
    if (s_.skeleton.Empty()) { msg = "open a model with a skeleton"; return false; }
    proj.source = src;
    msg.clear();
    base = LoadModelSpace(proj.source);
    if (!base) { if (msg.empty()) msg = "no base clip (retarget first, or give an RTM)"; return false; }
    std::string report;
    baked = BakeAnimProject(proj, *base, s_.skeleton, s_.masks, [this](const std::string& p) { return LoadModelSpace(p); }, report);
    msg = report;
    if (baked) Log::Get().Info("Layers: {}", report);
    return baked != nullptr;
}

float LayersJob::BakedDuration() const
{
    return proj.duration * (proj.ops.trimEnd - proj.ops.trimStart) / std::max(proj.ops.speed, 1e-3f);
}

void LayersJob::StartPreview()
{
    if (!baked) return;
    if (!previewOn) { saved_ = std::move(s_.anim.layers); previewOn = true; }
    s_.anim.layers.clear();
    AnimLayer l;
    l.anim = baked;
    l.tag = 4;
    l.duration = BakedDuration();
    s_.anim.layers.push_back(l);
    s_.anim.Bind(s_.skeleton);
}

void LayersJob::StopPreview()
{
    if (!previewOn) return;
    s_.anim.layers = std::move(saved_);
    saved_.clear();
    s_.anim.Bind(s_.skeleton);
    previewOn = false;
}

bool LayersJob::Write(const fs::path& file, bool overwrite)
{
    if (!baked && !Bake()) return false;
    std::error_code ec;
    if (fs::exists(file, ec))
    {
        if (!overwrite) { overwriteAsk = true; msg = PathToUtf8(file) + " exists - not written (confirm the overwrite; a .bak is kept)"; return false; }
        fs::path bak = file;
        bak += ".bak";
        for (int k = 2; fs::exists(bak, ec) && k < 1000; ++k) { bak = file; bak += ".bak" + std::to_string(k); } // never overwrite a backup
        fs::copy_file(file, bak, fs::copy_options::none, ec);
        if (ec) { msg = "cannot write the backup " + PathToUtf8(bak); return false; }
        fs::remove(file, ec);
    }
    std::string err;
    if (!WriteRtm0101(*baked, file, err)) { msg = err; return false; }
    RtmAnimation back;
    std::vector<std::string> warn;
    if (!LoadRtm(file, back, err, warn)) { msg = "written, re-read failed: " + err; return false; }
    float dp = 0, de = 0;
    std::string note;
    CompareAnimations(back, *baked, dp, de, note);
    msg = std::format("baked {} ({} frames); re-read: {:.2g} m / {:.2g} - {}", PathToUtf8(file), back.Frames(), dp, de,
                      dp < 1e-4f && de < 1e-4f && note.empty() ? "same as the preview" : "DIFFERS " + note);
    Log::Get().Info("Layers: {}", msg);
    return true;
}

bool LayersJob::BatchStart()
{
    const fs::path dir = fs::path(Utf8ToWide(batchDir));
    std::error_code ec;
    batch = {};
    batchFiles_.clear();
    if (!fs::is_directory(dir, ec)) { msg = "batch: no such folder"; return false; }
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (IEquals(PathToUtf8(e.path().extension()), ".rtm")) batchFiles_.push_back(e.path());
    std::sort(batchFiles_.begin(), batchFiles_.end());
    batch.total = int(batchFiles_.size());
    batch.running = true;
    msg = std::format("batch: {} file(s)", batch.total);
    return true;
}

bool LayersJob::BatchStep()
{
    if (!batch.running) return false;
    const fs::path outDir = fs::path(Utf8ToWide(batchOut));
    if (batch.index >= batch.total)
    {
        batch.running = false;
        batch.current.clear();
        msg = std::format("batch: {} written to {}, {} skipped (exist), {} failed", batch.written, PathToUtf8(outDir), batch.skipped, batch.failed);
        Log::Get().Info("Layers: {}", msg);
        return false;
    }
    const fs::path& in = batchFiles_[size_t(batch.index++)];
    batch.current = PathToUtf8(in.filename());
    const fs::path o = outDir / in.filename();
    std::error_code ec;
    if (fs::exists(o, ec)) { ++batch.skipped; return true; } // never overwrite in a batch
    std::shared_ptr<RtmAnimation> b;
    if (proj.source == "#retarget")
    {
        // the retarget preset (rig + mapping + options) of the Retarget panel first
        auto a = std::make_shared<RtmAnimation>();
        std::string err;
        std::vector<std::string> warn;
        if (!rt_.src.ok || !LoadRtm(in, *a, err, warn)) { ++batch.failed; return true; }
        auto ms = a;
        if (!ms) { ++batch.failed; return true; }
        b = RetargetAnimation(*ms, rt_.src.skel, s_.skeleton, rt_.map, rt_.opt).anim;
    }
    else
        b = LoadModelSpace(PathToUtf8(in));
    if (!b) { ++batch.failed; return true; }
    std::string report, err;
    auto bk = BakeAnimProject(proj, *b, s_.skeleton, s_.masks, [this](const std::string& p) { return LoadModelSpace(p); }, report);
    if (!bk || !WriteRtm0101(*bk, o, err)) { ++batch.failed; return true; }
    ++batch.written;
    return true;
}

void LayersJob::BatchCancel()
{
    if (!batch.running) return;
    batch.total = batch.index; // the next step reports what was done
    BatchStep();
}

void LayersJob::Batch()
{
    if (!BatchStart()) return;
    while (BatchStep()) {}
}

fs::path LayersJob::ProjectFile() const
{
    const fs::path base_ = src[0] && std::string(src) != "#retarget" ? fs::path(Utf8ToWide(src)) : s_.model.path;
    return base_.parent_path() / (PathToUtf8(base_.stem()) + ".avanim.json");
}

bool LayersJob::SaveProject()
{
    const fs::path f = ProjectFile();
    std::string err;
    proj.source = src;
    if (!SaveAnimProject(f, proj, err)) { msg = err; return false; }
    msg = "project saved: " + PathToUtf8(f);
    MarkProjectSaved();
    return true;
}

bool LayersJob::LoadProject()
{
    const fs::path f = ProjectFile();
    std::string err;
    if (!LoadAnimProject(f, proj, err)) { msg = err; return false; }
    strncpy_s(src, proj.source.c_str(), _TRUNCATE);
    msg = "loaded " + PathToUtf8(f);
    MarkProjectSaved();
    return true;
}

bool LayersJob::OpenInBlender(bool background, const BlenderRunner& run)
{
    if (!baked && !Bake()) return false;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"armaview";
    fs::create_directories(dir, ec);
    const fs::path rtm = dir / L"armaview_preview.rtm", py = dir / L"armaview_import.py";
    fs::remove(rtm, ec); // our own temp file
    std::string err;
    if (!WriteRtm0101(*baked, rtm, err)) { msg = err; return false; }
    auto pyStr = [](const fs::path& p) { std::string s = PathToUtf8(p); std::replace(s.begin(), s.end(), '\\', '/'); return s; };
    {
        std::ofstream f(py, std::ios::trunc);
        f << "import bpy\n"
          << "name = " << "'" << blendArmature << "'\n"
          << "arm = bpy.data.objects.get(name) if name else None\n"
          << "if arm is None:\n    arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)\n"
          << "if arm is None:\n    print('ARMAVIEW no armature in the file')\nelse:\n"
          << "    if arm.library or arm.override_library: arm = arm.make_local()\n"
          << "    ad = arm.animation_data\n"
          << "    if ad and getattr(ad, 'use_tweak_mode', False): ad.use_tweak_mode = False\n"
          << "    if ad and ad.is_property_readonly('action'): arm.animation_data_clear(); arm.animation_data_create()\n"
          << "    for o in bpy.context.view_layer.objects: o.select_set(False)\n"
          << "    bpy.context.view_layer.objects.active = arm\n    arm.select_set(True)\n"
          << "    try:\n"
          << "        res = bpy.ops.a3ob.import_rtm(filepath='" << pyStr(rtm) << "')\n"
          << "        act = arm.animation_data.action if arm.animation_data else None\n"
          << "        print('ARMAVIEW import', res, arm.name, act.name if act else None, tuple(act.frame_range) if act else None)\n"
          << "    except Exception as e:\n"
          << "        print('ARMAVIEW import failed:', e)\n";
    }
    const std::wstring exe = Utf8ToWide(blenderExe), blend = Utf8ToWide(blendFile);
    if (!fs::exists(exe, ec)) { msg = "Blender not found: " + std::string(blenderExe); return false; }
    const std::wstring args = L"\"" + blend + L"\" " + (background ? L"-b " : L"") + L"--python \"" + py.native() + L"\"";
    std::string runOut;
    if (!run(exe, args, background, runOut)) { msg = "cannot start Blender"; return false; }
    if (!background)
    {
        msg = "Blender started: " + std::string(blendFile) + " (a3ob import into the armature; back: Export RTM & Preview)";
        return true;
    }
    std::istringstream ls(runOut);
    for (std::string line; std::getline(ls, line);)
        if (line.find("ARMAVIEW") != std::string::npos) msg = "Blender: " + line;
    return msg.find("FINISHED") != std::string::npos;
}
