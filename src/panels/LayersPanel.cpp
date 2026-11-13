#include "LayersPanel.h"
#include "Lang.h"
#include "Text.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <format>

namespace fs = std::filesystem;

namespace {
const ImVec4 kWarnC = { 1.0f, 0.8f, 0.3f, 1 };
const ImVec4 kGoodC = { 0.45f, 0.85f, 0.45f, 1 };
} // namespace

void LayersPanel::Draw(const std::string& windowName, const std::string& dockWith)
{
    if (ImGuiWindow* tl = ImGui::FindWindowByName(dockWith.c_str()); tl && tl->DockId) ImGui::SetNextWindowDockID(tl->DockId, ImGuiCond_FirstUseEver);
    if (!ui::Begin(windowName.c_str()))
    {
        ImGui::End();
        return;
    }
    ui::HelpMarker("Animation layers: correct a clip without leaving the viewer.\n"
                   "1. Base clip: an RTM (or #retarget - the result of the Retarget panel).\n"
                   "2. + offset / + additive RTM / + replace RTM: layers over the base, each with bones, a mask and keys.\n"
                   "3. Clip: trim, speed, mirror, loop seam, root and foot locks.\n"
                   "4. Preview (bake) shows the result; Bake RTM_0101 writes a NEW file and re-reads it.\n"
                   "5. Save project keeps the layers to continue later; Blender (a3ob) opens the result there.");
    ImGui::SameLine();
    if (s_.skeleton.Empty())
    {
        ui::EmptyState("Open a character model with a skeleton (model.cfg skeletonName) - layers correct its RTM clips.");
        ImGui::End();
        return;
    }
    DrawProject();
    if (ui::CollapsingHeader("Clip: trim, speed, mirror, seam, root, foot locks")) DrawClipOps();
    DrawLayerList();
    if (selLayer >= 0 && selLayer < int(job_.proj.layers.size()) && !DrawSelectedLayer())
    {
        ImGui::End();
        return;
    }
    DrawActions();
    if (ui::CollapsingHeader("Batch: this project over a folder")) DrawBatch();
    if (job_.batch.running) DrawBatchProgress();
    if (ui::CollapsingHeader("Blender (a3ob)"))
    {
        const float dpi = host_.Dpi();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ui::InputTextWithHint("##bexe", "blender.exe", job_.blenderExe, sizeof(job_.blenderExe));
        ImGui::SetNextItemWidth(-220 * dpi);
        ui::InputTextWithHint("##bblend", ".blend with the armature", job_.blendFile, sizeof(job_.blendFile));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120 * dpi);
        ui::InputTextWithHint("##barm", "armature (empty: first)", job_.blendArmature, sizeof(job_.blendArmature));
        if (ui::Button("Open in Blender")) view_.LayersOpenInBlender();
        ImGui::SameLine();
        ui::TextDisabled("temp RTM -> a3ob import into the armature; back: Export RTM & Preview in Blender");
    }
    if (!job_.msg.empty()) ui::TextColored(job_.msg.find("DIFFERS") != std::string::npos ? ImVec4(1, 0.4f, 0.4f, 1) : kGoodC, "%s", job_.msg.c_str());
    ImGui::End();
}

// base clip, cycle length, project file
void LayersPanel::DrawProject()
{
    const float dpi = host_.Dpi();
    ImGui::SetNextItemWidth(-300 * dpi);
    ui::InputTextWithHint("##animsrc", "base clip .rtm (or #retarget = result of the Retarget panel)", job_.src, sizeof(job_.src));
    ImGui::SameLine();
    if (ui::SmallButton("#retarget")) strncpy_s(job_.src, "#retarget", _TRUNCATE);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * dpi);
    ui::DragFloat("cycle s", &job_.proj.duration, 0.02f, 0.05f, 60.0f, "%.2f");
    if (ui::SmallButton("Save project")) job_.SaveProject();
    ImGui::SameLine();
    if (ui::SmallButton("Load project")) job_.LoadProject();
    ImGui::SameLine();
    ui::TextDisabled("%s", PathToUtf8(job_.ProjectFile().filename()).c_str());
}

void LayersPanel::DrawClipOps()
{
    const float dpi = host_.Dpi();
    ClipOps& c = job_.proj.ops;
    ImGui::SetNextItemWidth(220 * dpi);
    ui::DragFloatRange2("trim", &c.trimStart, &c.trimEnd, 0.005f, 0.0f, 1.0f, "%.3f", "%.3f");
    if (ImGui::IsItemHovered()) ui::SetTooltip("Part of the clip to keep, from start to end (0..1 of its length).");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * dpi);
    ui::DragFloat("speed", &c.speed, 0.01f, 0.1f, 5.0f, "x %.2f");
    ui::Checkbox("mirror L/R", &c.mirror);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * dpi);
    ui::DragInt("loop seam frames", &c.seamFrames, 0.2f, 0, 30);
    ImGui::SameLine();
    ui::Checkbox("align root (pelvis X/Z)", &c.alignRoot);
    for (size_t i = 0; i < c.footLocks.size(); ++i)
    {
        ImGui::PushID(int(i));
        char fb[32];
        strncpy_s(fb, c.footLocks[i].foot.c_str(), _TRUNCATE);
        ImGui::SetNextItemWidth(100 * dpi);
        if (ui::InputText("##foot", fb, sizeof(fb))) c.footLocks[i].foot = fb;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180 * dpi);
        ui::DragFloatRange2("foot IK lock", &c.footLocks[i].from, &c.footLocks[i].to, 0.005f, 0.0f, 1.0f);
        if (ImGui::IsItemHovered()) ui::SetTooltip("Phase range (0..1) where this foot stays planted on the ground.");
        ImGui::SameLine();
        if (ui::SmallButton("x"))
        {
            c.footLocks.erase(c.footLocks.begin() + std::ptrdiff_t(i));
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ui::SmallButton("+ foot lock")) c.footLocks.push_back({});
}

