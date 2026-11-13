#include "ReloadPanel.h"
#include "Lang.h"
#include "Text.h"

#include <imgui.h>

#include <algorithm>
#include <format>

void ReloadPanel::Draw(const std::string& windowName)
{
    if (!ui::Begin(windowName.c_str()))
    {
        ImGui::End();
        return;
    }
    ui::HelpMarker("Reload sync: the weapon's reload as in the game.\n"
                   "1. Open the weapon P3D (MLOD) - its CfgWeapons class is found by model = ....\n"
                   "2. The character is posed in weapon space: stance + handAnim + the reload gesture (reloadAction -> CfgGesturesMale).\n"
                   "3. Play / scrub: model.cfg reloadMagazine follows phase = clamp(k * t / T).\n"
                   "4. Touch (hand bone / weapon part) and Distance over time show where the hand meets the magazine; Suggest window proposes the model.cfg window.\n"
                   "5. Diff... / Save write the model.cfg (with .bak) - then Validation.");
    ImGui::SameLine();
    if (!s_.hasModel)
    {
        ui::EmptyState("Open a weapon P3D (MLOD) with Ctrl+O: its CfgWeapons class, reload gesture and model.cfg animations are found from it.");
        ImGui::End();
        return;
    }
    if (!s_.weapon.found)
    {
        ui::EmptyState(TF("No CfgWeapons class with model = {} - check the config.cpp of the addon (next to or above the model).",
                          ArmaPathOf(s_.model.path, s_.projectRoot)).c_str());
        ImGui::End();
        return;
    }
    DrawConfigChain();
    DrawTiming();
    DrawTransport();
    DrawTimeline();
    DrawSave();
    ImGui::Separator();
    DrawTouchHelper();
    if (!rs_.weaponMsg_.empty()) ui::TextWrapped("%s", rs_.weaponMsg_.c_str());
    ImGui::End();
}

// CfgWeapons class -> reloadAction -> gesture, handAnim, stance, reload time and switch phase
void ReloadPanel::DrawConfigChain()
{
    const WeaponCfg& w = s_.weapon;
    const auto& wc = rs_.wchar_;
    ui::Text("%s", w.cls.c_str());
    if (w.candidates.size() > 1)
    {
        ImGui::SameLine();
        ui::TextDisabled("(+%d classes with this model)", int(w.candidates.size()) - 1);
    }
    ui::TextDisabled("%s", w.ref.Where().c_str());
    ui::Text("reloadAction %s -> CfgGesturesMale >> %s: %s", w.reloadAction.empty() ? "-" : w.reloadAction.c_str(),
             wc.gestureName.empty() ? "-" : wc.gestureName.c_str(), PathToUtf8(wc.gestureFile.filename()).c_str());
    ui::TextDisabled("  speed %s -> %.3f s, mask %s   %s", ShortNum(wc.gestureSpeed).c_str(), wc.gestureDuration, wc.gestureMask.c_str(),
                     wc.gestureRef.Where().c_str());
    for (const auto& [m, a] : w.muzzleReloads) ui::TextDisabled("  muzzle %s: reloadAction %s", m.c_str(), a.c_str());
    ui::TextDisabled("handAnim %s   stance %s", w.handAnim.empty() ? "-" : w.handAnim.c_str(), wc.baseState.c_str());
    ui::TextDisabled("magazineReloadTime %s   magazineReloadSwitchPhase %s", w.magazineReloadTime < 0 ? "-" : ShortNum(w.magazineReloadTime).c_str(),
                     w.switchPhase < 0 ? "-" : ShortNum(w.switchPhase).c_str());
    if (!wc.problem.empty()) ui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", wc.problem.c_str());
}

