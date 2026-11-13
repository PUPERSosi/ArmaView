#include "InfoWindows.h"
#include "Lang.h"
#include "Log.h"
#include "Version.h"

#include <algorithm>
#include <array>
#include <string>

void AboutWindow::Draw(float dpi)
{
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(560 * dpi, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (ui::Begin("About ArmaView", &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ui::Text("ArmaView %s", AV_VERSION_STRING);
        ui::TextDisabled("commit %s, built %s %s", AV_GIT_COMMIT, __DATE__, __TIME__);
#ifdef NDEBUG
        const char* config = "optimised";
#else
        const char* config = "debug";
#endif
        ui::TextDisabled("MSVC %d, C++%ld, %s build, %s runtime", _MSC_FULL_VER, _MSVC_LANG / 100 % 100, config,
#ifdef _DLL
                            "DLL (/MD)"
#else
                            "static (/MT)"
#endif
        );
        ImGui::Separator();
        ui::TextWrapped("A viewer for Arma 3 source content (MLOD, RTM, PAA, configs) and glTF: models, animations and materials without starting the game.");
        ImGui::TextUnformatted("Copyright (c) 2026 Pin. MIT License.");
        ImGui::Spacing();
        ui::TextUnformatted("Third-party libraries (MIT license):");
        ui::BulletText("Dear ImGui %s - Omar Cornut", IMGUI_VERSION);
        ui::BulletText("imgui-node-editor - Michal Cichon");
        ui::BulletText("GLM - G-Truc Creation");
        ui::BulletText("cgltf - Johannes Kuhlmann");
        ImGui::Spacing();
        ui::TextDisabled("Arma 3 is a trademark of Bohemia Interactive. This project is not affiliated with Bohemia Interactive.");
    }
    ImGui::End();
}

void HelpWindow::Draw(float dpi)
{
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(760 * dpi, 620 * dpi), ImGuiCond_FirstUseEver);
    if (!ui::Begin("Help (F1)", &open))
    {
        ImGui::End();
        return;
    }
    filter_.Draw(T("search"), 300 * dpi);
    ImGui::SameLine();
    ui::TextDisabled("keys by their place on the keyboard (US labels) - the same in any layout");
    auto match = [&](const std::string& a, const std::string& b, const std::string& c) {
        return filter_.PassFilter((a + " " + b + " " + c + " " + T(a) + " " + T(b) + " " + T(c)).c_str());
    };
    auto table = [&](const char* id, const std::vector<std::array<std::string, 3>>& rows, const char* c0, const char* c1) {
        std::vector<std::string> groups;
        for (const auto& r : rows)
            if (match(r[0], r[1], r[2]) && std::find(groups.begin(), groups.end(), r[2]) == groups.end()) groups.push_back(r[2]);
        for (const auto& g : groups)
        {
            ui::SeparatorText(g.c_str());
            if (ImGui::BeginTable((std::string(id) + g).c_str(), 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ui::TableSetupColumn(c0, ImGuiTableColumnFlags_WidthFixed, 230 * dpi);
                ui::TableSetupColumn(c1);
                for (const auto& r : rows)
                {
                    if (r[2] != g || !match(r[0], r[1], r[2])) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1), "%s", ui::TrNoRecord(r[0].c_str()));
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", ui::TrNoRecord(r[1].c_str()));
                }
                ImGui::EndTable();
            }
        }
    };
    if (ImGui::BeginTabBar("helptabs"))
    {
        if (ui::BeginTabItem("Keys & mouse"))
        {
            std::vector<std::array<std::string, 3>> rows;
            for (const auto& h : hotkeys_)
                if (h.keys || h.chord != ImGuiKey_None) rows.push_back({ h.keys ? std::string(h.keys) : ChordName(h.chord), h.description, h.group });
            table("k", rows, "key", "action");
            ImGui::EndTabItem();
        }
        if (ui::BeginTabItem("Commands"))
        {
            std::vector<std::array<std::string, 3>> rows;
            for (const auto& h : hotkeys_)
                if (h.run) rows.push_back({ h.description, h.chord != ImGuiKey_None ? ChordName(h.chord) : std::string("-"), h.group });
            ui::TextDisabled("Everything here runs from the command palette (Ctrl+Shift+P).");
            table("p", rows, "command", "key");
            ImGui::EndTabItem();
        }
        if (ui::BeginTabItem("Windows"))
        {
            const char* modes = "Modes (tabs on top, each with its own layout)";
            std::vector<std::array<std::string, 3>> rows = {
                { "Model (Ctrl+1)", "Outliner: LODs + selections; Inspector: LOD / selection / picked point; Log", modes },
                { "Skeleton & Anim (Ctrl+2)", "Outliner: bone tree; Timeline (RTM layers, onion, trail, A/B, states), Sources, Log", modes },
                { "Vehicle & Crew (Ctrl+3, Ctrl+R)", "Crew (seats, proxies, IK, get-in), Sources, Log; Inspector: seat (manual RTM, chain)", modes },
                { "Materials (Ctrl+4)", "sections + config skins; Inspector: texture, RVMAT editor, stages, preview", modes },
                { "Validation (Ctrl+5)", "rig, model.cfg animations, RTM bones, loop seam, crew problems; Log", modes },
                { "Inspector", "the last thing clicked: selection, bone, point / hit sphere, section, crew seat (LOD info otherwise)", "Panels" },
                { "Toolbar", "open, reload, LOD, play, grid / skeleton / wireframe / crew / shadows / reflections, View and Light options, camera", "Panels" },
                { "Status bar", "model, LOD, frame time, errors / warnings (click: Validation), game configs, hot reload", "Panels" },
                { "model.cfg graph (Ctrl+G)", "floating", "Panels" },
                { "Command palette (Ctrl+Shift+P)", "every command of the Commands tab, type to filter", "Panels" },
            };
            table("w", rows, "window", "contents");
            ImGui::EndTabItem();
        }
        if (ui::BeginTabItem("Command line"))
        {
            std::vector<std::array<std::string, 3>> rows;
            rows.push_back({ "<file.p3d|file.rtm> ...", "open files (a running viewer gets them through a named pipe)", "Files" });
            for (const auto& o : CliOptions())
                rows.push_back({ WideToUtf8(o.name) + (o.arg ? std::string(" ") + o.arg : ""), o.description, o.group });
            table("c", rows, "option", "meaning");
            ImGui::EndTabItem();
        }
        if (ui::BeginTabItem("ui-script"))
        {
            std::vector<std::array<std::string, 3>> rows;
            for (const auto& c : UiScriptCommands()) rows.push_back({ c.name, c.description, "--ui-script <file>: one command per line, # comments" });
            table("u", rows, "command", "meaning");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
