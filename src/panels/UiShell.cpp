#include "UiShell.h"
#include "LayoutStore.h"
#include "Camera.h"
#include "Lang.h"
#include "Log.h"
#include "Text.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace {

constexpr float kPi = 3.14159265f;
const ImVec4 kWarnColor = { 1.0f, 0.8f, 0.3f, 1 };
const ImVec4 kErrColor = { 1.0f, 0.4f, 0.4f, 1 };

std::string Utf8(uint32_t cp)
{
    std::string s;
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
    else { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
    return s;
}

// Segoe Fluent Icons / MDL2 code points
enum : uint32_t
{
    kIcoOpen = 0xE838, kIcoReload = 0xE72C, kIcoPlay = 0xE768, kIcoPause = 0xE769, kIcoPrev = 0xE892, kIcoNext = 0xE893,
    kIcoGrid = 0xE80A, kIcoSkeleton = 0xE805, kIcoWire = 0xF158, kIcoShadow = 0xE7A1, kIcoReflect = 0xE793, kIcoFrame = 0xE9A6,
    kIcoSearch = 0xE721, kIcoCrew = 0xE804,
};

} // namespace

const char* UiShell::ModeName(UiMode m) { return AppModeName(m); }

std::string UiShell::WinName(const char* title) const { return std::format("{}###{}_{}", T(title), title, int(mode)); }

float UiShell::ApplyUiScale()
{
    const float s = uiScale > 0.0f ? uiScale : systemDpi;
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 3.0f;
    st.FrameRounding = 3.0f;
    st.TabRounding = 3.0f;
    st.ScaleAllSizes(s);
    st.HoverDelayNormal = 0.5f; // tooltips of the controls (ui::Tip) after ~0.5 s
    st.FontScaleMain = s / systemDpi; // the font was loaded for the system DPI
    return s;
}

void UiShell::DrawModeBar()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight() * 2.0f + st.ItemSpacing.y + st.WindowPadding.y * 2.0f;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginViewportSideBar("##modebar", vp, ImGuiDir_Up, h, flags))
    {
        // mode tabs
        auto& log = Log::Get();
        for (int i = 0; i < int(UiMode::Count); ++i)
        {
            const UiMode m = UiMode(i);
            std::string label = T(ModeName(m));
            if (m == UiMode::Validation && (log.Errors() || log.Warnings())) label += std::format("  {}/{}", log.Errors(), log.Warnings());
            label += std::format("###mode{}", i);
            const bool active = m == mode;
            // read once: a background load may log an error between the push and the pop
            const bool red = m == UiMode::Validation && log.Errors() > 0;
            ImGui::PushStyleColor(ImGuiCol_Button, active ? st.Colors[ImGuiCol_TabSelected] : st.Colors[ImGuiCol_Tab]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, st.Colors[ImGuiCol_TabHovered]);
            if (red) ImGui::PushStyleColor(ImGuiCol_Text, kErrColor);
            if (ImGui::Button(label.c_str())) view_.SetMode(m);
            ui::Tip((std::string("##") + ModeName(m)).c_str(), ui::Kind::Button); // tooltip: the mode's row of the string table
            ImGui::PopStyleColor(red ? 3 : 2);
            ImGui::SameLine();
        }
        const std::string pal = (hasIcons ? Utf8(kIcoSearch) + "  " : std::string()) + T("Commands") + "  Ctrl+Shift+P##palette";
        const float palW = ImGui::CalcTextSize(pal.c_str()).x + st.FramePadding.x * 2.0f;
        const float resetW = ImGui::CalcTextSize(T("Reset layout")).x + st.FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - palW - resetW - st.ItemSpacing.x);
        if (ui::Button("Reset layout")) resetLayout[int(mode)] = true;
        ImGui::SameLine();
        if (ImGui::Button(pal.c_str())) { palette_.open = true; palette_.focus = true; }
        ui::Tip("##palette", ui::Kind::Button);

        DrawToolbar();
    }
    ImGui::End();
}

