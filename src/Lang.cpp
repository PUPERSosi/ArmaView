// The ui:: wrappers: ImGui controls with translated labels (Strings.h), tooltips after ~0.5 s, the status bar hint,
// the Getting started highlight and the ImGui ID conflict check.
#include "Lang.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string_view>
#include <unordered_map>

namespace {

std::set<std::string> g_seenControls;
std::unordered_map<std::string, ImVec2> g_centers;   // control label / key -> screen centre (ui-script "hover")
std::unordered_map<ImGuiID, std::string> g_frameIds; // ids of the controls submitted this frame
std::set<ImGuiID> g_idSeen;                          // every control ID checked this session (coverage of the check)
std::set<std::string> g_idConflicts;                 // "window: label (id)" of two controls with one ID in a frame
std::string g_hintNow, g_hintShown, g_lastTipNow, g_lastTip, g_highlight;
ImGuiID g_sliderId = 0; // the last slider / drag drawn: its tooltips get the typing hint

std::string Generic(ui::Kind kind, const std::string& name)
{
    const bool ru = GetUiLang() == UiLang::Ru;
    switch (kind)
    {
    case ui::Kind::Button: return ru ? "Выполняет: " + name + "." : "Runs: " + name + ".";
    case ui::Kind::Check: return ru ? "Включает / выключает: " + name + "." : "Turns on / off: " + name + ".";
    case ui::Kind::Radio: return ru ? "Выбор варианта: " + name + "." : "Picks the option: " + name + ".";
    case ui::Kind::Slider:
        return ru ? name + ": перетащите, чтобы изменить.\nCtrl+клик — ввести значение." : name + ": drag to change.\nCtrl+click: type a value.";
    case ui::Kind::Input: return ru ? "Поле ввода: " + name + "." : "Input field: " + name + ".";
    case ui::Kind::Combo: return ru ? "Выбор из списка: " + name + "." : "Pick from the list: " + name + ".";
    case ui::Kind::Menu: return ru ? "Пункт меню: " + name + "." : "Menu item: " + name + ".";
    case ui::Kind::Header: return ru ? "Раздел «" + name + "»: щелчок - развернуть / свернуть." : "Section \"" + name + "\": click to expand / collapse.";
    default: return name;
    }
}

} // namespace

size_t IdsChecked() { return g_idSeen.size(); }
std::vector<std::string> IdConflicts() { return { g_idConflicts.begin(), g_idConflicts.end() }; }

namespace ui {

void BeginFrame()
{
    g_frameIds.clear();
    // ImGui's own detection (any control, also the ones drawn without the wrappers): the hovered ID was used twice
    if (const ImGuiContext* g = ImGui::GetCurrentContext(); g && g->DebugDrawIdConflictsId != 0 && g_idConflicts.size() < 500)
        g_idConflicts.insert(std::format("(hovered) {} items with ID 0x{:08X}", g->HoveredIdPreviousFrameItemCount, g->DebugDrawIdConflictsId));
    g_hintShown = g_hintNow;
    g_hintNow.clear();
    g_lastTip = g_lastTipNow;
    g_lastTipNow.clear();
}

const std::string& HoverHint() { return g_hintShown; }
std::string LastTip() { return g_lastTip.empty() ? g_lastTipNow : g_lastTip; }
void SetHighlight(const std::string& label) { g_highlight = label; }

bool ControlCenter(const std::string& label, float& x, float& y)
{
    auto it = g_centers.find(label);
    if (it == g_centers.end()) return false;
    x = it->second.x;
    y = it->second.y;
    return true;
}
const std::string& Highlight() { return g_highlight; }

bool Hovered()
{
    return ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay);
}

static void ShowTip(const std::string& tip)
{
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
    ImGui::TextUnformatted(tip.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
    g_lastTipNow = tip;
}

static void StatusHint(const std::string& tip)
{
    g_hintNow = tip;
    std::replace(g_hintNow.begin(), g_hintNow.end(), '\n', ' ');
}

void TrackId(const char* label)
{
    // ID conflicts: two controls of one frame with the same ID (they would share clicks / state)
    if (const ImGuiID id = ImGui::GetItemID(); id != 0)
    {
        g_idSeen.insert(id);
        auto [it, fresh] = g_frameIds.try_emplace(id, label ? label : "");
        if (!fresh && g_idConflicts.size() < 500)
            g_idConflicts.insert(std::format("{}: \"{}\" and \"{}\" (0x{:08X})", ImGui::GetCurrentWindow()->Name, it->second, label ? label : "", id));
    }
}

const char* TypeValueHint() { return GetUiLang() == UiLang::Ru ? "Ctrl+клик — ввести значение." : "Ctrl+click: type a value."; }

void Tip(const char* label, Kind kind)
{
    const std::string key = LabelKey(label);
    TrackId(label);
    if (kind == Kind::Slider) g_sliderId = ImGui::GetItemID();
    if (!g_highlight.empty() && !key.empty() && (key == g_highlight || g_highlight == label))
    {
        // Getting started: the control of the next step pulses
        const float t = float(ImGui::GetTime());
        const float a = 0.55f + 0.45f * std::sin(t * 5.0f);
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRect(ImVec2(lo.x - 4, lo.y - 4), ImVec2(hi.x + 4, hi.y + 4), ImGui::GetColorU32(ImVec4(1.0f, 0.75f, 0.1f, a)), 4.0f, 0, 3.0f);
    }
    {
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax(), c((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        if (g_centers.size() < 5000) { g_centers[label] = c; if (!key.empty()) g_centers[key] = c; }
    }
    // every control shown once: does it have an own tooltip (ui-script "lang tips")
    if (kind != Kind::Header && !key.empty() && g_seenControls.size() < 5000 && g_seenControls.insert(label).second && TipOf(label).empty())
        NoteMissingTip(key);
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    std::string tip = TipOf(label);
    if (tip.empty())
    {
        if (key.empty()) return;
        tip = Generic(kind, T(key.c_str()));
    }
    else if (kind == Kind::Slider) tip += std::string("\n") + TypeValueHint();
    StatusHint(tip);
    if (Hovered()) ShowTip(tip);
}

void SetTooltipText(const char* text)
{
    std::string withHint;
    if (const ImGuiID id = ImGui::GetItemID(); id != 0 && id == g_sliderId && !std::string_view(text).ends_with(TypeValueHint()))
    {
        withHint = std::string(text) + "\n" + TypeValueHint();
        text = withHint.c_str();
    }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
    g_lastTipNow = text;
    StatusHint(text);
}

void HelpMarker(const char* text)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        const char* t = T(text);
        StatusHint(t);
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(t);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
        g_lastTipNow = t;
    }
}

void EmptyState(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.8f, 1.0f, 1.0f));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(T(text));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

bool Begin(const char* name, bool* open, ImGuiWindowFlags flags)
{
    if (!name || strstr(name, "###") || (name[0] == '#' && name[1] == '#')) return ImGui::Begin(name, open, flags);
    static std::unordered_map<std::string, std::string> cache[2];
    std::string& full = cache[GetUiLang() == UiLang::Ru][name];
    if (full.empty()) full = std::string(T(name)) + "###" + name;
    return ImGui::Begin(full.c_str(), open, flags);
}


} // namespace ui