// k, blending of handAnim / gesture, fade, hand IK
void ReloadPanel::DrawTiming()
{
    const auto& wc = rs_.wchar_;
    ImGui::SetNextItemWidth(120 * host_.Dpi());
    ui::DragFloat("k", &rs_.reloadK_, 0.005f, 0.2f, 3.0f, "%.3f");
    ImGui::SameLine();
    ui::TextDisabled("%s phase = clamp(k * t / %.3f s) - k = 1 until the in-game test confirms it", rs_.reloadSource_.c_str(), rs_.ReloadDuration());
    ui::TextUnformatted("handAnim / gesture blending:");
    ImGui::SameLine();
    ui::RadioButton("Model space", &s_.blendMode, 0);
    ImGui::SameLine();
    ui::RadioButton("Parent-relative", &s_.blendMode, 1);
    ImGui::SameLine();
    ui::Checkbox("Gesture fade in / out", &rs_.gestureFade_);
    ImGui::SameLine();
    if (ui::Checkbox("Hand IK (gesture curves)", &rs_.handIK_)) view_.CharacterPoseChanged();
    if (rs_.handIK_ && (wc.leftIK.size() >= 2 || wc.rightIK.size() >= 2))
    {
        ImGui::SameLine();
        ui::TextDisabled("L %.2f  R %.2f", rs_.handIKWeight_[0], rs_.handIKWeight_[1]);
    }
    else if (wc.leftIK.size() < 2 && wc.rightIK.size() < 2)
    {
        ImGui::SameLine();
        ui::TextDisabled("(the gesture has no leftHandIKCurve / rightHandIKCurve)");
    }
    ImGui::SameLine();
    ui::TextDisabled("1 / interpolationSpeed = %.2f s (approximation)", 1.0f / std::max(0.5f, wc.gestureInterp));
}

// comparison weapon, play / frame steps / loop / speed / onion skin
void ReloadPanel::DrawTransport()
{
    const float dpi = host_.Dpi();
    ImGui::SetNextItemWidth(260 * dpi);
    const bool cmpEnter = ui::InputTextWithHint("##cmpweapon", "compare with: .p3d (MLOD) or CfgWeapons class", cmpInput_, sizeof(cmpInput_),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ui::Button("Compare") || cmpEnter) view_.CompareLoad(cmpInput_);
    if (rs_.cmp_.model)
    {
        ImGui::SameLine();
        if (ui::Button("Clear##cmp"))
        {
            cmpInput_[0] = 0;
            view_.CompareLoad("");
        }
        ImGui::SameLine();
        ui::TextDisabled("%s below, reload %.2f s", rs_.cmp_.cls.c_str(), rs_.cmp_.duration);
    }
    if (ui::Button(rs_.playing ? "Pause##reload" : "Play##reload")) rs_.playing = !rs_.playing;
    ImGui::SameLine();
    const auto& g = rs_.wchar_.gesture;
    const float frameT = g && g->Frames() > 1 ? rs_.ReloadDuration() / float(g->Frames() - 1) : 1.0f / 30.0f;
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left))
    {
        rs_.time = std::max(0.0, rs_.time - frameT);
        rs_.playing = false;
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right))
    {
        rs_.time += frameT;
        rs_.playing = false;
    }
    ImGui::SameLine();
    ui::Checkbox("Loop##reload", &rs_.loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * dpi);
    ui::SliderFloat("speed##reload", &rs_.speed, 0.05f, 2.0f, "x %.2f");
    ImGui::SameLine();
    ui::Checkbox("Onion skin##reload", &opt_.reloadOnion);
    if (opt_.reloadOnion)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * dpi);
        if (ui::DragFloat("##odt", &opt_.reloadOnionDt, 0.005f, 0.02f, 1.0f, "+-%.2f s")) host_.PoseChanged();
    }
    ImGui::SameLine();
    ui::Text("t %.3f s   phase %.3f", rs_.time, rs_.ReloadPhase(rs_.time));
}

void ReloadPanel::SetWindow(int anim, float mn, float mx)
{
    rs_.SetWindow(anim, mn, mx);
    if (rs_.TakePoseChanged()) host_.PoseChanged();
}