void UiShell::DrawToolbar()
{
    const ImGuiStyle& st = ImGui::GetStyle();
    auto icon = [&](uint32_t cp, const char* fallback, const char* tip, bool active, bool enabled = true) {
        std::string label = (hasIcons ? Utf8(cp) : std::string(fallback)) + "##" + tip;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, st.Colors[ImGuiCol_ButtonActive]);
        ImGui::BeginDisabled(!enabled);
        bool pressed = ui::Button(label.c_str());
        ImGui::EndDisabled();
        if (active) ImGui::PopStyleColor();
        if (ui::Hovered()) ui::SetTooltip("%s", TipOf(("##" + std::string(tip)).c_str()).empty() ? T(tip) : TipOf(("##" + std::string(tip)).c_str()).c_str());
        ImGui::SameLine();
        return pressed;
    };
    auto sep = [&] {
        ImGui::SameLine(0, st.ItemSpacing.x * 2.0f);
        ui::TextDisabled("|");
        ImGui::SameLine(0, st.ItemSpacing.x * 2.0f);
    };

    if (icon(kIcoOpen, "Open", "Open model / RTM (Ctrl+O)", false)) view_.ShowOpenDialog();
    if (icon(kIcoReload, "Reload", "Reload (F5)", false, s_.hasModel)) view_.ReloadModel();
    sep();
    ImGui::SetNextItemWidth(190 * host_.Dpi());
    ImGui::BeginDisabled(!s_.hasModel);
    if (ui::BeginCombo("##lod", s_.hasModel ? std::format("LOD {}  {}", s_.lod, s_.model.lods[s_.lod].Name()).c_str() : "LOD"))
    {
        for (int i = 0; s_.hasModel && i < int(s_.model.lods.size()); ++i)
            if (ui::SelectableRaw(std::format("{}  {}", i, s_.model.lods[i].Name()).c_str(), i == s_.lod) && i != s_.lod)
            {
                s_.lod = i;
                host_.LodChanged();
            }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ui::Hovered()) ui::SetTooltip("LOD (PgUp / PgDn)");
    ImGui::SameLine();
    sep();
    if (icon(pb_.playing ? kIcoPause : kIcoPlay, pb_.playing ? "Pause" : "Play", "Play / pause (Space)", pb_.playing)) pb_.playing = !pb_.playing;
    if (icon(kIcoPrev, "<", "Previous frame (Left)", false)) pb_.StepFrame(-1);
    if (icon(kIcoNext, ">", "Next frame (Right)", false)) pb_.StepFrame(1);
    sep();
    if (icon(kIcoGrid, "Grid", "Show grid (G)", d_.showGrid)) d_.showGrid = !d_.showGrid;
    if (icon(kIcoSkeleton, "Bones", "Show skeleton (B)", d_.showSkeleton)) d_.showSkeleton = !d_.showSkeleton;
    if (icon(kIcoWire, "Wire", "Wireframe overlay (W)", d_.wire != WireMode::Off)) d_.wire = d_.wire == WireMode::Off ? WireMode::Overlay : WireMode::Off;
    if (icon(kIcoCrew, "Crew", "Show crew on proxies", s_.attach.showCrew))
    {
        s_.attach.showCrew = !s_.attach.showCrew;
        if (s_.hasModel) host_.RebuildGeometry();
    }
    if (icon(kIcoShadow, "Shadows", "Sun shadows", d_.shadows)) d_.shadows = !d_.shadows;
    if (icon(kIcoReflect, "Refl", "Reflections A/B (E)", d_.reflections)) d_.reflections = !d_.reflections;
    sep();
    if (ui::Button("View")) ImGui::OpenPopup("##viewpop");
    if (ui::Hovered()) ui::SetTooltip("Shading, overlays, points / hitpoints");
    ImGui::SameLine();
    if (ui::Button("Light")) ImGui::OpenPopup("##lightpop");
    if (ui::Hovered()) ui::SetTooltip("Sun / headlight, time of day, exposure, shadows");
    ImGui::SameLine();
    if (ImGui::BeginPopup("##viewpop"))
    {
        dp_.DrawViewOptions();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##lightpop"))
    {
        dp_.DrawLightOptions();
        ImGui::EndPopup();
    }
    sep();
    struct V { const char* name; const char* key; float yaw, pitch; } views[] = {
        { "Front", "1", kPi, 0 }, { "Back", "2", 0, 0 }, { "Left", "3", kViewLeftYaw, 0 }, { "Right", "4", kViewRightYaw, 0 }, { "Top", "5", kPi, 1.55f } };
    for (const auto& v : views)
    {
        if (ui::Button(v.name)) view_.SetView(v.yaw, v.pitch);
        if (ui::Hovered()) ui::SetTooltip("Camera %s (%s)", v.name, v.key);
        ImGui::SameLine();
    }
    if (icon(kIcoFrame, "Frame", "Frame the model (F)", false, s_.hasModel)) view_.FrameModel();
    ImGui::NewLine();
}