void LayersPanel::DrawLayerList()
{
    auto& layers = job_.proj.layers;
    ui::SeparatorText(TF("Correction layers ({})", layers.size()).c_str());
    if (ui::SmallButton("+ offset"))
    {
        layers.push_back({});
        layers.back().name = "offset";
    }
    ImGui::SameLine();
    auto add = [&](CorrectionLayer::Type t, const char* name) {
        CorrectionLayer l;
        l.type = t;
        l.name = name;
        layers.push_back(l);
    };
    if (ui::SmallButton("+ additive RTM")) add(CorrectionLayer::Type::Additive, "additive");
    ImGui::SameLine();
    if (ui::SmallButton("+ replace RTM")) add(CorrectionLayer::Type::Replace, "replace");
    for (size_t i = 0; i < layers.size(); ++i)
    {
        CorrectionLayer& l = layers[i];
        ImGui::PushID(int(i));
        ui::Checkbox("##on", &l.enabled);
        ImGui::SameLine();
        const char* types[] = { "offset", "additive", "replace" };
        if (ui::SelectableRaw(std::format("{} [{}]  weight {:.2f}  mask {}", l.name, types[int(l.type)], l.weight,
                                          !l.armaMask.empty() ? l.armaMask : std::format("{} bone(s)", l.bones.size())).c_str(), selLayer == int(i)))
        {
            selLayer = int(i);
            view_.ShowMaskHeat(LayerMaskWeights(l, s_.skeleton, s_.masks));
        }
        ImGui::PopID();
    }
}

