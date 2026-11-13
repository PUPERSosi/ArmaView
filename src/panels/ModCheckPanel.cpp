#include "ModCheckPanel.h"
#include "Lang.h"
#include "Log.h"

#include <imgui.h>

#include <windows.h>
#include <shellapi.h> // open the HTML report

#include <algorithm>
#include <chrono>
#include <format>

namespace fs = std::filesystem;

namespace {
const ImVec4 kErrorColor{ 1.0f, 0.4f, 0.4f, 1 }, kWarnColor{ 1.0f, 0.8f, 0.3f, 1 };
} // namespace

void ModCheckPanel::Start(const std::string& folder)
{
    if (job_.valid() || folder.empty()) return;
    strncpy_s(folder_, folder.c_str(), _TRUNCATE);
    prog_.done = 0;
    prog_.total = 0;
    prog_.cancel = false;
    const fs::path dir = fs::path(Utf8ToWide(folder));
    std::shared_ptr<const ArmaConfig> game = session_.game;
    const fs::path root = session_.projectRoot.empty() ? ArmaProjectRoot(dir) : session_.projectRoot;
    MountModPrefix(dir, root);
    job_ = std::async(std::launch::async, [this, dir, game, root] {
        ModReport r = RunModCheck(dir, root, game, prog_);
        FinishModReport(r);
        return r;
    });
    open = true;
    Log::Get().Info("Mod check started: {}", folder);
}