// one time axis: ruler (seconds / source phase), gesture IK curves + switch phase, the windows of the reload source
// (edges drag), the comparison weapon's windows, the touch distance curve, the current time
void ReloadPanel::DrawTimeline()
{
    const float dpi = host_.Dpi();
    const float D = rs_.ReloadDuration(), k = std::max(0.01f, rs_.reloadK_);
    const float T = D / k;
    auto& anims = s_.cfgAnims;
    const auto& cmp = rs_.cmp_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float labelW = 190 * dpi, rowH = ImGui::GetFrameHeight() * 0.85f, rulerH = 26 * dpi;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = std::max(100.0f, ImGui::GetContentRegionAvail().x - labelW - 8 * dpi);
    const float x0 = p0.x + labelW;
    auto X = [&](double t) { return x0 + float(t / std::max(1e-4f, T)) * w; };
    auto TimeAt = [&](float x) { return std::clamp(double((x - x0) / w) * T, 0.0, double(T)); };
    const int rows = int(rs_.reloadAnims_.size()) + 2 + (cmp.model ? int(cmp.reloadAnims.size()) : 0);
    const float h = rulerH + rowH * rows;
    const ImU32 txt = ImGui::GetColorU32(ImGuiCol_Text), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x0 + w, p0.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg));
    // ruler: seconds on top, source phase below
    const float step = T > 6 ? 1.0f : T > 2 ? 0.5f : T > 0.8f ? 0.2f : 0.1f;
    for (float t = 0; t <= T + 1e-4f; t += step)
    {
        dl->AddLine(ImVec2(X(t), p0.y), ImVec2(X(t), p0.y + 6 * dpi), txt);
        dl->AddText(ImVec2(X(t) + 2, p0.y), dim, std::format("{:.1f}s", t).c_str());
    }
    for (int i = 0; i <= 10; ++i)
    {
        const double t = i / 10.0 * D / k;
        dl->AddLine(ImVec2(X(t), p0.y + rulerH - 6 * dpi), ImVec2(X(t), p0.y + rulerH), dim);
        if (i % 2 == 0) dl->AddText(ImVec2(X(t) + 2, p0.y + rulerH - 14 * dpi), dim, std::format("{:.1f}", i / 10.0).c_str());
    }
    dl->AddText(ImVec2(p0.x, p0.y), txt, "time / phase");
    ImGui::SetCursorScreenPos(ImVec2(x0, p0.y));
    ImGui::InvisibleButton("##ruler", ImVec2(w, rulerH));
    if (ImGui::IsItemActive())
    {
        rs_.time = TimeAt(ImGui::GetIO().MousePos.x);
        rs_.playing = false;
    }
    // row: gesture IK curves and the magazine switch phase
    float y = p0.y + rulerH;
    dl->AddText(ImVec2(p0.x, y), txt, "gesture IK / switch");
    auto curve = [&](const std::vector<float>& c, ImU32 col, float off) {
        for (size_t i = 0; i + 3 < c.size(); i += 2)
        {
            const float a = c[i], wa = c[i + 1], b = c[i + 2], wb = c[i + 3];
            const double ta = a * D, tb = b * D;
            dl->AddLine(ImVec2(X(ta), y + rowH - 3 - wa * (rowH - 6) + off), ImVec2(X(tb), y + rowH - 3 - wb * (rowH - 6) + off), col, 1.5f);
        }
    };
    curve(rs_.wchar_.leftIK, IM_COL32(90, 170, 255, 255), 0);
    curve(rs_.wchar_.rightIK, IM_COL32(255, 140, 90, 255), 1);
    if (s_.weapon.switchPhase >= 0)
    {
        const double ts = s_.weapon.switchPhase * D / k;
        dl->AddLine(ImVec2(X(ts), y), ImVec2(X(ts), y + rowH), IM_COL32(255, 220, 60, 255), 2.0f);
    }
    y += rowH;
    // rows: animations of the reload source, [minValue, maxValue] as time windows; edges drag
    int hovered = -1;
    for (int r = 0; r < int(rs_.reloadAnims_.size()); ++r)
    {
        const int ai = rs_.reloadAnims_[size_t(r)];
        CfgAnimation& a = anims[size_t(ai)];
        const bool edited = rs_.windowEdits_.count(ToLower(a.name)) > 0;
        dl->AddText(ImVec2(p0.x, y + 1), edited ? IM_COL32(255, 200, 80, 255) : txt, std::format("{} ({})", a.name, a.typeName).c_str());
        const double tA = a.minValue * D / k, tB = a.maxValue * D / k;
        const ImU32 fill = a.type == CfgAnimType::Hide ? IM_COL32(200, 90, 200, 150) : IM_COL32(70, 160, 230, 150);
        dl->AddRectFilled(ImVec2(X(tA), y + 2), ImVec2(std::max(X(tB), X(tA) + 2), y + rowH - 2), fill, 3.0f);
        for (int edge = 0; edge < 2; ++edge)
        {
            const float ex = X(edge ? tB : tA);
            ImGui::SetCursorScreenPos(ImVec2(ex - 4 * dpi, y));
            ImGui::PushID(r * 2 + edge);
            ImGui::InvisibleButton("##edge", ImVec2(8 * dpi, rowH));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0.0f))
            {
                const float ph = std::clamp(float(k * TimeAt(ImGui::GetIO().MousePos.x) / D), 0.0f, 1.0f);
                if (edge) SetWindow(ai, a.minValue, std::max(ph, a.minValue));
                else SetWindow(ai, std::min(ph, a.maxValue), a.maxValue);
                rs_.time = TimeAt(ImGui::GetIO().MousePos.x);
            }
            if (ImGui::IsItemHovered()) hovered = ai;
            ImGui::PopID();
            dl->AddLine(ImVec2(ex, y + 1), ImVec2(ex, y + rowH - 1), IM_COL32(255, 255, 255, 200), 2.0f);
        }
        y += rowH;
    }
    // the comparison weapon's windows on the same time axis (its own reload duration)
    if (cmp.model)
        for (int i : cmp.reloadAnims)
        {
            const CfgAnimation& a = cmp.anims[size_t(i)];
            dl->AddText(ImVec2(p0.x, y + 1), IM_COL32(150, 220, 150, 255), std::format("{}: {} ({})", cmp.cls, a.name, a.typeName).c_str());
            const double tA = a.minValue * cmp.duration / k, tB = a.maxValue * cmp.duration / k;
            dl->AddRectFilled(ImVec2(X(tA), y + 2), ImVec2(std::max(X(tB), X(tA) + 2), y + rowH - 2),
                              a.type == CfgAnimType::Hide ? IM_COL32(150, 110, 200, 120) : IM_COL32(90, 190, 120, 140), 3.0f);
            y += rowH;
        }
    // touch distance curve (row below)
    dl->AddText(ImVec2(p0.x, y), txt, "touch distance");
    const auto& td = rs_.touchDist_;
    if (!td.empty())
    {
        const float mx = std::max(*std::max_element(td.begin(), td.end()), 0.01f);
        for (size_t i = 0; i + 1 < td.size(); ++i)
        {
            const double ta = double(T) * i / (td.size() - 1), tb = double(T) * (i + 1) / (td.size() - 1);
            dl->AddLine(ImVec2(X(ta), y + rowH - 2 - td[i] / mx * (rowH - 4)), ImVec2(X(tb), y + rowH - 2 - td[i + 1] / mx * (rowH - 4)),
                        IM_COL32(255, 220, 60, 255), 1.5f);
        }
        const float thrY = y + rowH - 2 - std::min(1.0f, rs_.touchThresholdCm_ * 0.01f / mx) * (rowH - 4);
        dl->AddLine(ImVec2(x0, thrY), ImVec2(x0 + w, thrY), IM_COL32(255, 220, 60, 90));
        dl->AddCircleFilled(ImVec2(X(rs_.touchMinT_), y + rowH - 2 - rs_.touchMin_ / mx * (rowH - 4)), 3.5f * dpi, IM_COL32(255, 80, 60, 255));
    }
    y += rowH;
    // current time
    dl->AddLine(ImVec2(X(rs_.time), p0.y), ImVec2(X(rs_.time), y), IM_COL32(255, 70, 70, 255), 2.0f);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, y + 4 * dpi));
    if (hovered >= 0)
    {
        const CfgAnimation& a = anims[size_t(hovered)];
        ui::SetTooltip("%s: minValue %s, maxValue %s (drag the edges)\n%s", a.name.c_str(), ShortNum(a.minValue).c_str(), ShortNum(a.maxValue).c_str(),
                       s_.cfg.Where(a.file, a.line).c_str());
    }
}