bool LayersPanel::DrawSelectedLayer()
{
    const float dpi = host_.Dpi();
    CorrectionLayer& l = job_.proj.layers[size_t(selLayer)];
    ui::SeparatorText(("Layer " + l.name).c_str());
    char nb[64];
    strncpy_s(nb, l.name.c_str(), _TRUNCATE);
    ImGui::SetNextItemWidth(160 * dpi);
    if (ui::InputText("name", nb, sizeof(nb))) l.name = nb;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * dpi);
    ui::SliderFloat("weight", &l.weight, 0.0f, 1.0f);
    ImGui::SameLine();
    if (ui::SmallButton("Delete layer"))
    {
        job_.proj.layers.erase(job_.proj.layers.begin() + selLayer);
        selLayer = -1;
        return false;
    }
    if (l.type != CorrectionLayer::Type::Offset)
    {
        char rb[400];
        strncpy_s(rb, l.rtm.c_str(), _TRUNCATE);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ui::InputTextWithHint("##rtm", "RTM of this layer", rb, sizeof(rb))) l.rtm = rb;
    }
    else
    {
        for (size_t k = 0; k < l.keys.size(); ++k)
        {
            ImGui::PushID(int(k));
            ImGui::SetNextItemWidth(60 * dpi);
            ui::DragFloat("##ph", &l.keys[k].phase, 0.005f, 0.0f, 1.0f, "%.2f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220 * dpi);
            ui::DragFloat3("rot deg", &l.keys[k].rotDeg.x, 0.2f, -180, 180, "%.1f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200 * dpi);
            ui::DragFloat3("pos m", &l.keys[k].pos.x, 0.002f, -1, 1, "%.3f");
            ImGui::SameLine();
            if (l.keys.size() > 1 && ui::SmallButton("x"))
            {
                l.keys.erase(l.keys.begin() + std::ptrdiff_t(k));
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (l.keys.size() < 5 && ui::SmallButton("+ key"))
        {
            LayerKey k = l.keys.back();
            k.phase = std::min(1.0f, k.phase + 0.25f);
            l.keys.push_back(k);
        }
        ImGui::SameLine();
        ui::TextDisabled("1 key = constant; 2-5 keys = smooth over the phase");
    }
    // mask
    ImGui::SetNextItemWidth(220 * dpi);
    if (ui::BeginCombo("mask", l.armaMask.empty() ? "own bones" : l.armaMask.c_str()))
    {
        if (ui::SelectableRaw("own bones", l.armaMask.empty())) l.armaMask.clear();
        for (const auto& [k, m] : s_.masks)
            if (ui::SelectableRaw(k.c_str(), IEquals(k, l.armaMask))) l.armaMask = k;
        ImGui::EndCombo();
    }
    if (l.armaMask.empty())
    {
        ImGui::SameLine();
        const int bone = host_.SelectedBone();
        if (ui::SmallButton("+ selected bone") && bone >= 0) l.bones.push_back({ s_.skeleton.bones[size_t(bone)].name, 1.0f });
        if (ui::Hovered()) ui::SetTooltip("Select a bone in the Outliner / scene first");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * dpi);
        ui::SliderFloat("fall-off to children", &l.falloff, 0.0f, 1.0f);
        for (size_t k = 0; k < l.bones.size(); ++k)
        {
            ImGui::PushID(int(100 + k));
            ImGui::SetNextItemWidth(100 * dpi);
            ui::SliderFloat(l.bones[k].first.c_str(), &l.bones[k].second, 0.0f, 1.0f);
            ImGui::SameLine();
            if (ui::SmallButton("x"))
            {
                l.bones.erase(l.bones.begin() + std::ptrdiff_t(k));
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }
    if (ui::SmallButton("Show mask on the mesh")) view_.ShowMaskHeat(LayerMaskWeights(l, s_.skeleton, s_.masks));
    // weight curve
    ui::TextDisabled("weight curve (phase, weight), empty = constant:");
    for (size_t k = 0; k < l.weightCurve.size(); ++k)
    {
        ImGui::PushID(int(200 + k));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * dpi);
        ui::DragFloat2("##wc", &l.weightCurve[k].first, 0.01f, 0.0f, 1.0f, "%.2f");
        ImGui::PopID();
    }
    ImGui::SameLine();
    if (l.weightCurve.size() < 5 && ui::SmallButton("+ point"))
        l.weightCurve.push_back({ l.weightCurve.empty() ? 0.0f : std::min(1.0f, l.weightCurve.back().first + 0.25f), 1.0f });
    ImGui::SameLine();
    if (!l.weightCurve.empty() && ui::SmallButton("clear")) l.weightCurve.clear();
    return true;
}

// preview, A/B, bake to a new file (overwrite only after asking, with a .bak)
void LayersPanel::DrawActions()
{
    ImGui::Separator();
    if (ui::Button("Preview (bake)")) view_.LayersPreview();
    ImGui::SameLine();
    if (ui::Checkbox("A/B with the base", &job_.previewAB) && job_.previewOn) view_.LayersPreview();
    ImGui::SameLine();
    ui::Checkbox("onion skin", &opt_.onionSkin);
    ImGui::SameLine();
    ImGui::BeginDisabled(!job_.previewOn);
    if (ui::Button("Stop preview")) view_.LayersStopPreview();
    ImGui::EndDisabled();
    if (!job_.out[0] && job_.src[0] && std::string(job_.src) != "#retarget")
    {
        const fs::path src = fs::path(Utf8ToWide(job_.src));
        strncpy_s(job_.out, PathToUtf8(src.parent_path() / (PathToUtf8(src.stem()) + "_edit.rtm")).c_str(), _TRUNCATE);
    }
    ImGui::SetNextItemWidth(-260 * host_.Dpi());
    ui::InputTextWithHint("##animout", "bake to .rtm (new file)", job_.out, sizeof(job_.out));
    ImGui::SameLine();
    if (ui::Button("Bake RTM_0101")) job_.Write(fs::path(Utf8ToWide(job_.out)), false);
    if (job_.overwriteAsk)
    {
        ui::TextColored(kWarnC, "%s exists.", job_.out);
        ImGui::SameLine();
        if (ui::SmallButton("Overwrite (keep .bak)"))
        {
            job_.overwriteAsk = false;
            job_.Write(fs::path(Utf8ToWide(job_.out)), true);
        }
        ImGui::SameLine();
        if (ui::SmallButton("Cancel")) job_.overwriteAsk = false;
    }
}

// the batch runs one file per frame (App steps it); the progress comes from the job
void LayersPanel::DrawBatch()
{
    const float dpi = host_.Dpi();
    const auto& b = job_.batch;
    ImGui::BeginDisabled(b.running);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ui::InputTextWithHint("##bdir", "folder with .rtm (source: #retarget = retarget each with the Retarget panel preset first)", job_.batchDir, sizeof(job_.batchDir));
    ImGui::SetNextItemWidth(-120 * dpi);
    ui::InputTextWithHint("##bout", "output folder (existing files are skipped)", job_.batchOut, sizeof(job_.batchOut));
    ImGui::SameLine();
    if (ui::Button("Run batch")) job_.BatchStart();
    ImGui::EndDisabled();
}

void LayersPanel::DrawBatchProgress()
{
    const float dpi = host_.Dpi();
    const auto& b = job_.batch;
    const float f = b.total > 0 ? float(b.index) / float(b.total) : 0.0f;
    ImGui::ProgressBar(f, ImVec2(-90 * dpi, 0), std::format("{} / {}  {}", b.index, b.total, b.current).c_str());
    ImGui::SameLine();
    if (ui::SmallButton("Cancel##batch")) job_.BatchCancel();
    ui::TextDisabled("%d written, %d skipped (exist), %d failed", b.written, b.skipped, b.failed);
}
