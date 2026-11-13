#pragma once
// Registries the Help window (F1) is generated from: command-line options (parsed through this table in main.cpp),
// hotkeys (registered and dispatched by App), windows and --ui-script commands.
#include <imgui.h>

#include <functional>
#include <string>
#include <vector>

struct Automation;

struct CliOption
{
    const wchar_t* name;        // L"--lod"
    const char* arg;            // "N" (nullptr = flag)
    const char* group;
    const char* description;
    std::function<void(Automation&, const std::wstring& value)> apply;
};
const std::vector<CliOption>& CliOptions();

struct Hotkey
{
    ImGuiKeyChord chord = ImGuiKey_None; // ImGuiKey_None: mouse / local key described in `keys`
    const char* keys = nullptr;          // display text when chord is not enough ("LMB drag", "Esc (graph)")
    const char* group;
    const char* description;
    std::function<void()> run;           // null: handled where it applies (window-local, mouse)
    bool needsModel = false;
};

struct HelpEntry
{
    std::string name, description;
};
// --ui-script commands (UiScript.cpp parses exactly these names).
const std::vector<HelpEntry>& UiScriptCommands();

std::string ChordName(ImGuiKeyChord chord);
