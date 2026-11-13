#include "StatesGraph.h"
#include "Lang.h"
#include "ArmaConfig.h" // kGameFileBase

#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

constexpr float kW = 300.0f, kPlateH = 48.0f, kCardH = 84.0f, kColW = 360.0f, kRowH = 64.0f;
constexpr int kColRows = 120; // a family column wraps after this many states

std::string Lower(std::string s)
{
    for (auto& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

// a == b where b is already lower case
bool IEqualsLower(const std::string& a, const std::string& lowerB)
{
    if (a.size() != lowerB.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (char(std::tolower(uint8_t(a[i]))) != lowerB[i]) return false;
    return true;
}

bool ContainsNoCase(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                       [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); }) != hay.end();
}

// "AmovPercMstpSrasWrflDnon" -> family "AmovPerc", sub key "Wrfl Mstp"; transitions "X_Y" -> family of X;
// other names: the part before the first '_' / digit ("Acts", "CutSceneAnimationBase", "Bipod")
void Family(const std::string& name, std::string& family, std::string& sub)
{
    std::string head = name.substr(0, name.find('_'));
    auto isCode = [](const std::string& s) {
        return s.size() >= 24 && (s[0] == 'A' || s[0] == 'a') && (s[4] == 'P' || s[4] == 'p') && (s[8] == 'M' || s[8] == 'm') &&
               (s[12] == 'S' || s[12] == 's') && (s[16] == 'W' || s[16] == 'w') && (s[20] == 'D' || s[20] == 'd');
    };
    if (isCode(head))
    {
        family = head.substr(0, 8);
        sub = head.substr(16, 4) + head.substr(8, 4) + (name.size() > head.size() ? "~" : "");
        return;
    }
    size_t k = 0;
    while (k < head.size() && !std::isdigit(uint8_t(head[k]))) ++k;
    family = head.substr(0, std::max<size_t>(k, 1));
    sub = name;
}

} // namespace

StatesGraph::~StatesGraph()
{
    cancel_ = true;
    if (layoutJob_.valid()) layoutJob_.wait();
}

void StatesGraph::SetGraph(const MovesGraph& g, const fs::path& positionsFile)
{
    // keep the positions of known names (hot reload, same config again), else read the saved ones
    std::unordered_map<std::string, ImVec2> keep;
    for (size_t i = 0; i < nodes_.size() && i < names_.size(); ++i)
        if (nodes_[i].placed) keep[Lower(names_[i])] = nodes_[i].pos;
    if (keep.empty() || positionsFile != file_)
    {
        keep.clear();
        std::ifstream f(positionsFile);
        for (std::string line; std::getline(f, line);)
        {
            const size_t a = line.find('\t'), b = line.find('\t', a + 1);
            if (a == std::string::npos || b == std::string::npos) continue;
            keep[Lower(line.substr(0, a))] = ImVec2(std::stof(line.substr(a + 1, b - a - 1)), std::stof(line.substr(b + 1)));
        }
        fitPending_ = true;
    }
    file_ = positionsFile;
    cancel_ = true;
    if (layoutJob_.valid()) layoutJob_.wait();
    cancel_ = false;
    names_.clear();
    nodes_.assign(g.states.size(), {});
    needLayout_.assign(g.states.size(), 0);
    int missing = 0;
    for (size_t i = 0; i < g.states.size(); ++i)
    {
        names_.push_back(g.states[i].name);
        if (auto it = keep.find(Lower(g.states[i].name)); it != keep.end()) nodes_[i] = { it->second, true };
        else { needLayout_[i] = 1; ++missing; }
    }
    if (selected_ >= int(nodes_.size())) selected_ = -1;
    lastSearch_ = "\x01"; // re-run the search
    if (missing) StartLayout(g);
}