void UiShell::DrawStatusBar()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##statusbar", vp, ImGuiDir_Down, ImGui::GetFrameHeight(), flags))
    {
        if (ImGui::BeginMenuBar())
        {
            auto& log = Log::Get();
            if (s_.hasModel)
            {
                ui::TextUnformatted(PathToUtf8(s_.model.path.filename()).c_str());
                if (ui::Hovered()) ui::SetTooltip("%s", PathToUtf8(s_.model.path).c_str());
                ui::TextDisabled("|");
                ui::Text("LOD %d %s", s_.lod, s_.model.lods[s_.lod].Name().c_str());
            }
            else
                ui::TextDisabled("no model - Ctrl+O or drop a .p3d");
            ui::TextDisabled("|");
            ui::Text("%.2f ms", view_.FrameCpuMs());
            if (ui::Hovered()) ui::SetTooltip("CPU time of the last frame (without the vsync wait)");
            ui::TextDisabled("|");
            const std::string problems = std::format("{} error(s), {} warning(s)##status", log.Errors(), log.Warnings());
            ImGui::PushStyleColor(ImGuiCol_Text, log.Errors() ? kErrColor : log.Warnings() ? kWarnColor : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            if (ui::SelectableRaw(problems.c_str(), false, 0, ImVec2(ImGui::CalcTextSize(problems.c_str(), nullptr, true).x, 0))) view_.SetMode(UiMode::Validation);
            ImGui::PopStyleColor();
            if (ui::Hovered()) ui::SetTooltip("Open Validation (Ctrl+5)");
            ui::TextDisabled("|");
            ui::TextDisabled(view_.GameConfigsLoading() ? "game configs: loading" : s_.game ? "game configs: loaded" : "game configs: not on P:");
            ui::TextDisabled("|");
            if (watcher_.enabled) ui::TextDisabled("hot reload: %zu files", watcher_.Count());
            else ui::TextDisabled("hot reload off");
            float fraction = 0;
            if (const std::string what = view_.LoadProgress(fraction); !what.empty())
            {
                ui::TextDisabled("|");
                const float t = float(ImGui::GetTime());
                const float f = fraction >= 0 ? fraction : 0.5f + 0.5f * std::sin(t * 4.0f); // not known: a pulsing bar
                ImGui::ProgressBar(f, ImVec2(160 * host_.Dpi(), 0), "");
                ImGui::SameLine();
                ImGui::TextUnformatted(what.c_str());
            }
            if (!ui::HoverHint().empty())
            {
                ui::TextDisabled("|");
                ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "%s", ui::HoverHint().c_str());
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

ImGuiID UiShell::DrawDockHost()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                   ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ui::Begin("##dockhost", nullptr, flags);
    ImGui::PopStyleVar(3);
    ImGuiID active = 0;
    for (int i = 0; i < int(UiMode::Count); ++i)
    {
        const ImGuiID id = ImGui::GetID(std::format("dock_mode_{}", i).c_str());
        if (UiMode(i) == mode)
        {
            ImGuiDockNode* node = ImGui::DockBuilderGetNode(id);
            if (resetLayout[i] || !node || (node->IsLeafNode() && node->Windows.Size == 0)) // new, reset, or emptied
            {
                resetLayout[i] = false;
                BuildModeLayout(UiMode(i), id);
            }
            ImGui::DockSpace(id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
            active = id;
        }
        else if (ImGui::DockBuilderGetNode(id))
            ImGui::DockSpace(id, ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly); // other modes keep their layout (not created before first use)
    }
    ImGui::End();
    return active;
}

void UiShell::BuildModeLayout(UiMode m, ImGuiID dock)
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, int(ImGuiDockNodeFlags_DockSpace) | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dock, vp->WorkSize);
    ImGuiID left = 0, right = 0, bottom = 0, center = dock;
    const float leftFrac = m == UiMode::Validation ? 0.38f : 0.24f;
    const float bottomFrac = m == UiMode::Animation || m == UiMode::Vehicle ? 0.36f : m == UiMode::Reload || m == UiMode::Optics ? 0.42f : 0.22f;
    if (m != UiMode::Vehicle) ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, leftFrac, &left, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, &right, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, bottomFrac, &bottom, &center);
    auto dockWin = [&](const char* title, ImGuiID node) { ImGui::DockBuilderDockWindow(WinName(title).c_str(), node); };
    switch (m)
    {
    case UiMode::Model:
        dockWin("Outliner", left);
        dockWin("Log", bottom);
        dockWin("Unit", bottom);
        break;
    case UiMode::Animation:
        dockWin("Outliner", left);
        dockWin("Timeline", bottom);
        dockWin("Moves", bottom);
        dockWin("Move editor", bottom);
        dockWin("Retarget", bottom);
        dockWin("Layers", bottom);
        dockWin("Sources", bottom);
        dockWin("Unit", bottom);
        dockWin("Log", bottom);
        break;
    case UiMode::Vehicle:
        dockWin("Crew", bottom);
        dockWin("Sources", bottom);
        dockWin("Log", bottom);
        break;
    case UiMode::Materials:
        dockWin("Materials", left);
        dockWin("Log", bottom);
        break;
    case UiMode::Validation:
        dockWin("Validation", left);
        dockWin("Log", bottom);
        break;
    case UiMode::Reload:
        dockWin("Outliner", left);
        dockWin("Reload sync", bottom);
        dockWin("Sources", bottom);
        dockWin("Log", bottom);
        break;
    case UiMode::Optics:
        dockWin("Outliner", left);
        dockWin("Optics", bottom);
        dockWin("Log", bottom);
        break;
    default: break;
    }
    dockWin("Inspector", right);
    ImGui::DockBuilderFinish(dock);
    // the first window of the bottom node is the visible tab
    focusFirstTab = 3;
}

