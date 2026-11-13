#include "RecordPanel.h"
#include "Lang.h"
#include "Text.h"

#include <imgui.h>
#include <imgui_internal.h> // MarkIniSettingsDirty

#include <windows.h>
#include <shellapi.h> // open the recorded file / its folder

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>

namespace {
const ImVec4 kWarnColor{ 1.0f, 0.8f, 0.3f, 1 };
} // namespace

void RecordPanel::Draw()
{
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(560 * host_.Dpi(), 0), ImGuiCond_FirstUseEver);
    if (!ui::Begin("Record", &open, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    ui::HelpMarker("Records the current view without the UI.\n"
                   "1. What: the animation once (RTM layers, the CfgMoves state, the reload in Reload sync) or a 360 degree turn around the model.\n"
                   "2. Format: MP4 (H.264) or GIF through ffmpeg, or a folder of PNG frames (no ffmpeg needed).\n"
                   "3. Size and FPS of the file - independent of the window; the camera, lighting and overlays are the current ones.\n"
                   "4. Record: the frames are rendered one by one at exact times (not in real time), then the file is written.");
    ImGui::BeginDisabled(view_.Recording());
    ui::RadioButton("Animation##rec", &settings.what, 0);
    ImGui::SameLine();
    ui::RadioButton("Turn 360##rec", &settings.what, 1);
    if (settings.what == 0)
    {
        std::string what;
        const float len = view_.RecordAnimLength(&what);
        ImGui::SameLine();
        if (len > 0) ui::TextDisabled("%s, %.2f s", what.c_str(), len);
        else ui::TextColored(kWarnColor, "%s", what.c_str());
    }
    else
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120 * host_.Dpi());
        ui::DragFloat("seconds##rec", &settings.turnSeconds, 0.1f, 0.5f, 60.0f, "%.1f s");
    }
    static const char* kFormats[] = { "MP4 (ffmpeg)", "GIF (ffmpeg)", "PNG frames" };
    ImGui::SetNextItemWidth(160 * host_.Dpi());
    ui::Combo("format##rec", &settings.format, kFormats, 3);
    ImGui::SetNextItemWidth(180 * host_.Dpi());
    int size[2] = { settings.width, settings.height };
    if (ui::DragInt2("size##rec", size, 2.0f, 16, 8192)) { settings.width = size[0]; settings.height = size[1]; ImGui::MarkIniSettingsDirty(); }
    ImGui::SameLine();
    for (auto [w, h] : { std::pair{ 1280, 720 }, std::pair{ 1920, 1080 }, std::pair{ 800, 800 }, std::pair{ 480, 480 } })
    {
        if (ui::SmallButtonRaw(std::format("{}x{}", w, h).c_str())) { settings.width = w; settings.height = h; ImGui::MarkIniSettingsDirty(); }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::SetNextItemWidth(120 * host_.Dpi());
    if (ui::SliderInt("fps##rec", &settings.fps, 5, 60)) ImGui::MarkIniSettingsDirty();
    ImGui::SetNextItemWidth(-60 * host_.Dpi());
    if (ui::InputTextWithHint("##ffmpeg", "ffmpeg.exe (or ffmpeg on PATH)", settings.ffmpeg, sizeof(settings.ffmpeg))) ImGui::MarkIniSettingsDirty();
    ImGui::SetNextItemWidth(-60 * host_.Dpi());
    if (ui::InputTextWithHint("##recdir", "output folder (empty: Videos\\ArmaView)", settings.outDir, sizeof(settings.outDir))) ImGui::MarkIniSettingsDirty();
    ImGui::SameLine();
    if (ui::Button("...##recdir"))
        if (std::wstring p = view_.PickFolder(L"Output folder"); !p.empty()) { strncpy_s(settings.outDir, WideToUtf8(p).c_str(), _TRUNCATE); ImGui::MarkIniSettingsDirty(); }
    ImGui::EndDisabled();
    if (view_.Recording())
    {
        ImGui::ProgressBar(float(view_.RecordFrame()) / float(std::max(1, view_.RecordFrames())), ImVec2(-90 * host_.Dpi(), 0), std::format("{} / {}", view_.RecordFrame(), view_.RecordFrames()).c_str());
        ImGui::SameLine();
        if (ui::Button("Stop##rec")) view_.StopRecording(false);
    }
    else if (ui::Button("Record##rec")) view_.StartRecording();
    if (!view_.RecordMessage().empty()) ImGui::TextWrapped("%s", view_.RecordMessage().c_str());
    if (!view_.LastRecording().empty() && !view_.Recording())
    {
        if (ui::SmallButton("Open##reclast")) ShellExecuteW(nullptr, L"open", view_.LastRecording().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        if (ui::SmallButton("Show in folder##reclast"))
            ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + view_.LastRecording().native() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    }
    ImGui::End();
}
