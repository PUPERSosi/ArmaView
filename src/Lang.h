#pragma once
// UI side of the language (string table: Strings.h, core). Every label and text of the panels goes through
// T(); the ui:: wrappers below call ImGui with the translated label and then show the control's tooltip (string table,
// else the hotkey registry) after ~0.5 s, put it in the status bar and draw the Getting started highlight.
#include "Strings.h"

#include <imgui.h>

#include <cstdio>
#include <format>
#include <string>
#include <utility>
#include <vector>

// controls drawn with the same ImGui ID in one frame (seen by the ui:: wrappers), "window: label and label (id)"
std::vector<std::string> IdConflicts();
size_t IdsChecked(); // distinct control IDs seen by the check this session

namespace ui {

enum class Kind { Button, Check, Radio, Slider, Input, Combo, Menu, Header, Other };

void BeginFrame();                            // once per frame, before the panels
const std::string& HoverHint();               // tooltip of the control under the mouse (status bar)
std::string LastTip();                        // ui-script: tooltip shown in the last frame
void Tip(const char* label, Kind kind);       // after an item: delayed tooltip, status hint, highlight
void TrackId(const char* label);              // after an item: ID conflict check only (Tip does it too)
bool Hovered();                               // the last item is hovered long enough for a tooltip (~0.5 s)
void SetHighlight(const std::string& label);  // Getting started: the control of the next step ("" = none)
bool ControlCenter(const std::string& label, float& x, float& y); // ui-script "hover": a control drawn last frame
const std::string& Highlight();
void HelpMarker(const char* text);            // "(?)": what this panel is and how to use it
void EmptyState(const char* text);            // a panel with nothing to show: what to do

inline const char* TrNoRecord(const char* s) { return ::TrNoRecord(s); }
void SetTooltipText(const char* text);      // tooltip (wrapped) + status hint; after a slider / drag: + "Ctrl+click: type a value"
const char* TypeValueHint();                // "Ctrl+click: type a value" in the UI language
template <class... A> void SetTooltip(const char* fmt, A... a)
{
    char buf[2048];
    if constexpr (sizeof...(A) == 0) snprintf(buf, sizeof(buf), "%s", T(fmt));
    else snprintf(buf, sizeof(buf), T(fmt), a...);
    SetTooltipText(buf);
}

// ---- controls: label translated, tooltip after the item (arguments pass through as the call site wrote them)
#pragma warning(push)
#pragma warning(disable : 4244)
// controls labelled with data (names, paths): not translated, no own tooltip, but checked for ID conflicts
template <class... A> bool SelectableRaw(const char* l, A... a) { const bool r = ImGui::Selectable(l, a...); TrackId(l); return r; }
template <class... A> bool SmallButtonRaw(const char* l, A... a) { const bool r = ImGui::SmallButton(l, a...); TrackId(l); return r; }
template <class... A> bool Button(const char* l, A... a) { const bool r = ImGui::Button(T(l), a...); Tip(l, Kind::Button); return r; }
template <class... A> bool SmallButton(const char* l, A... a) { const bool r = ImGui::SmallButton(T(l), a...); Tip(l, Kind::Button); return r; }
template <class... A> bool Checkbox(const char* l, A... a) { const bool r = ImGui::Checkbox(T(l), a...); Tip(l, Kind::Check); return r; }
template <class... A> bool RadioButton(const char* l, A... a) { const bool r = ImGui::RadioButton(T(l), a...); Tip(l, Kind::Radio); return r; }
template <class... A> bool SliderFloat(const char* l, A... a) { const bool r = ImGui::SliderFloat(T(l), a...); Tip(l, Kind::Slider); return r; }
template <class... A> bool SliderFloat2(const char* l, A... a) { const bool r = ImGui::SliderFloat2(T(l), a...); Tip(l, Kind::Slider); return r; }
template <class... A> bool SliderFloat3(const char* l, A... a) { const bool r = ImGui::SliderFloat3(T(l), a...); Tip(l, Kind::Slider); return r; }
template <class... A> bool SliderInt(const char* l, A... a) { const bool r = ImGui::SliderInt(T(l), a...); Tip(l, Kind::Slider); return r; }
template <class... A> bool SliderAngle(const char* l, A... a) { const bool r = ImGui::SliderAngle(T(l), a...); Tip(l, Kind::Slider); return r; }
template <class... A> bool DragFloat(const char* l, A... a) { const bool r = ImGui::DragFloat(T(l), a...); Tip(l, Kind::Slider); return r; }
template <class... A> bool DragFloat2(const char* l, A... a) { const bool r = ImGui::DragFloat2(T(l), a...); Tip(l, Kind::Slider); return r; }
template <class... A> bool DragFloat3(const char* l, A... a) { const bool r = ImGui::DragFloat3(T(l), a...); Tip(l, Kind::Slider); return r; }
template <class... A> bool DragInt(const char* l, A... a) { const bool r = ImGui::DragInt(T(l), a...); Tip(l, Kind::Slider); return r; }
template <class... A> bool DragInt2(const char* l, A... a) { const bool r = ImGui::DragInt2(T(l), a...); Tip(l, Kind::Slider); return r; }
template <class... A> bool DragFloatRange2(const char* l, A... a) { const bool r = ImGui::DragFloatRange2(T(l), a...); Tip(l, Kind::Slider); return r; }
template <class... A> bool InputText(const char* l, A... a) { const bool r = ImGui::InputText(T(l), a...); Tip(l, Kind::Input); return r; }
template <class... A> bool InputTextWithHint(const char* l, const char* hint, A... a)
{
    const bool r = ImGui::InputTextWithHint(T(l), T(hint), a...);
    Tip(l[0] == '#' && l[1] == '#' && !TipOf(l).size() ? hint : l, Kind::Input);
    return r;
}
template <class... A> bool InputTextMultiline(const char* l, A... a) { const bool r = ImGui::InputTextMultiline(T(l), a...); Tip(l, Kind::Input); return r; }
template <class... A> bool InputFloat(const char* l, A... a) { const bool r = ImGui::InputFloat(T(l), a...); Tip(l, Kind::Input); return r; }
template <class... A> bool InputFloat3(const char* l, A... a) { const bool r = ImGui::InputFloat3(T(l), a...); Tip(l, Kind::Input); return r; }
template <class... A> bool InputInt(const char* l, A... a) { const bool r = ImGui::InputInt(T(l), a...); Tip(l, Kind::Input); return r; }
template <class... A> bool InputInt2(const char* l, A... a) { const bool r = ImGui::InputInt2(T(l), a...); Tip(l, Kind::Input); return r; }
template <class... A> bool ColorEdit3(const char* l, A... a) { const bool r = ImGui::ColorEdit3(T(l), a...); Tip(l, Kind::Input); return r; }
template <class... A> bool ColorEdit4(const char* l, A... a) { const bool r = ImGui::ColorEdit4(T(l), a...); Tip(l, Kind::Input); return r; }
template <class... A> bool Combo(const char* l, A... a) { const bool r = ImGui::Combo(T(l), a...); Tip(l, Kind::Combo); return r; }
// item lists written as arrays of literals are translated too
inline bool Combo(const char* l, int* cur, const char** items, int n, int h = -1)
{
    const char* tr[32];
    for (int i = 0; i < n && i < 32; ++i) tr[i] = T(items[i]);
    const bool r = ImGui::Combo(T(l), cur, tr, n < 32 ? n : 32, h);
    Tip(l, Kind::Combo);
    return r;
}
inline bool BeginCombo(const char* l, const char* preview, ImGuiComboFlags f = 0)
{
    const bool r = ImGui::BeginCombo(T(l), TrNoRecord(preview), f);
    if (!r) Tip(l, Kind::Combo); // open: the last item is the popup's
    return r;
}
template <class... A> bool MenuItem(const char* l, A... a) { const bool r = ImGui::MenuItem(T(l), a...); Tip(l, Kind::Menu); return r; }
inline bool BeginMenu(const char* l, bool enabled = true) { return ImGui::BeginMenu(T(l), enabled); }
template <class... A> bool CollapsingHeader(const char* l, A... a) { const bool r = ImGui::CollapsingHeader(T(l), a...); Tip(l, Kind::Header); return r; }
inline bool BeginTabItem(const char* l, bool* open = nullptr, ImGuiTabItemFlags f = 0) { return ImGui::BeginTabItem(T(l), open, f); }

#pragma warning(pop)

// ---- windows: the title is translated, the ID stays the English title ("###")
bool Begin(const char* name, bool* open = nullptr, ImGuiWindowFlags flags = 0);

// ---- texts (printf formats go through the table as they are written)
template <class... A> void Text(const char* f, A... a) { ImGui::Text(T(f), a...); }
template <class... A> void TextDisabled(const char* f, A... a) { ImGui::TextDisabled(T(f), a...); }
template <class... A> void TextWrapped(const char* f, A... a) { ImGui::TextWrapped(T(f), a...); }
template <class... A> void TextColored(const ImVec4& c, const char* f, A... a) { ImGui::TextColored(c, T(f), a...); }
template <class... A> void BulletText(const char* f, A... a) { ImGui::BulletText(T(f), a...); }
inline void SeparatorText(const char* l) { ImGui::SeparatorText(T(l)); }
inline void TableSetupColumn(const char* l, ImGuiTableColumnFlags f = 0, float w = 0.0f, ImGuiID id = 0) { ImGui::TableSetupColumn(TrNoRecord(l), f, w, id); }
inline bool TreeNode(const char* l) { return ImGui::TreeNode(T(l)); }
template <class... A> bool TreeNode(const char* id, const char* fmt, A... a) { return ImGui::TreeNode(id, T(fmt), a...); }
inline void TextUnformatted(const char* s, const char* e = nullptr) { ImGui::TextUnformatted(e ? s : TrNoRecord(s), e); }

} // namespace ui
