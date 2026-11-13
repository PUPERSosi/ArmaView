#pragma once
// Recording the view: the animation once (RTM layers, the CfgMoves state, the Reload timeline) or a 360 degree turn,
// rendered offscreen at any size without the UI, frame by frame (the clock is set per frame, not real time), piped
// into ffmpeg (MP4 / GIF) or written as PNGs. The Record window (panels/RecordPanel) shows it and starts / stops it.
#include "Camera.h"
#include "FfmpegPipe.h"
#include "MovesSim.h"
#include "Playback.h"
#include "RecordPanel.h"
#include "ReloadSync.h"
#include "Renderer.h"
#include "Session.h"
#include "UiShell.h"
#include "ViewHost.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

class Recorder : public IRecordView
{
public:
    using FolderPicker = std::function<std::wstring(const wchar_t* title)>;
    Recorder(Session& session, Playback& playback, ReloadSync& reload, MovesSim& sim, Renderer& renderer, OrbitCamera& camera, RecordSettings& settings,
             const UiShell& shell, IViewHost& host, FolderPicker pickFolder)
        : s_(session), playback_(playback), reload_(reload), movesSim_(sim), renderer_(renderer), camera_(camera), rec_(settings), shell_(shell),
          host_(host), pickFolder_(std::move(pickFolder)) {}

    void FrameSetup();                                         // before the pose of a recorded frame: its time / turn
    void FrameCapture(const SceneDraw& main, const float clear[4]); // after: render offscreen, hand to ffmpeg / PNG

    // IRecordView
    float RecordAnimLength(std::string* what) const override;
    bool StartRecording() override;
    void StopRecording(bool completed) override;
    bool Recording() const override { return run_.active; }
    int RecordFrame() const override { return run_.frame; }
    int RecordFrames() const override { return run_.frames; }
    const std::string& RecordMessage() const override { return msg_; }
    const std::filesystem::path& LastRecording() const override { return last_; }
    std::wstring PickFolder(const wchar_t* title) override { return pickFolder_(title); }

private:
    struct Run
    {
        bool active = false;
        int frame = 0, frames = 0;
        double animStart = 0;
        float yaw0 = 0;
        bool reload = false;
        std::filesystem::path out;
        std::string source;
        std::unique_ptr<FfmpegPipe> pipe;
    } run_;
    Session& s_;
    Playback& playback_;
    ReloadSync& reload_;
    MovesSim& movesSim_;
    Renderer& renderer_;
    OrbitCamera& camera_;
    RecordSettings& rec_;
    const UiShell& shell_;
    IViewHost& host_;
    FolderPicker pickFolder_;
    std::string msg_;
    std::filesystem::path last_;
};
