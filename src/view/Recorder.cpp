#include "Recorder.h"
#include "Lang.h"
#include "Log.h"
#include "Text.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace fs = std::filesystem;

namespace {
constexpr float kPi = 3.14159265f;
} // namespace

float Recorder::RecordAnimLength(std::string* what) const
{
    auto say = [&](const std::string& s) { if (what) *what = s; };
    if (!s_.hasModel) { say(T("no model")); return 0; }
    if (mode_() == AppMode::Reload && reload_.wchar_.model) { say(TF("the reload ({})", s_.weapon.reloadAction)); return reload_.ReloadDuration() / std::max(0.01f, reload_.reloadK_); }
    if (movesSim_.drive && movesSim_.player.Current() >= 0)
    {
        const MoveState& st = s_.moves.states[size_t(movesSim_.player.Current())];
        say(TF("one cycle of the CfgMoves state {}", st.name));
        return st.duration;
    }
    float len = 0;
    for (const auto& l : s_.anim.layers)
        if (l.enabled && l.anim && l.anim->Frames() > 1) len = std::max(len, l.duration / std::max(0.01f, l.speed > 0 ? l.speed : 1.0f));
    if (len > 0) { say(T("one cycle of the RTM layers")); return len; }
    say(T("no animation playing (record a turn instead)"));
    return 0;
}

bool Recorder::StartRecording()
{
    if (run_.active) return false;
    if (!s_.hasModel) { msg_ = T("Open a model first."); return false; }
    Run& r = run_;
    r = Run{};
    // H.264 wants even sizes
    rec_.width = std::clamp(rec_.width & ~1, 16, 7680);
    rec_.height = std::clamp(rec_.height & ~1, 16, 4320);
    rec_.fps = std::clamp(rec_.fps, 1, 120);
    float seconds = 0;
    if (rec_.what == 0)
    {
        seconds = RecordAnimLength(&r.source);
        if (seconds <= 0) { msg_ = TF("Nothing to record: {}", r.source); return false; }
    }
    else
    {
        seconds = std::max(0.5f, rec_.turnSeconds);
        r.source = T("a 360 degree turn");
    }
    r.frames = std::max(1, int(std::lround(seconds * float(rec_.fps))));
    r.reload = mode_() == AppMode::Reload;
    r.animStart = r.reload ? 0.0 : playback_.time;
    r.yaw0 = camera_.yaw;
    // output: <folder>\<model>_<what>_<time>.<ext>
    fs::path dir = rec_.outDir[0] ? fs::path(Utf8ToWide(rec_.outDir)) : fs::path();
    if (dir.empty())
    {
        wchar_t buf[MAX_PATH] = {};
        ExpandEnvironmentStringsW(L"%USERPROFILE%\\Videos\\ArmaView", buf, MAX_PATH);
        dir = buf;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::current_zone()->to_local(std::chrono::system_clock::now()));
    const std::string base = std::format("{}_{}_{:%Y%m%d_%H%M%S}", PathToUtf8(s_.model.path.stem()), rec_.what ? "turn" : "anim", now);
    static const char* const kExt[] = { ".mp4", ".gif", "" };
    r.out = dir / fs::path(Utf8ToWide(base + kExt[rec_.format]));
    if (rec_.format == 2) fs::create_directories(r.out, ec); // PNG frames: a folder
    else
    {
        r.pipe = std::make_unique<FfmpegPipe>();
        std::string err;
        if (!r.pipe->Start(Utf8ToWide(rec_.ffmpeg), FfmpegArgs(rec_.width, rec_.height, rec_.fps, r.out), fs::path(r.out).replace_extension(".ffmpeg.log"), err))
        {
            msg_ = err;
            r = Run{};
            return false;
        }
    }
    r.active = true;
    playback_.playing = false;
    reload_.playing = false;
    msg_ = TF("Recording {}: {} frames {}x{} at {} fps", r.source, r.frames, rec_.width, rec_.height, rec_.fps);
    Log::Get().Info("Record: {} -> {}", msg_, PathToUtf8(r.out));
    return true;
}

void Recorder::FrameSetup()
{
    Run& r = run_;
    if (!r.active) return;
    const double t = double(r.frame) / double(rec_.fps);
    if (rec_.what == 0)
    {
        if (r.reload) reload_.time = t;
        else playback_.time = r.animStart + t;
        playback_.playing = false;
        reload_.playing = false;
    }
    else
        camera_.yaw = r.yaw0 + 2.0f * kPi * float(r.frame) / float(r.frames);
    host_.PoseChanged();
}

void Recorder::FrameCapture(const SceneDraw& main, const float clear[4])
{
    Run& r = run_;
    SceneDraw s = main;
    s.viewportX = 0;
    s.viewportY = 0;
    s.viewportW = float(rec_.width);
    s.viewportH = float(rec_.height);
    s.viewProj = camera_.Projection(float(rec_.width) / float(rec_.height)) * camera_.View();
    std::vector<uint8_t> rgba;
    bool ok = renderer_.BeginOffscreen(UINT(rec_.width), UINT(rec_.height), clear);
    if (ok)
    {
        renderer_.DrawScene(s);
        ok = renderer_.ReadOffscreen(rgba);
    }
    renderer_.EndOffscreen();
    if (ok)
    {
        if (r.pipe) ok = r.pipe->Write(rgba.data(), rgba.size());
        else
        {
            std::string err;
            ok = WritePngRgba(r.out / std::format("frame_{:05}.png", r.frame), rec_.width, rec_.height, rgba, err);
            if (!ok) msg_ = err;
        }
    }
    if (!ok) { StopRecording(false); return; }
    if (++r.frame >= r.frames) StopRecording(true);
}

void Recorder::StopRecording(bool completed)
{
    Run& r = run_;
    if (!r.active) return;
    r.active = false;
    std::string tail;
    int code = 0;
    bool ok = completed;
    if (r.pipe) ok = r.pipe->Finish(code, tail) && completed;
    last_ = ok ? r.out : fs::path();
    if (ok)
    {
        msg_ = TF("Saved {} ({} frames)", PathToUtf8(r.out), r.frames);
        Log::Get().Info("Record: {}", msg_);
        std::error_code ec;
        fs::remove(fs::path(r.out).replace_extension(".ffmpeg.log"), ec);
    }
    else
    {
        msg_ = TF("Recording failed after {} of {} frames (ffmpeg exit {}): {}", r.frame, r.frames, code, tail);
        Log::Get().Error("Record: {}", msg_);
    }
    r.pipe.reset();
    if (rec_.what == 1) camera_.yaw = r.yaw0;
}