// edited windows: diff + save (with .bak) or revert
void ReloadPanel::DrawSave()
{
    auto& edits = rs_.windowEdits_;
    if (!edits.empty())
    {
        ui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%d window(s) changed (viewer only until saved)", int(edits.size()));
        ImGui::SameLine();
        if (ui::Button("Diff..."))
        {
            view_.SaveWindows(false);
            showWindowSave_ = true;
        }
        ImGui::SameLine();
        if (ui::Button("Revert##windows"))
        {
            for (const auto& [name, e] : edits)
                for (auto& a : s_.cfgAnims)
                    if (IEquals(a.name, name))
                    {
                        a.minValue = e.min0;
                        a.maxValue = e.max0;
                    }
            edits.clear();
            host_.PoseChanged();
        }
    }
    if (showWindowSave_)
    {
        ImGui::OpenPopup("model.cfg windows");
        showWindowSave_ = false;
    }
    if (ImGui::BeginPopupModal("model.cfg windows", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        for (const auto& l : rs_.windowDiff_)
            ui::TextColored(l.starts_with("+") ? ImVec4(0.5f, 1, 0.5f, 1) : l.starts_with("-") ? ImVec4(1, 0.5f, 0.5f, 1) : ImVec4(0.8f, 0.8f, 0.8f, 1), "%s",
                            l.c_str());
        if (ui::Button("Save (.bak, then Validation)"))
        {
            view_.SaveWindows(true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// hand bone + weapon part: distance over the reload, a suggested window for an animation
void ReloadPanel::DrawTouchHelper()
{
    ui::TextUnformatted("Touch helper");
    if (!rs_.wchar_.model) return;
    const float dpi = host_.Dpi();
    const Skeleton& sk = rs_.wchar_.model->skeleton;
    int& bone = rs_.touchBone_;
    int& sel = rs_.touchSel_;
    ImGui::SetNextItemWidth(170 * dpi);
    if (ui::BeginCombo("hand bone", bone >= 0 && bone < int(sk.bones.size()) ? sk.bones[size_t(bone)].name.c_str() : "-"))
    {
        for (int b = 0; b < int(sk.bones.size()); ++b)
        {
            const std::string n = ToLower(sk.bones[size_t(b)].name);
            if (!n.starts_with("lefthand") && !n.starts_with("righthand")) continue;
            if (ui::SelectableRaw(sk.bones[size_t(b)].name.c_str(), b == bone))
            {
                bone = b;
                rs_.touchDist_.clear();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const P3DLod& l = s_.model.lods[size_t(s_.lod)];
    ImGui::SetNextItemWidth(170 * dpi);
    if (ui::BeginCombo("weapon part", sel >= 0 && sel < int(l.selections.size()) ? l.selections[size_t(sel)].name.c_str() : "-"))
    {
        for (int s = 0; s < int(l.selections.size()); ++s)
        {
            if (l.selections[size_t(s)].name.starts_with("proxy:") || !l.selections[size_t(s)].pointCount) continue;
            if (ui::SelectableRaw(l.selections[size_t(s)].name.c_str(), s == sel))
            {
                sel = s;
                rs_.touchDist_.clear();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ui::Button("Distance over time")) view_.ComputeTouch();
    if (!rs_.touchDist_.empty())
    {
        ImGui::SameLine();
        ui::Text("closest %.1f cm at %.2f s (phase %.3f)", rs_.touchMin_ * 100, rs_.touchMinT_, rs_.ReloadPhase(rs_.touchMinT_));
    }
    ImGui::SetNextItemWidth(120 * dpi);
    ui::DragFloat("threshold", &rs_.touchThresholdCm_, 0.1f, 0.2f, 30.0f, "%.1f cm");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200 * dpi);
    const auto& anims = s_.cfgAnims;
    int& ta = rs_.touchAnim_;
    if (ui::BeginCombo("window of", ta >= 0 && ta < int(anims.size()) ? anims[size_t(ta)].name.c_str() : "-"))
    {
        for (int i : rs_.reloadAnims_)
            if (ui::SelectableRaw(anims[size_t(i)].name.c_str(), i == ta)) ta = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ui::Button("Suggest window")) rs_.SuggestWindow();
}
