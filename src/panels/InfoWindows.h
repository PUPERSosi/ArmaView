#pragma once
// Small floating windows that only show information: About (version, build, libraries) and Help (F1: keys, commands,
// windows, command line, ui-script - generated from the registries).
#include "Commands.h"

#include <imgui.h>

#include <vector>

class AboutWindow
{
public:
    void Draw(float dpi);
    bool open = false;
};

class HelpWindow
{
public:
    explicit HelpWindow(const std::vector<Hotkey>& hotkeys) : hotkeys_(hotkeys) {}
    void Draw(float dpi);
    bool open = false;

private:
    const std::vector<Hotkey>& hotkeys_;
    ImGuiTextFilter filter_;
};
