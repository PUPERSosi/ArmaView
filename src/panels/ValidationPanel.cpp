#include "ValidationPanel.h"
#include "Lang.h"
#include "Text.h"
#include "Validation.h"

#include <imgui.h>

#include <algorithm>
#include <format>
#include <tuple>

namespace fs = std::filesystem;

namespace {
const ImVec4 kTextColor{ 0.9f, 0.9f, 0.9f, 1 }, kOkColor{ 0.45f, 0.85f, 0.45f, 1 }, kWarnColor{ 1.0f, 0.8f, 0.3f, 1 },
    kErrorColor{ 1.0f, 0.4f, 0.4f, 1 };
} // namespace

void ValidationPanel::Draw(const std::string& windowName, const std::function<void()>& movesSection)
{
    if (!ui::Begin(windowName.c_str()))
    {
        ImGui::End();
        return;
    }
    if (!session_.hasModel || !view_.LodReport().valid)
    {
        ui::TextWrapped(session_.hasModel ? "No skeleton for this model - nothing to validate (see Log)." : "No model loaded.");
    }
    else
    {
        const P3DLod& l = session_.model.lods[session_.lod];
        const ValidationReport& r = view_.LodReport();
        ui::Text("LOD %d [%s] vs %s, %zu points", session_.lod, l.Name().c_str(), session_.skeleton.name.c_str(), r.points);

        auto countLine = [&](const char* label, size_t n) {
            ui::TextColored(n ? kWarnColor : kOkColor, "%-34s %zu", T(label), n);
        };
        countLine("Weighted selections not in skeleton", r.weightedNotInSkeleton.size());
        countLine("Skeleton bones without selection", r.bonesWithoutSelection.size());
        if (r.discrete) ui::TextDisabled("isDiscrete=1: each point uses its deepest bone");
        else countLine("Points with > 4 bones", r.tooManyBones.size());
        countLine(r.discrete ? "Points in unrelated bones" : "Points with weight sum != 1 (0.01)", r.badWeightSum.size());
        countLine("Points without bone", r.noBone.size());

        auto nameList = [&](const char* title, const std::vector<std::string>& names, bool open) {
            if (!ui::CollapsingHeader(TF("{} ({})###{}", T(title), names.size(), title).c_str(), open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) return;
            ImGui::Indent();
            for (const auto& n : names) ui::TextUnformatted(n.c_str());
            ImGui::Unindent();
        };
        nameList("Weighted selections not in skeleton", r.weightedNotInSkeleton, true);
        nameList("Skeleton bones without selection", r.bonesWithoutSelection, false);
        nameList("Other selections not in skeleton", r.otherNotInSkeleton, false);

        auto weightsOf = [&](uint32_t p) {
            std::string s;
            for (size_t b = 0; b < host_.BoneSelection().size(); ++b)
                if (int si = host_.BoneSelection()[b]; si >= 0)
                    if (float w = l.selections[si].Weight(p); w > 0.0f) s += std::format("{}{} {:.3f}", s.empty() ? "" : ", ", session_.skeleton.bones[b].name, w);
            return s.empty() ? std::string("-") : s;
        };
        auto vertexSection = [&](const char* title, PointProblem kind, size_t count, auto getPoint) {
            if (!ui::CollapsingHeader(TF("{} ({})###{}", T(title), count, title).c_str())) return;
            ImGui::PushID(title);
            bool on = view_.HighlightedProblem() == kind;
            if (ui::Checkbox("Highlight on model (red)", &on)) view_.HighlightProblem(on ? kind : PointProblem::None);
            if (count && ImGui::BeginTable("v", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp,
                                           ImVec2(0, 220 * host_.Dpi())))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ui::TableSetupColumn("Point", ImGuiTableColumnFlags_WidthFixed);
                ui::TableSetupColumn("Bones", ImGuiTableColumnFlags_WidthFixed);
                ui::TableSetupColumn("Sum", ImGuiTableColumnFlags_WidthFixed);
                ui::TableSetupColumn("Weights");
                ImGui::TableHeadersRow();
                ImGuiListClipper clip;
                clip.Begin(int(count));
                while (clip.Step())
                    for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row)
                    {
                        auto [p, bones, sum] = getPoint(row);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ui::Text("%u", p);
                        ImGui::TableNextColumn();
                        ui::Text("%d", bones);
                        ImGui::TableNextColumn();
                        ui::Text("%.3f", sum);
                        ImGui::TableNextColumn();
                        ui::TextUnformatted(weightsOf(p).c_str());
                    }
                ImGui::EndTable();
            }
            ImGui::PopID();
        };
        if (!r.discrete)
        vertexSection("Points with > 4 bones", PointProblem::TooManyBones, r.tooManyBones.size(),
                      [&](int i) { return std::tuple(r.tooManyBones[i].point, r.tooManyBones[i].bones, r.tooManyBones[i].weightSum); });
        vertexSection(r.discrete ? "Points in unrelated bones" : "Points with weight sum != 1", PointProblem::BadWeightSum, r.badWeightSum.size(),
                      [&](int i) { return std::tuple(r.badWeightSum[i].point, r.badWeightSum[i].bones, r.badWeightSum[i].weightSum); });
        vertexSection("Points without bone", PointProblem::NoBone, r.noBone.size(),
                      [&](int i) { return std::tuple(r.noBone[i], 0, 0.0f); });
    }

    // model.cfg animations / config skins
    if (session_.hasModel && (!session_.cfgAnims.empty() || !session_.configSkins.empty()))
    {
        struct Item { std::string text; int bone; bool warning = false; fs::path file; int line = 0; };
        std::vector<Item> items;
        for (const auto& a : session_.cfgAnims)
        {
            AnimStatus st = AnimStatusOf(a, host_.BoneSelection(), session_.lod);
            if (st.severity != Severity::Error && st.severity != Severity::Warning) continue;
            fs::path f = session_.cfg.FileOf(a.file);
            items.push_back({ std::format("{}: {}", a.name, st.message), a.bone, st.severity == Severity::Warning, f, a.line });
        }
        for (const auto& s : session_.configSkins)
            for (const auto& sel : s.selections)
            {
                bool inSections = false;
                for (const auto& sec : session_.modelInfo.sections) inSections |= IEquals(sec, sel);
                if (!inSections) items.push_back({ std::format("{}::{}: hidden selection \"{}\" not in sections[] (no re-texture in game)", s.cfgRoot, s.className, sel), -1, false, s.file, s.line });
            }
        size_t warnings = 0;
        for (const auto& it : items) warnings += it.warning;
        const size_t errors = items.size() - warnings;
        ImGui::PushStyleColor(ImGuiCol_Text, errors ? kErrorColor : warnings ? kWarnColor : kOkColor);
        bool open = ui::CollapsingHeader(TF("model.cfg animations / skins: {} error(s), {} warning(s)###cfgprob", errors, warnings).c_str());
        ImGui::PopStyleColor();
        if (open)
            for (size_t i = 0; i < items.size(); ++i)
            {
                if (!items[i].file.empty())
                {
                    if (ui::SmallButton(TF("open##op{}", i).c_str())) host_.OpenInEditor(items[i].file, items[i].line);
                    if (ui::Hovered()) ui::SetTooltip("%s:%d (VS Code, path copied to the clipboard)", PathToUtf8(items[i].file).c_str(), items[i].line);
                    ImGui::SameLine();
                }
                ImGui::PushStyleColor(ImGuiCol_Text, items[i].warning ? kWarnColor : kErrorColor);
                if (ui::SelectableRaw(std::format("{}##cp{}", items[i].text, i).c_str()) && items[i].bone >= 0) host_.PickBone(items[i].bone, true);
                ImGui::PopStyleColor();
            }
    }

    ui::SeparatorText("RTM bones vs skeleton");
    ui::TextWrapped("Folder: %s", view_.RtmFolder().empty() ? "(none - open an .rtm or pick a folder)" : PathToUtf8(view_.RtmFolder()).c_str());
    if (ui::Button("Pick folder...")) view_.PickRtmFolder();
    ImGui::SameLine();
    if (ui::Button("Re-check") && !view_.RtmFolder().empty()) view_.RecheckRtm();
    if (int done = 0, total = 0; (view_.RtmCheckProgress(done, total), total > 0))
    {
        ImGui::SameLine();
        ImGui::ProgressBar(float(done) / float(total), ImVec2(160 * ImGui::GetFontSize() / 13.0f, 0), TF("checking {} / {}", done, total).c_str());
        ImGui::SameLine();
        if (ui::SmallButton("Cancel##rtmcheck")) view_.CancelRtmCheck();
        if (ui::Hovered()) ui::SetTooltip("Stops the check of the RTM folder; the files checked so far stay listed");
    }
    for (size_t i = 0; i < view_.RtmChecks().size(); ++i)
    {
        const RtmBoneCheck& c = view_.RtmChecks()[i];
        ImGui::PushID(int(i));
        std::string name = PathToUtf8(c.file.filename());
        if (!c.error.empty())
        {
            ui::TextColored(c.info.binarized ? kWarnColor : kErrorColor, "%s: %s", name.c_str(), c.error.c_str());
            ImGui::PopID();
            continue;
        }
        bool clean = c.missing.empty() && c.extra.empty();
        ImGui::PushStyleColor(ImGuiCol_Text, clean ? kOkColor : kWarnColor);
        bool open = ui::TreeNode("rtm", "%s  %u frames, %zu bones  missing %zu, extra %zu", name.c_str(), c.info.frames,
                                    c.info.bones.size(), c.missing.size(), c.extra.size());
        ImGui::PopStyleColor();
        if (open)
        {
            if (!c.missing.empty() && ui::TreeNode("missing", "Skeleton bones not in RTM (%zu)", c.missing.size()))
            {
                for (const auto& b : c.missing)
                {
                    int bi = session_.skeleton.Find(b);
                    IValidationView::BoneUsage u = view_.UsageOfBone(bi);
                    std::string label = b;
                    if (u.meshPoints) label += std::format("  [{} mesh vertices: stretch in game]", u.meshPoints);
                    for (const auto& px : u.proxies) label += std::format("  [proxy {} stays in bind pose]", px);
                    ImGui::PushStyleColor(ImGuiCol_Text, u.meshPoints ? kErrorColor : !u.proxies.empty() ? kWarnColor : kTextColor);
                    if (ui::SelectableRaw(std::format("{}##m{}", label, bi).c_str(), bi == host_.SelectedBone()) && bi >= 0) host_.PickBone(bi, true);
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
            if (!c.extra.empty() && ui::TreeNode("extra", "RTM bones not in skeleton (%zu) - ignored", c.extra.size()))
            {
                for (const auto& b : c.extra) ui::TextUnformatted(b.c_str());
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    // Loop seam of the primary animation layer
    if (int p = view_.PrimaryLayer(); p >= 0)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, view_.LoopSeam().empty() ? kTextColor : kWarnColor);
        bool open = ui::CollapsingHeader(TF("Loop seam {}: {}###seamval", session_.anim.layers[p].anim->Name(),
                                                        view_.LoopSeam().empty() ? std::string("ok") : std::format("{} bone(s) jump", view_.LoopSeam().size())).c_str());
        ImGui::PopStyleColor();
        if (open)
            for (const auto& j : view_.LoopSeam())
            {
                int bi = session_.skeleton.Find(j.bone);
                if (ui::SelectableRaw(std::format("{}: {:.3f} m, {:.1f} deg##seam{}", j.bone, j.distance, j.angle, j.bone).c_str(), bi == host_.SelectedBone()) && bi >= 0)
                    host_.PickBone(bi, true);
            }
    }

    // Crew preview: seat -> action -> state -> RTM problems
    {
        auto& seats = session_.attach.Seats();
        int bad = int(std::count_if(seats.begin(), seats.end(), [](const CrewSeat& s) { return !s.problem.empty() && !s.problemIsWarning; }));
        ImGui::PushStyleColor(ImGuiCol_Text, bad ? kErrorColor : kTextColor);
        bool open = ui::CollapsingHeader(TF("Crew (config): {} seat(s), {} problem(s)###crewval", seats.size(), bad).c_str());
        ImGui::PopStyleColor();
        if (open)
            for (int i = 0; i < int(seats.size()); ++i)
            {
                const CrewSeat& s = seats[i];
                if (s.problem.empty()) continue;
                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_Text, (s.problemIsWarning ? kWarnColor : kErrorColor));
                ui::TextWrapped("%s: %s", s.title.c_str(), s.problem.c_str());
                ImGui::PopStyleColor();
                if (!s.actionRef.file.empty())
                {
                    ImGui::SameLine();
                    if (ui::SmallButton("open")) host_.OpenInEditor(s.actionRef.file, s.actionRef.line);
                }
                ImGui::PopID();
            }
    }
    if (movesSection) movesSection(); // CfgMoves of the model's skeleton (the Moves panel draws it)
    ImGui::End();
}