void ModCheckPanel::Poll()
{
    if (!job_.valid() || job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    report_ = job_.get();
    haveReport_ = true;
    const int newOnes = int(std::count_if(report_.issues.begin(), report_.issues.end(), [](const ModIssue& i) { return i.isNew; }));
    Log::Get().Info("Mod check {}: {} error(s), {} warning(s){} in {:.1f} s; report {}", PathToUtf8(report_.folder), report_.Count(ModIssue::Sev::Error),
                    report_.Count(ModIssue::Sev::Warning), report_.hadPrevious ? std::format(", {} new, {} fixed", newOnes, report_.fixed.size()) : "",
                    report_.seconds, PathToUtf8(report_.html));
}

void ModCheckPanel::Draw(float dpi)
{
    Poll();
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(980 * dpi, 620 * dpi), ImGuiCond_FirstUseEver);
    if (!ui::Begin("Mod check", &open))
    {
        ImGui::End();
        return;
    }
    ui::HelpMarker("Mod check: every file of a mod folder, read only.\n"
                   "1. Pick the mod folder (e.g. P:\\MyMod) and press Check: P3D, model.cfg, config.cpp, RVMAT and RTM files are read in the background.\n"
                   "2. P3D: textures / materials / proxies that do not exist, the rig against its model.cfg skeleton, model.cfg animations and axes.\n"
                   "3. config.cpp: parse errors, missing base classes, model / texture paths, CfgMoves states without RTM, links to missing states.\n"
                   "4. Problems are listed per file with open (the line in the editor); the HTML report is kept in %LOCALAPPDATA%\\ArmaView\\modcheck.\n"
                   "5. The next run of the same folder marks new problems and lists the fixed ones. Nothing is changed in the mod.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-330 * dpi);
    ui::InputTextWithHint("##modfolder", "mod folder (e.g. P:\\MyMod)", folder_, sizeof(folder_));
    ImGui::SameLine();
    if (ui::Button("...##modfolder"))
        if (std::wstring p = pickFolder_(L"Mod folder"); !p.empty()) strncpy_s(folder_, WideToUtf8(p).c_str(), _TRUNCATE);
    ImGui::SameLine();
    if (job_.valid())
    {
        if (ui::Button("Cancel##modcheck")) prog_.cancel = true;
        ImGui::SameLine();
        const int total = prog_.total, done = prog_.done;
        ImGui::ProgressBar(total ? float(done) / float(total) : 0.0f, ImVec2(-FLT_MIN, 0), std::format("{} / {}", done, total).c_str());
    }
    else
    {
        ImGui::BeginDisabled(!folder_[0]);
        if (ui::Button("Check##modcheck")) Start(folder_);
        ImGui::EndDisabled();
    }
    if (!haveReport_)
    {
        ui::EmptyState("Pick a mod folder and press Check. Nothing is written into the mod: the report goes to %LOCALAPPDATA%\\ArmaView\\modcheck.");
        ImGui::End();
        return;
    }
    const ModReport& r = report_;
    // summary
    std::string files;
    for (const auto& [k, n] : r.files) files += std::format("{}{} {}", files.empty() ? "" : ", ", k, n);
    const int newOnes = int(std::count_if(r.issues.begin(), r.issues.end(), [](const ModIssue& i) { return i.isNew; }));
    ui::TextDisabled("%s  |  %s  |  %.1f s%s", PathToUtf8(r.folder).c_str(), files.c_str(), r.seconds, r.cancelled ? T("  |  cancelled") : "");
    ImGui::TextColored(r.Count(ModIssue::Sev::Error) ? kErrorColor : ImVec4(0.45f, 0.85f, 0.45f, 1), "%s", TF("{} error(s)", r.Count(ModIssue::Sev::Error)).c_str());
    ImGui::SameLine();
    ImGui::TextColored(kWarnColor, "%s", TF("{} warning(s)", r.Count(ModIssue::Sev::Warning)).c_str());
    if (r.hadPrevious)
    {
        ImGui::SameLine();
        ui::TextDisabled("%s", TF("| against {}: {} new, {} fixed", r.previousWhen, newOnes, r.fixed.size()).c_str());
    }
    ImGui::SameLine();
    if (ui::SmallButton("Open HTML report")) ShellExecuteW(nullptr, L"open", r.html.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // filters
    ImGui::SetNextItemWidth(260 * dpi);
    ui::InputTextWithHint("##modfilter", "filter (file or text)", filter_, sizeof(filter_));
    ImGui::SameLine();
    ui::Checkbox("errors##mod", &showErr_);
    ImGui::SameLine();
    ui::Checkbox("warnings##mod", &showWarn_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!r.hadPrevious);
    ui::Checkbox("only new##mod", &onlyNew_);
    ImGui::EndDisabled();
    ImGui::SameLine();
    static const char* kKinds[] = { "all kinds", "p3d", "model.cfg", "config.cpp", "rvmat", "rtm" };
    ImGui::SetNextItemWidth(130 * dpi);
    ui::Combo("##modkind", &kind_, kKinds, 6);
    auto pass = [&](const ModIssue& i) {
        if (i.sev == ModIssue::Sev::Error ? !showErr_ : !showWarn_) return false;
        if (onlyNew_ && !i.isNew) return false;
        if (kind_ > 0 && i.kind != kKinds[kind_]) return false;
        if (!filter_[0]) return true;
        auto has = [&](const std::string& s) {
            const std::string a = ToLower(s), b = ToLower(filter_);
            return a.find(b) != std::string::npos;
        };
        return has(PathToUtf8(i.file)) || has(i.text);
    };
    if (ImGui::BeginChild("##modissues", ImVec2(0, 0), ImGuiChildFlags_Borders))
    {
        if (r.issues.empty()) ui::EmptyState("No problems found.");
        size_t i = 0;
        int shown = 0;
        while (i < r.issues.size())
        {
            size_t j = i;
            int nPass = 0;
            while (j < r.issues.size() && r.issues[j].file == r.issues[i].file) nPass += pass(r.issues[j++]);
            if (nPass)
            {
                ImGui::PushID(int(i));
                const bool expanded = ImGui::TreeNodeEx("##file", ImGuiTreeNodeFlags_DefaultOpen, "%s  (%d)", PathToUtf8(r.issues[i].file).c_str(), nPass);
                if (expanded)
                {
                    for (size_t k = i; k < j && shown < 3000; ++k)
                    {
                        const ModIssue& is = r.issues[k];
                        if (!pass(is)) continue;
                        ++shown;
                        ImGui::PushID(int(k));
                        if (ui::SmallButton("open##issue")) openInEditor_(is.file, std::max(1, is.line));
                        ImGui::SameLine();
                        ImGui::TextColored(is.sev == ModIssue::Sev::Error ? kErrorColor : kWarnColor, "%s%s", is.kind.c_str(), is.isNew ? T(" (new)") : "");
                        ImGui::SameLine();
                        if (is.line) { ImGui::TextDisabled("%d:", is.line); ImGui::SameLine(); }
                        ImGui::TextUnformatted(is.text.c_str());
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            i = j;
        }
        if (!r.fixed.empty() && ImGui::TreeNode(TF("Fixed since {} ({})###modfixed", r.previousWhen, r.fixed.size()).c_str()))
        {
            for (const auto& f : r.fixed)
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%s%s  %s", PathToUtf8(f.file.filename()).c_str(), f.line ? std::format("({})", f.line).c_str() : "", f.text.c_str());
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}