void UiShell::DrawMenu(const ShellWindows& w)
{
    if (!ImGui::BeginMainMenuBar()) return;
    if (ui::BeginMenu("File"))
    {
        if (ui::MenuItem("Open...", "Ctrl+O")) view_.ShowOpenDialog();
        if (ui::MenuItem("Reload##menu", "F5", false, s_.hasModel)) view_.ReloadModel();
        if (ui::MenuItem("RTM folder...")) view_.ShowFolderDialog();
        if (ui::MenuItem("Mod check...")) *w.modCheck = true;
        if (ui::MenuItem("Record video / GIF...")) *w.record = true;
        ImGui::Separator();
        if (ui::MenuItem("Exit")) view_.RequestClose();
        ImGui::EndMenu();
    }
    if (ui::BeginMenu("Help"))
    {
        if (w.start) ui::MenuItem("Start screen", nullptr, w.start);
        if (w.setup) ui::MenuItem("Setup and paths", nullptr, w.setup);
        ui::MenuItem("Getting started", nullptr, w.guide);
        ui::MenuItem("Help", "F1", w.help);
        ui::MenuItem("About ArmaView", nullptr, w.about);
        ImGui::EndMenu();
    }
    if (ui::BeginMenu("Window"))
    {
        if (ui::MenuItem("Command palette", "Ctrl+Shift+P")) { palette_.open = true; palette_.focus = true; }
        for (int i = 0; i < int(UiMode::Count); ++i)
            if (ui::MenuItem(ModeName(UiMode(i)), std::format("Ctrl+{}", i + 1).c_str(), mode == UiMode(i))) view_.SetMode(UiMode(i));
        ImGui::Separator();
        ui::MenuItem("model.cfg graph", "Ctrl+G", w.graph);
        ImGui::Separator();
        DrawLayoutsMenu();
        if (ui::BeginMenu("UI scale"))
        {
            if (ui::MenuItem(std::format("System ({:.0f}%)", systemDpi * 100.0f).c_str(), nullptr, uiScale == 0.0f)) { uiScale = 0.0f; uiScaleDirty = true; ImGui::MarkIniSettingsDirty(); }
            for (float sc : { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f })
                if (ui::MenuItem(std::format("{:.0f}%", sc * 100.0f).c_str(), nullptr, uiScale == sc)) { uiScale = sc; uiScaleDirty = true; ImGui::MarkIniSettingsDirty(); }
            ImGui::EndMenu();
        }
        ui::MenuItem("Hot reload (watch files)", nullptr, &watcher_.enabled);
        if (ui::BeginMenu("Language / Язык"))
        {
            if (ImGui::MenuItem("Русский", nullptr, GetUiLang() == UiLang::Ru)) { SetUiLang(UiLang::Ru); ImGui::MarkIniSettingsDirty(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Интерфейс на русском");
            if (ImGui::MenuItem("English", nullptr, GetUiLang() == UiLang::En)) { SetUiLang(UiLang::En); ImGui::MarkIniSettingsDirty(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Interface in English");
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

// named layouts (snapshots of all modes), reset, export / import of the settings file
void UiShell::DrawLayoutsMenu()
{
    if (!ui::BeginMenu("Layouts")) return;
    if (ui::MenuItem("Reset layout of this mode")) resetLayout[int(mode)] = true;
    if (ui::MenuItem("Reset layout of all modes"))
        for (auto& r : resetLayout) r = true;
    ImGui::Separator();
    ImGui::SetNextItemWidth(160 * host_.Dpi());
    ui::InputTextWithHint("##layoutname", "name", layoutName_, sizeof(layoutName_));
    ImGui::SameLine();
    ImGui::BeginDisabled(!layoutName_[0]);
    if (ui::Button("Save current"))
    {
        layouts_.SaveAs(layoutName_);
        layoutName_[0] = 0;
    }
    ImGui::EndDisabled();
    const auto names = layouts_.Names();
    if (names.empty()) ui::TextDisabled("no saved layouts");
    std::string remove;
    for (const auto& n : names)
    {
        ImGui::PushID(n.c_str());
        if (ui::MenuItem(n.c_str())) layouts_.Use(n);
        ImGui::SameLine();
        if (ui::SmallButton("x")) remove = n;
        ImGui::PopID();
    }
    if (!remove.empty()) layouts_.Remove(remove);
    ImGui::Separator();
    if (ui::MenuItem("Export settings...")) view_.ExportSettings();
    if (ui::MenuItem("Import settings...")) view_.ImportSettings();
    ImGui::EndMenu();
}
