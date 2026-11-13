#pragma once
// Record window: what (the animation once / a turn), format (MP4 / GIF via ffmpeg, PNG frames), size, fps, output; the
// progress of a running recording. The recording itself is the viewer's (offscreen frames in the frame loop).
#include "ViewHost.h"

#include <filesystem>
#include <string>

struct RecordSettings
{
    int what = 0;    // 0 the animation once, 1 a 360 degree turn around the model
    int format = 0;  // 0 MP4, 1 GIF, 2 PNG frames
    int width = 1280, height = 720, fps = 30;
    float turnSeconds = 6.0f;
    char ffmpeg[260] = "ffmpeg";
    char outDir[400] = {};
};

// What the Record window asks of the viewer (implemented by App).
class IRecordView
{
public:
    virtual ~IRecordView() = default;
    virtual float RecordAnimLength(std::string* what) const = 0; // seconds of "the animation once" (0: nothing plays)
    virtual bool StartRecording() = 0;
    virtual void StopRecording(bool completed) = 0;
    virtual bool Recording() const = 0;
    virtual int RecordFrame() const = 0;
    virtual int RecordFrames() const = 0;
    virtual const std::string& RecordMessage() const = 0;
    virtual const std::filesystem::path& LastRecording() const = 0;
    virtual std::wstring PickFolder(const wchar_t* title) = 0;
};

class RecordPanel
{
public:
    RecordPanel(IViewHost& host, IRecordView& view) : host_(host), view_(view) {}
    void Draw();

    bool open = false;
    RecordSettings settings;

private:
    IViewHost& host_;
    IRecordView& view_;
};
