#include "StartScreen.h"
#include "Lang.h"

#include <imgui.h>
#include <imgui_internal.h> // MarkIniSettingsDirty

#include <cfloat>

namespace {

struct Task
{
    const char* label; // button (string table: title + tooltip)
    const char* about; // one line under it
    int mode;          // UiMode
    const char* panel; // panel brought to the front
};

const Task kTasks[] = {
    { "Model##task", "Check a P3D: LODs, selections, textures, rig, validation.", 0, "Outliner" },
    { "Animation##task", "Play RTM animations on the model's skeleton, blend layers.", 1, "Timeline" },
    { "CfgMoves##task", "Walk the character through its move config, as in the game.", 1, "Moves" },
    { "Unit##task", "Dress a soldier class from CfgVehicles: head, helmet, vest, NVG, backpack.", 0, "Unit" },
    { "Vehicle##task", "Crew seats, proxies and get-in points of a vehicle.", 2, "Crew" },
    { "Weapon##task", "Reload gesture against the weapon, optics on its slots.", 5, "Reload sync" },
};

const ImVec4 kOk{ 0.45f, 0.85f, 0.45f, 1 }, kMissing{ 1.0f, 0.55f, 0.35f, 1 }, kOptional{ 0.75f, 0.75f, 0.75f, 1 };

} // namespace

void StartScreen::Draw(float dpi)
{
    if (setupOpen) DrawSetup(dpi);
    else if (open) DrawStart(dpi);
}

void StartScreen::DrawStart(float dpi)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.45f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(660 * dpi, 0), ImVec2(660 * dpi, FLT_MAX)); // fixed width, height of the content
    if (!ui::Begin("What do you want to do?", &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 640 * dpi);
    ui::TextDisabled("Pick a task: the viewer opens its mode and panel. Then open a model (Ctrl+O or drop a .p3d onto the window).");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (ImGui::BeginTable("##tasks", 2, ImGuiTableFlags_SizingStretchSame, ImVec2(640 * dpi, 0)))
    {
        for (const Task& t : kTasks)
        {
            ImGui::TableNextColumn();
            if (ui::Button(t.label, ImVec2(-FLT_MIN, 34 * dpi)))
            {
                host_.StartTask(t.mode, t.panel);
                open = false;
            }
            ImGui::PushTextWrapPos(0.0f);
            ui::TextDisabled(t.about);
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    if (ui::Button("Open model...##start")) { open = false; host_.ShowOpenDialog(); }
    ImGui::SameLine();
    if (ui::Button("Step-by-step guide")) { open = false; host_.OpenGuide(); }
    ImGui::SameLine();
    if (ui::Button("Setup and paths")) setupOpen = true;
    ImGui::SameLine();
    if (ui::Checkbox("Show at startup", &showAtStart)) ImGui::MarkIniSettingsDirty();
    ImGui::End();
}

void StartScreen::DrawSetup(float dpi)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.45f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(800 * dpi, 0), ImVec2(800 * dpi, FLT_MAX)); // fixed width, height of the content
    bool keep = true;
    if (!ui::Begin("Setup and paths", &keep, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        if (!keep) setupOpen = false;
        return;
    }
    const IStartHost::Setup s = host_.SetupState();
    auto row = [&](const char* what, bool ok, bool required, const std::string& value, const char* fix) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ui::TextUnformatted(what);
        ImGui::TableNextColumn();
        ImGui::TextColored(ok ? kOk : required ? kMissing : kOptional, "%s", ok ? T("found") : required ? T("missing") : T("not set (optional)"));
        ImGui::TableNextColumn();
        ImGui::PushTextWrapPos(0.0f);
        if (!value.empty()) ImGui::TextUnformatted(value.c_str());
        if (!ok && fix) ui::TextDisabled(fix);
        ImGui::PopTextWrapPos();
    };
    const float width = 780 * dpi;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ui::TextDisabled("What the viewer uses on this machine. Nothing here is required to open a model; missing items only turn features off.");
    ImGui::PopTextWrapPos();
    if (ImGui::BeginTable("##setup", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp, ImVec2(width, 0)))
    {
        ui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthFixed, 170 * dpi);
        ui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 120 * dpi);
        ui::TableSetupColumn("Path");
        row("Settings folder", true, true, s.dataDir + (s.dataWhy.empty() ? "" : "  (" + s.dataWhy + ")"), nullptr);
        row("P: drive", s.pDrive, true, s.pTarget,
            "Mount the project drive: Arma 3 Tools > Project Drive Management (or Mount the project drive). Models use paths like \\a3\\... from P:.");
        row("Game data on P:\\a3", s.a3, true, s.gameConfigs,
            "Unpack the game data to P: (Arma 3 Tools > Project Drive Management > Extract game data). Without it config.cpp inheritance stops at the game's classes and BI textures are missing.");
        row("Arma 3 Samples", s.samplesOk, false, s.samples,
            "Arma 3 Samples (Steam: Arma 3 Samples): BI's sample man for the crew preview and as the retarget source.");
        row("Blender", s.blenderOk, false, s.blender, "blender.exe: the Layers window runs Blender scripts (tools/blender).");
        row("ffmpeg", s.ffmpegOk, false, s.ffmpeg, "ffmpeg.exe: video recording (MP4 / GIF). Set its path in the Record window.");
        ImGui::EndTable();
    }
    if (ui::Button("Set Arma 3 Samples folder...")) host_.PickSamples();
    ImGui::SameLine();
    if (ui::Button("Set blender.exe...")) host_.PickBlender();
    ui::TextDisabled("These two are saved in armaview.local.ini in the settings folder and take effect after a restart.");
    if (!s.localIni.empty()) ui::TextDisabled("Machine paths read from: %s", s.localIni.c_str());
    ImGui::Separator();
    if (ui::Button("Continue")) { setupOpen = false; open = true; }
    ImGui::End();
    if (!keep) setupOpen = false;
}