void StatesGraph::StartLayout(const MovesGraph& g)
{
    // inputs copied for the thread
    struct In { std::vector<std::string> names; std::vector<std::vector<int>> adj; std::vector<uint8_t> need; std::vector<ImVec2> fixed; };
    auto in = std::make_shared<In>();
    in->names = names_;
    in->adj.resize(g.states.size());
    for (size_t i = 0; i < g.states.size(); ++i)
        for (const auto& e : g.states[i].edges)
            if (e.to != int(i))
            {
                in->adj[i].push_back(e.to);
                in->adj[size_t(e.to)].push_back(int(i));
            }
    in->need = needLayout_;
    for (const auto& n : nodes_) in->fixed.push_back(n.pos);
    std::atomic<bool>* cancel = &cancel_;
    layoutJob_ = std::async(std::launch::async, [in, cancel]() -> std::vector<ImVec2> {
        const size_t n = in->names.size();
        std::vector<ImVec2> pos = in->fixed;
        const bool full = std::all_of(in->need.begin(), in->need.end(), [](uint8_t x) { return x != 0; });
        if (!full)
        {
            // incremental: a new state goes next to a placed neighbour (right of it), else below everything
            float bottom = 0;
            for (size_t i = 0; i < n; ++i)
                if (!in->need[i]) bottom = std::max(bottom, pos[i].y);
            float freeX = 0;
            for (size_t i = 0; i < n; ++i)
            {
                if (!in->need[i]) continue;
                int anchor = -1;
                for (int j : in->adj[i])
                    if (!in->need[size_t(j)]) { anchor = j; break; }
                if (anchor >= 0) pos[i] = ImVec2(pos[size_t(anchor)].x + kColW, pos[size_t(anchor)].y + kRowH * 0.5f);
                else { pos[i] = ImVec2(freeX, bottom + 3 * kRowH); freeX += kColW; }
            }
            return pos;
        }
        // full layout: families -> columns (wrapped), ordered by sub key, then barycentric sweeps
        std::map<std::string, std::vector<int>> fam;
        std::vector<std::string> sub(n);
        for (size_t i = 0; i < n; ++i)
        {
            std::string f;
            Family(in->names[i], f, sub[i]);
            fam[f].push_back(int(i));
        }
        std::vector<std::pair<std::string, std::vector<int>>> fams(fam.begin(), fam.end());
        std::sort(fams.begin(), fams.end(), [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
        std::vector<std::vector<int>> cols;
        for (auto& [f, members] : fams)
        {
            std::sort(members.begin(), members.end(), [&](int a, int b) { return sub[size_t(a)] != sub[size_t(b)] ? sub[size_t(a)] < sub[size_t(b)] : in->names[size_t(a)] < in->names[size_t(b)]; });
            for (size_t k = 0; k < members.size(); k += kColRows)
                cols.emplace_back(members.begin() + std::ptrdiff_t(k), members.begin() + std::ptrdiff_t(std::min(members.size(), k + kColRows)));
        }
        std::vector<int> colOf(n), rowOf(n);
        auto place = [&] {
            for (size_t c = 0; c < cols.size(); ++c)
                for (size_t r = 0; r < cols[c].size(); ++r)
                {
                    colOf[size_t(cols[c][r])] = int(c);
                    rowOf[size_t(cols[c][r])] = int(r);
                }
        };
        place();
        for (int sweep = 0; sweep < 8 && !*cancel; ++sweep)
        {
            for (auto& col : cols)
            {
                std::vector<std::pair<float, int>> key;
                for (size_t r = 0; r < col.size(); ++r)
                {
                    const int v = col[r];
                    float sum = 0;
                    int cnt = 0;
                    for (int u : in->adj[size_t(v)])
                        if (colOf[size_t(u)] != colOf[size_t(v)]) { sum += float(rowOf[size_t(u)]); ++cnt; }
                    // keep the family order mostly: neighbour mean blended with the current row
                    key.push_back({ cnt ? 0.6f * sum / cnt + 0.4f * float(r) : float(r), v });
                }
                std::stable_sort(key.begin(), key.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                for (size_t r = 0; r < col.size(); ++r) col[r] = key[r].second;
            }
            place();
        }
        // pack the columns into shelves (a roughly 1.3 : 1 rectangle instead of one very long strip)
        double area = 0;
        for (const auto& col : cols) area += double(kColW) * double(col.size()) * kRowH;
        const float shelfW = float(std::sqrt(area * 1.3));
        std::vector<ImVec2> colPos(cols.size());
        float x = 0, y = 0, shelfH = 0;
        for (size_t c = 0; c < cols.size(); ++c)
        {
            const float h = float(cols[c].size()) * kRowH;
            if (x > 0 && x + kColW > shelfW) { x = 0; y += shelfH + 2 * kRowH; shelfH = 0; }
            colPos[c] = ImVec2(x, y);
            x += kColW;
            shelfH = std::max(shelfH, h);
        }
        for (size_t i = 0; i < n; ++i)
        {
            const ImVec2 cp = colPos[size_t(colOf[i])];
            pos[i] = ImVec2(cp.x, cp.y + float(rowOf[i]) * kRowH);
        }
        return pos;
    });
}

void StatesGraph::PollLayout()
{
    if (!layoutJob_.valid() || layoutJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto pos = layoutJob_.get();
    if (pos.size() != nodes_.size()) return;
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (needLayout_[i]) { nodes_[i] = { pos[i], true }; needLayout_[i] = 0; }
    Save();
}

void StatesGraph::Save() const
{
    if (file_.empty()) return;
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    std::ofstream f(file_, std::ios::trunc);
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].placed) f << names_[i] << '\t' << nodes_[i].pos.x << '\t' << nodes_[i].pos.y << '\n';
}

void StatesGraph::FocusOn(int state, bool zoomIn)
{
    if (state < 0 || state >= int(nodes_.size())) return;
    selected_ = state;
    if (zoomIn) zoom_ = std::max(zoom_, 0.8f);
    focusTarget_ = state;
}

std::vector<std::pair<std::string, int>> StatesGraph::ActionTargets(const MovesGraph& g, const std::string& a) const
{
    std::vector<std::pair<std::string, int>> out;
    for (const auto& [cls, m] : g.actions)
    {
        if (!actionsClass.empty() && cls != actionsClass) continue;
        if (auto it = m.find(a); it != m.end() && !it->second.empty()) out.push_back({ cls, g.Find(it->second) });
    }
    return out;
}

void StatesGraph::BuildMatrix(const MovesGraph& g)
{
    const size_t key = g.states.size() * 1000003u + g.actions.size();
    if (matrixFor_ == &g && matrixKey_ == key) return;
    matrixFor_ = &g;
    matrixKey_ = key;
    mClasses_.clear();
    std::set<std::string> seen, acts;
    for (const auto& c : g.actionsOrder)
        if (g.actions.count(c) && seen.insert(c).second) mClasses_.push_back(c);
    for (const auto& [c, m] : g.actions)
    {
        if (seen.insert(c).second) mClasses_.push_back(c);
        for (const auto& [a, s] : m) acts.insert(a);
    }
    mActions_.assign(acts.begin(), acts.end());
    mUsed_.clear();
    for (const auto& st : g.states) ++mUsed_[Lower(st.actions)];
}

// toolbar row 2: view, actions class, action
void StatesGraph::DrawActionsBar(const MovesGraph& g, float dpi)
{
    BuildMatrix(g);
    ui::RadioButton("States##graphview", &view, 0);
    ImGui::SameLine();
    ui::RadioButton("Actions##graphview", &view, 1);
    if (ui::Hovered()) ui::SetTooltip("Actions: the matrix of actions classes x actions (red: leads to a state that does not exist)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(230 * dpi);
    if (ImGui::BeginCombo("##actclass", actionsClass.empty() ? T("actions class: all states") : actionsClass.c_str(), ImGuiComboFlags_HeightLarge))
    {
        if (ImGui::Selectable(T("(all states)"), actionsClass.empty())) actionsClass.clear();
        for (const auto& c : mClasses_)
        {
            const auto u = mUsed_.find(c);
            if (ImGui::Selectable(std::format("{}  ({})", c, u == mUsed_.end() ? 0 : u->second).c_str(), c == actionsClass)) actionsClass = c;
        }
        ImGui::EndCombo();
    }
    ui::TrackId("##actclass");
    if (ImGui::IsItemHovered()) ui::SetTooltip("Shows only the states that use this actions class (actions = ...); the Actions view lists its action -> state table.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200 * dpi);
    auto written = [&](const std::string& a) { auto it = g.actionNames.find(a); return it == g.actionNames.end() ? a : it->second; };
    if (ImGui::BeginCombo("##actname", action.empty() ? T("action: none") : written(action).c_str(), ImGuiComboFlags_HeightLarge))
    {
        if (ImGui::Selectable(T("(none)"), action.empty())) action.clear();
        for (const auto& a : mActions_)
            if (ImGui::Selectable(written(a).c_str(), a == action)) action = a;
        ImGui::EndCombo();
    }
    ui::TrackId("##actname");
    if (ImGui::IsItemHovered()) ui::SetTooltip("Highlights every state this action leads to (cyan), in the chosen actions class or in all of them.");
    if (!action.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 1.0f, 1.0f), "%s", std::format("{} {}", actTargets_, T("target state(s)")).c_str());
    }
    // highlight cache
    if (action != lastActionHl_ || actionsClass != lastHlClass_ || actTarget_.size() != g.states.size())
    {
        lastActionHl_ = action;
        lastHlClass_ = actionsClass;
        actTarget_.assign(g.states.size(), 0);
        actTargets_ = 0;
        if (!action.empty())
            for (const auto& [cls, k] : ActionTargets(g, action))
                if (k >= 0 && !actTarget_[size_t(k)]) { actTarget_[size_t(k)] = 1; ++actTargets_; }
    }
}

// action -> state table of the chosen actions class, with "show"
void StatesGraph::DrawClassTable(const MovesGraph& g, Actions& act, float dpi, float height)
{
    auto it = g.actions.find(actionsClass);
    if (it == g.actions.end()) return;
    ImGui::TextDisabled("%s", std::format("{} >> {}: {} {}", "Actions", actionsClass, it->second.size(), T("action(s)")).c_str());
    if (ImGui::BeginTable("##classactions", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, height)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ui::TableSetupColumn("Action");
        ui::TableSetupColumn("State");
        ui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60 * dpi);
        ImGui::TableHeadersRow();
        int row = 0;
        for (const auto& [a, s] : it->second)
        {
            ImGui::PushID(row++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            auto w = g.actionNames.find(a);
            ImGui::TextUnformatted(w == g.actionNames.end() ? a.c_str() : w->second.c_str());
            ImGui::TableNextColumn();
            const int k = s.empty() ? -1 : g.Find(s);
            if (s.empty()) ImGui::TextDisabled("(no action)");
            else if (k < 0) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", std::format("{} ({})", s, T("no such state")).c_str());
            else ImGui::TextUnformatted(s.c_str());
            ImGui::TableNextColumn();
            if (k >= 0 && ui::SmallButton("show"))
            {
                view = 0;
                FocusOn(k);
                act.select = k;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// Actions view: the class table (if a class is chosen) and the matrix classes x actions
void StatesGraph::DrawActionsView(const MovesGraph& g, Actions& act, float dpi)
{
    const float avail = ImGui::GetContentRegionAvail().y;
    if (!actionsClass.empty()) DrawClassTable(g, act, dpi, std::max(120.0f, avail * 0.35f));
    ImGui::SetNextItemWidth(200 * dpi);
    ui::InputTextWithHint("##matrixfilter", "filter actions", matrixFilter, sizeof(matrixFilter));
    std::vector<int> cols;
    for (int i = 0; i < int(mActions_.size()); ++i)
        if (ContainsNoCase(mActions_[size_t(i)], matrixFilter)) cols.push_back(i);
    const int maxCols = 400; // ImGui tables: 512 columns at most
    ImGui::SameLine();
    int bad = 0;
    for (const auto& [c, m] : g.actions)
        for (const auto& [a, s] : m) bad += !s.empty() && g.Find(s) < 0;
    ImGui::TextDisabled("%s", std::format("{} {} x {} {}{}", mClasses_.size(), T("classes"), cols.size(), T("actions"),
                                          cols.size() > size_t(maxCols) ? std::format(" ({} {})", T("first"), maxCols) : "").c_str());
    if (bad)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", std::format("{} {}", bad, T("lead to states that do not exist")).c_str());
    }
    if (cols.size() > size_t(maxCols)) cols.resize(size_t(maxCols));
    if (cols.empty() || mClasses_.empty()) { ImGui::TextDisabled("No actions"); return; }
    const ImGuiTableFlags fl = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##actmatrix", int(cols.size()) + 1, fl, ImVec2(0, 0)))
    {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("##class", ImGuiTableColumnFlags_WidthFixed, 190 * dpi);
        for (int c : cols)
        {
            auto w = g.actionNames.find(mActions_[size_t(c)]);
            ImGui::TableSetupColumn(w == g.actionNames.end() ? mActions_[size_t(c)].c_str() : w->second.c_str(), ImGuiTableColumnFlags_WidthFixed, 130 * dpi);
        }
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(int(mClasses_.size()));
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r)
            {
                const std::string& cls = mClasses_[size_t(r)];
                const auto& m = g.actions.at(cls);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const bool chosen = cls == actionsClass;
                if (ImGui::Selectable(std::format("{}##row{}", cls, r).c_str(), chosen)) actionsClass = chosen ? std::string() : cls;
                if (ImGui::IsItemHovered()) ui::SetTooltip("Click: filter the graph to the states of this actions class (and list its actions above)");
                for (size_t ci = 0; ci < cols.size(); ++ci)
                {
                    if (!ImGui::TableSetColumnIndex(int(ci) + 1)) continue; // scrolled out
                    auto it = m.find(mActions_[size_t(cols[ci])]);
                    if (it == m.end()) continue;
                    if (it->second.empty()) { ImGui::TextDisabled("\"\""); continue; }
                    const int k = g.Find(it->second);
                    ImGui::PushID(r * 4096 + int(ci));
                    if (k < 0) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                    const bool hl = mActions_[size_t(cols[ci])] == action;
                    if (ImGui::Selectable(it->second.c_str(), hl) && k >= 0)
                    {
                        view = 0;
                        FocusOn(k);
                        act.select = k;
                    }
                    if (k < 0) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                        ui::SetTooltip("%s", std::format("{} >> {} = {}{}", cls, mActions_[size_t(cols[ci])], it->second,
                                                         k < 0 ? std::string("\n") + T("no such state") : std::string("\n") + T("click: show it in the graph")).c_str());
                    ImGui::PopID();
                }
            }
        ImGui::EndTable();
    }
}

StatesGraph::Actions StatesGraph::Draw(const MovesGraph& g, int current, const std::vector<int>& path, float dpi)
{
    const auto t0 = std::chrono::steady_clock::now();
    Actions act;
    PollLayout();
    if (nodes_.size() != g.states.size()) { ImGui::TextDisabled("graph not built"); return act; }
    DrawActionsBar(g, dpi);
    if (view == 1)
    {
        DrawActionsView(g, act, dpi);
        return act;
    }

    // ---- toolbar: search + filters
    ImGui::SetNextItemWidth(220 * dpi);
    const bool enter = ImGui::InputTextWithHint("##statesearch", "search (Enter: next match)", search, sizeof(search), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemHovered()) ui::SetTooltip("Finds states by name; Enter jumps to the next match.");
    ImGui::SameLine();
    ImGui::Checkbox("RTM", &filterRtm);
    if (ImGui::IsItemHovered()) ui::SetTooltip("search also in the RTM path");
    ImGui::SameLine();
    ImGui::Checkbox("actions", &filterAction);
    if (ImGui::IsItemHovered()) ui::SetTooltip("search also in the actions class / the action names leading to the state");
    ImGui::SameLine();
    const char* src[] = { "all", "my mod", "BI" };
    ImGui::SetNextItemWidth(90 * dpi);
    ImGui::Combo("##src", &filterSource, src, 3);
    if (ImGui::IsItemHovered()) ui::SetTooltip("Which states to show: all, only those of your mod's config, or only BI's.");
    if (search != lastSearch_ || filterSource != lastFilter_ || filterRtm != lastRtm_ || filterAction != lastAct_ || actionsClass != lastClass_ ||
        match_.size() != g.states.size())
    {
        lastClass_ = actionsClass;
        lastSearch_ = search;
        lastFilter_ = filterSource;
        lastRtm_ = filterRtm;
        lastAct_ = filterAction;
        match_.assign(g.states.size(), 1);
        // action -> states
        std::vector<std::string> actionText(g.states.size());
        if (filterAction && search[0])
            for (const auto& [cls, m] : g.actions)
                for (const auto& [a, s] : m)
                    if (int k = g.Find(s); k >= 0) actionText[size_t(k)] += a + " ";
        matches_ = 0;
        for (size_t i = 0; i < g.states.size(); ++i)
        {
            const MoveState& s = g.states[i];
            const bool mine = s.cfgFile < kGameFileBase;
            bool ok = (filterSource == 0 || (filterSource == 1) == mine) && (actionsClass.empty() || IEqualsLower(s.actions, actionsClass));
            if (ok && search[0])
                ok = ContainsNoCase(s.name, search) || (filterRtm && ContainsNoCase(s.file, search)) ||
                     (filterAction && (ContainsNoCase(s.actions, search) || ContainsNoCase(actionText[i], search)));
            match_[i] = ok;
            matches_ += ok;
        }
        matchCursor_ = 0;
    }
    const bool filtering = search[0] || filterSource || !actionsClass.empty();
    ImGui::SameLine();
    ImGui::TextDisabled(filtering ? "%d match(es)" : "%zu states", filtering ? matches_ : g.states.size());
    if (enter && filtering && matches_)
    {
        for (size_t k = 0; k < g.states.size(); ++k)
        {
            const size_t i = (size_t(matchCursor_) + k) % g.states.size();
            if (match_[i]) { FocusOn(int(i)); matchCursor_ = int(i) + 1; break; }
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit")) fitPending_ = true;
    if (ImGui::IsItemHovered()) ui::SetTooltip("Zooms the graph so that every state is visible.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Current") && current >= 0) FocusOn(current);
    if (ImGui::IsItemHovered()) ui::SetTooltip("Centers the graph on the state the character is in now.");
    ImGui::SameLine();
    const bool relayoutStates = ImGui::SmallButton("Re-layout");
    if (ImGui::IsItemHovered()) ui::SetTooltip("Arranges the states again from scratch.");
    if (relayoutStates)
    {
        std::fill(needLayout_.begin(), needLayout_.end(), uint8_t(1));
        for (auto& nd : nodes_) nd.placed = false;
        StartLayout(g);
        fitPending_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(layoutJob_.valid() ? "| layout..." : "| wheel zoom, RMB/MMB drag pan, LMB drag node, dbl-click playMove, Alt+drag link");

    // ---- canvas
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 50.0f);
    size.y = std::max(size.y, 50.0f);
    ImGui::InvisibleButton("##statescanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(c0, ImVec2(c0.x + size.x, c0.y + size.y), true);
    dl->AddRectFilled(c0, ImVec2(c0.x + size.x, c0.y + size.y), IM_COL32(24, 26, 30, 255));
    ImGuiIO& io = ImGui::GetIO();

    bool anyPlaced = false;
    ImVec2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
    for (const auto& n : nodes_)
        if (n.placed)
        {
            anyPlaced = true;
            mn = ImVec2(std::min(mn.x, n.pos.x), std::min(mn.y, n.pos.y));
            mx = ImVec2(std::max(mx.x, n.pos.x + kW), std::max(mx.y, n.pos.y + kPlateH));
        }
    if (fitPending_ && anyPlaced && !layoutJob_.valid())
    {
        zoom_ = std::clamp(std::min(size.x / (mx.x - mn.x + 200), size.y / (mx.y - mn.y + 200)), 0.01f, 2.0f);
        pan_ = ImVec2(-mn.x + 100 + (size.x / zoom_ - (mx.x - mn.x + 200)) * 0.5f, -mn.y + 100);
        fitPending_ = false;
    }
    if (focusTarget_ >= 0 && focusTarget_ < int(nodes_.size()) && nodes_[size_t(focusTarget_)].placed)
    {
        const ImVec2 p = nodes_[size_t(focusTarget_)].pos;
        pan_ = ImVec2(size.x * 0.5f / zoom_ - p.x - kW * 0.5f, size.y * 0.5f / zoom_ - p.y);
        focusTarget_ = -1;
    }
    auto toScreen = [&](ImVec2 w) { return ImVec2(c0.x + (w.x + pan_.x) * zoom_, c0.y + (w.y + pan_.y) * zoom_); };
    auto toWorld = [&](ImVec2 s) { return ImVec2((s.x - c0.x) / zoom_ - pan_.x, (s.y - c0.y) / zoom_ - pan_.y); };

    // pan / zoom
    if (hovered && io.MouseWheel != 0)
    {
        const ImVec2 before = toWorld(io.MousePos);
        zoom_ = std::clamp(zoom_ * std::pow(1.15f, io.MouseWheel), 0.01f, 3.0f);
        const ImVec2 after = toWorld(io.MousePos);
        pan_.x += after.x - before.x;
        pan_.y += after.y - before.y;
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
    {
        pan_.x += io.MouseDelta.x / zoom_;
        pan_.y += io.MouseDelta.y / zoom_;
    }

    // level of detail
    const bool cards = zoom_ >= 0.75f;
    const float nodeH = cards ? kCardH : kPlateH;
    const bool names = zoom_ * kPlateH >= 7.0f;
    const float nameSize = std::min(ImGui::GetFontSize(), zoom_ * kPlateH * 0.8f); // small plates: smaller text
    const ImVec2 vmin = toWorld(c0), vmax = toWorld(ImVec2(c0.x + size.x, c0.y + size.y));
    std::vector<uint8_t> vis(nodes_.size(), 0);
    visibleNodes = 0;
    for (size_t i = 0; i < nodes_.size(); ++i)
    {
        const ImVec2 p = nodes_[i].pos;
        if (nodes_[i].placed && p.x + kW >= vmin.x && p.x <= vmax.x && p.y + nodeH >= vmin.y && p.y <= vmax.y) { vis[i] = 1; ++visibleNodes; }
    }
    // hovered node
    int hover = -1;
    if (hovered)
    {
        const ImVec2 w = toWorld(io.MousePos);
        for (size_t i = 0; i < nodes_.size() && hover < 0; ++i)
            if (vis[i] && w.x >= nodes_[i].pos.x && w.x <= nodes_[i].pos.x + kW && w.y >= nodes_[i].pos.y && w.y <= nodes_[i].pos.y + nodeH) hover = int(i);
    }
    std::set<std::pair<int, int>> pathEdges;
    std::vector<uint8_t> onPath(nodes_.size(), 0);
    for (size_t k = 0; k < path.size(); ++k)
    {
        if (path[k] >= 0 && path[k] < int(onPath.size())) onPath[size_t(path[k])] = 1;
        if (k) pathEdges.insert({ path[k - 1], path[k] });
    }

    // edges: all of the visible nodes when few are visible, else only around the hovered / selected node and the path
    drawnEdges = 0;
    const bool allEdges = visibleNodes <= 400;
    auto edgeColor = [](bool interp, bool path) { return path ? IM_COL32(255, 80, 255, 255) : interp ? IM_COL32(110, 160, 255, 110) : IM_COL32(170, 170, 180, 90); };
    for (size_t i = 0; i < nodes_.size() && drawnEdges < 40000; ++i)
    {
        if (!nodes_[i].placed) continue;
        for (const auto& e : g.states[i].edges)
        {
            const size_t j = size_t(e.to);
            if (j == i || !nodes_[j].placed) continue;
            const bool onP = pathEdges.count({ int(i), int(j) }) > 0;
            const bool focus = int(i) == hover || int(j) == hover || int(i) == selected_ || int(j) == selected_;
            if (!onP && !focus && (!allEdges || !vis[i] || !vis[j])) continue; // both ends on screen, or the focus / path
            if (filtering && !onP && !focus && (!match_[i] || !match_[j])) continue;
            const ImVec2 a = toScreen(ImVec2(nodes_[i].pos.x + kW, nodes_[i].pos.y + nodeH * 0.5f));
            const ImVec2 b = toScreen(ImVec2(nodes_[j].pos.x, nodes_[j].pos.y + nodeH * 0.5f));
            ImU32 col = edgeColor(e.interpolate, onP);
            if (focus && !onP) col = e.interpolate ? IM_COL32(120, 180, 255, 230) : IM_COL32(220, 220, 230, 220);
            dl->AddLine(a, b, col, onP ? 3.0f : focus ? 2.0f : 1.0f);
            if ((focus || onP) && zoom_ > 0.3f)
            {
                const ImVec2 m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                dl->AddText(m, col, std::format("{:g}", e.cost).c_str());
            }
            ++drawnEdges;
        }
    }
    // nodes
    for (size_t i = 0; i < nodes_.size(); ++i)
    {
        if (!vis[i]) continue;
        const MoveState& s = g.states[i];
        const ImVec2 a = toScreen(nodes_[i].pos), b = toScreen(ImVec2(nodes_[i].pos.x + kW, nodes_[i].pos.y + nodeH));
        const bool dim = filtering && !match_[i];
        ImU32 fill = s.abstract ? IM_COL32(45, 45, 50, 230) : !s.fileExists ? IM_COL32(90, 35, 35, 230) : s.cfgFile < kGameFileBase ? IM_COL32(40, 70, 55, 235) : IM_COL32(48, 52, 64, 235);
        if (dim) fill = IM_COL32(32, 33, 37, 200);
        const bool target = i < actTarget_.size() && actTarget_[i];
        ImU32 border = int(i) == current ? IM_COL32(255, 80, 255, 255) : int(i) == selected_ ? IM_COL32(255, 210, 80, 255) : onPath[i] ? IM_COL32(255, 140, 50, 255)
                     : target ? IM_COL32(80, 230, 255, 255) : filtering && match_[i] && search[0] ? IM_COL32(255, 230, 120, 200) : IM_COL32(90, 95, 110, 200);
        const bool strong = int(i) == current || int(i) == selected_ || onPath[i] || target;
        if (b.x - a.x < 3.0f)
        {
            dl->AddRectFilled(a, ImVec2(a.x + (target ? 6.0f : 3.0f), a.y + (target ? 5.0f : 2.0f)), strong ? border : fill);
            continue;
        }
        dl->AddRectFilled(a, b, fill, 3.0f * zoom_);
        dl->AddRect(a, b, border, 3.0f * zoom_, 0, strong ? (target ? 3.5f : 2.5f) : 1.0f);
        if (!names) continue;
        const ImVec4 clip(a.x + 4, a.y, b.x - 4, b.y);
        const ImU32 tc = dim ? IM_COL32(110, 110, 115, 255) : IM_COL32(235, 235, 240, 255);
        dl->AddText(nullptr, nameSize, ImVec2(a.x + 4, a.y + std::max(1.0f, (zoom_ * kPlateH - nameSize) * 0.5f)), tc, s.name.c_str(), nullptr, 0.0f, &clip);
        if (cards)
        {
            const float lh = ImGui::GetTextLineHeight();
            dl->AddText(nullptr, 0.0f, ImVec2(a.x + 6, a.y + 3 + lh), IM_COL32(150, 155, 165, 255),
                        s.abstract ? "(no file: base class)" : fs::path(s.file).filename().string().c_str(), nullptr, 0.0f, &clip);
            dl->AddText(nullptr, 0.0f, ImVec2(a.x + 6, a.y + 3 + 2 * lh), IM_COL32(150, 155, 165, 255),
                        std::format("{:.2f} s{}  interp {:g}  {} out  {}", s.duration, s.looped ? " looped" : "", s.interpolationSpeed, s.edges.size(),
                                    s.cfgFile < kGameFileBase ? "mod" : "BI").c_str(), nullptr, 0.0f, &clip);
        }
    }
    // interaction
    if (hover >= 0)
    {
        const MoveState& s = g.states[size_t(hover)];
        ImGui::SetTooltip("%s\n%s\n%.2f s%s, interpolationSpeed %g, actions %s\n%zu links out", s.name.c_str(), s.file.c_str(), s.duration,
                          s.looped ? " looped" : "", s.interpolationSpeed, s.actions.c_str(), s.edges.size());
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            selected_ = hover;
            act.select = hover;
            if (io.KeyAlt) dragLinkFrom_ = hover;
            else dragNode_ = hover;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) act.play = hover;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGui::IsMouseDragging(ImGuiMouseButton_Right)) { ctxNode_ = hover; ImGui::OpenPopup("##statectx"); }
    }
    if (dragNode_ >= 0)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f))
            {
                nodes_[size_t(dragNode_)].pos.x += io.MouseDelta.x / zoom_;
                nodes_[size_t(dragNode_)].pos.y += io.MouseDelta.y / zoom_;
                movedNode_ = true;
            }
        }
        else
        {
            if (movedNode_) Save();
            dragNode_ = -1;
            movedNode_ = false;
        }
    }
    if (dragLinkFrom_ >= 0)
    {
        const ImVec2 a = toScreen(ImVec2(nodes_[size_t(dragLinkFrom_)].pos.x + kW, nodes_[size_t(dragLinkFrom_)].pos.y + nodeH * 0.5f));
        dl->AddLine(a, io.MousePos, IM_COL32(120, 200, 255, 255), 2.0f);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (hover >= 0 && hover != dragLinkFrom_)
            {
                act.linkFrom = dragLinkFrom_;
                act.linkTo = hover;
                act.linkInterpolate = !io.KeyShift; // Shift: ConnectTo
            }
            dragLinkFrom_ = -1;
        }
    }
    if (ImGui::BeginPopup("##statectx"))
    {
        if (ctxNode_ >= 0 && ctxNode_ < int(g.states.size()))
        {
            ImGui::TextDisabled("%s", g.states[size_t(ctxNode_)].name.c_str());
            if (ImGui::MenuItem("playMove (path)")) act.play = ctxNode_;
            if (ImGui::IsItemHovered()) ui::SetTooltip("Goes to this state through the transitions, as playMove does in the game.");
            if (ImGui::MenuItem("switchMove (jump)")) act.switchTo = ctxNode_;
            if (ImGui::IsItemHovered()) ui::SetTooltip("Jumps straight to this state, as switchMove does in the game.");
            if (ImGui::MenuItem("Select")) { selected_ = ctxNode_; act.select = ctxNode_; }
            if (ImGui::IsItemHovered()) ui::SetTooltip("Selects this state: its details in the Inspector.");
        }
        ImGui::EndPopup();
    }
    dl->PopClipRect();
    // overlay: counts and draw time
    lastDrawMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    dl->AddText(ImVec2(c0.x + 8, c0.y + size.y - ImGui::GetTextLineHeight() - 6), IM_COL32(150, 150, 160, 255),
                std::format("zoom {:.2f}  visible {}  links {}  {:.2f} ms", zoom_, visibleNodes, drawnEdges, lastDrawMs).c_str());
    return act;
}
