#include "NodeGraph.h"
#include "Lang.h"
#include "Log.h"

#include <imgui.h>
#include <imgui-node-editor/imgui_node_editor.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <set>

namespace ed = ax::NodeEditor;
namespace fs = std::filesystem;

struct MinimapData
{
    ImVec2 regionMin, regionMax;   // screen rect of the canvas
    ImVec2 viewMin, viewMax;       // visible canvas rect
    struct R { ImVec2 a, b; ImU32 col; int node; };
    std::vector<R> rects;          // canvas rects of visible nodes
};

namespace {

uintptr_t Hash(const std::string& s)
{
    uintptr_t h = std::hash<std::string>{}(s);
    return h ? h : 1;
}

ImVec4 SeverityColor(Severity s)
{
    switch (s)
    {
    case Severity::Error: return { 1.0f, 0.25f, 0.25f, 1.0f };
    case Severity::Warning: return { 1.0f, 0.7f, 0.2f, 1.0f };
    case Severity::Inactive: return { 0.45f, 0.45f, 0.48f, 1.0f };
    default: return { 0.45f, 0.75f, 0.45f, 1.0f };
    }
}

const char* TypeName(CfgAnimType t)
{
    static const char* n[] = { "rotation", "rotationX", "rotationY", "rotationZ", "translation", "translationX",
                               "translationY", "translationZ", "hide", "direct", "?" };
    return n[int(t)];
}

} // namespace

// ============================================================================================ lifecycle

CfgGraph::~CfgGraph() { Shutdown(); }

void CfgGraph::Shutdown()
{
    Save();
    if (ctx_) ed::DestroyEditor(ctx_);
    ctx_ = nullptr;
}

void CfgGraph::SetSearch(const std::string& text)
{
    strncpy_s(search_, text.c_str(), _TRUNCATE);
    filterDirty_ = true;
}

void CfgGraph::Save()
{
    if (nodes_.empty() || saveFile_.empty() || !ctx_) return;
    ed::SetCurrentEditor(ctx_);
    std::error_code ec;
    fs::create_directories(saveFile_.parent_path(), ec);
    // keep positions of nodes that exist only in the other collapse mode
    for (size_t i = 0; i < nodes_.size(); ++i)
    {
        const Node& n = nodes_[i];
        ImVec2 p = compact_ && i < fullPos_.size() ? ImVec2(fullPos_[i].first, fullPos_[i].second) : ed::GetNodePosition(ed::NodeId(n.id));
        if (p.x == FLT_MAX) p = ImVec2(n.x, n.y);
        saved_[n.id] = { p.x, p.y };
    }
    std::ofstream f(saveFile_, std::ios::trunc);
    for (const auto& [id, p] : saved_) f << id << ' ' << p.first << ' ' << p.second << '\n';
    ed::SetCurrentEditor(nullptr);
}

void CfgGraph::Rebuild(const fs::path& saveFile, const Skeleton& skel, const std::vector<CfgAnimation>& anims,
                       const std::vector<CfgSource>& sources, const std::vector<AnimStatus>& status,
                       const std::vector<size_t>& bonePoints)
{
    if (!nodes_.empty()) Save();
    saveFile_ = saveFile;
    saved_.clear();
    if (std::ifstream f(saveFile_); f)
    {
        uintptr_t id;
        float x, y;
        while (f >> id >> x >> y) saved_[id] = { x, y };
    }
    in_ = {};
    for (const auto& b : skel.bones)
    {
        in_.boneNames.push_back(b.name);
        in_.boneParent.push_back(b.parent);
    }
    in_.bonePoints = bonePoints;
    in_.bonePoints.resize(skel.bones.size(), 0);
    in_.anims = anims;
    in_.status = status;
    for (const auto& s : sources) in_.sources.push_back(s.name);
    if (sourceFilter_ >= int(in_.sources.size())) sourceFilter_ = -1;
    focusNode_ = -1;
    BuildNodes();
}

// ============================================================================================ graph building

void CfgGraph::BuildNodes()
{
    compact_ = false;
    fullPos_.clear();
    nodes_.clear();
    links_.clear();
    const auto& bn = in_.boneNames;
    const auto& parent = in_.boneParent;

    std::vector<int> sourceNode(in_.sources.size(), -1);
    for (int s = 0; s < int(in_.sources.size()); ++s)
    {
        Node n;
        n.kind = Kind::Source;
        n.index = s;
        n.id = Hash("S:" + ToLower(in_.sources[s]));
        n.title = in_.sources[s];
        n.layer = 0;
        sourceNode[s] = int(nodes_.size());
        nodes_.push_back(n);
    }

    std::vector<bool> animated(bn.size(), false);
    for (const auto& a : in_.anims)
        if (a.bone >= 0 && a.bone < int(bn.size())) animated[a.bone] = true;

    std::map<int, int> boneNode, chainNode;
    auto addLink = [&](int a, int b, Severity s) {
        links_.push_back({ Hash(std::format("L:{}>{}", nodes_[a].id, nodes_[b].id)), OutPin(nodes_[a]), InPin(nodes_[b]), a, b, s });
    };
    std::function<int(int)> boneNodeFor;
    // node for the parent side of bone b (a bone node or a collapsed chain), -1 at the root
    std::function<int(int)> parentNodeOf = [&](int b) -> int {
        int p = parent[b];
        if (p < 0) return -1;
        if (!collapseChains_ || animated[p]) return boneNodeFor(p);
        if (auto it = chainNode.find(p); it != chainNode.end()) return it->second;
        std::vector<int> chain;
        int q = p;
        for (; q >= 0 && !animated[q]; q = parent[q]) chain.push_back(q);
        Node n;
        n.kind = Kind::Chain;
        n.index = p;
        n.bone = p;
        n.id = Hash("C:" + ToLower(bn[p]));
        std::string names;
        for (size_t i = 0; i < chain.size(); ++i) names += (i ? " > " : "") + bn[chain[i]];
        if (chain.size() <= 3) n.title = names;
        else n.title = bn[chain.front()] + " > ... > " + bn[chain.back()];
        if (q < 0) n.title += " > (root)";
        n.lines.push_back(std::format("{} parent bone(s), not animated", chain.size()));
        if (chain.size() > 3) n.lines.push_back(names.size() > 70 ? names.substr(0, 67) + "..." : names);
        n.severity = Severity::Inactive;
        int self = int(nodes_.size());
        chainNode[p] = self;
        nodes_.push_back(n);
        if (q >= 0) addLink(self, boneNodeFor(q), Severity::Ok);
        return self;
    };
    boneNodeFor = [&](int b) -> int {
        if (auto it = boneNode.find(b); it != boneNode.end()) return it->second;
        Node n;
        n.kind = Kind::Bone;
        n.index = b;
        n.bone = b;
        n.id = Hash("B:" + ToLower(bn[b]));
        n.title = bn[b];
        size_t pts = in_.bonePoints[b];
        n.lines.push_back(pts ? std::format("{} points in this LOD", pts) : "no points in this LOD");
        n.severity = pts ? Severity::Ok : Severity::Inactive;
        int self = int(nodes_.size());
        boneNode[b] = self;
        nodes_.push_back(n);
        if (int pn = parentNodeOf(b); pn >= 0) addLink(self, pn, Severity::Ok);
        return self;
    };

    for (int i = 0; i < int(in_.anims.size()); ++i)
    {
        const CfgAnimation& a = in_.anims[i];
        Node n;
        n.kind = Kind::Anim;
        n.index = i;
        n.bone = a.bone;
        n.layer = 1;
        n.id = Hash("A:" + ToLower(a.name) + "#" + std::to_string(i));
        n.title = a.name;
        n.lines.push_back(std::format("{}  sel: {}", a.typeName.empty() ? TypeName(a.type) : a.typeName, a.selection));
        if (a.type == CfgAnimType::Direct) n.lines.push_back(std::format("axisPos {} / axisDir {}", a.axisPosName, a.axisDirName));
        else if (!a.begin.empty() || !a.end.empty()) n.lines.push_back(std::format("begin/end {} / {}", a.begin, a.end));
        else if (!a.axis.empty()) n.lines.push_back(std::format("axis {}", a.axis));
        n.lines.push_back(std::format("memory={}  value {:g}..{:g}", a.memory ? 1 : 0, a.minValue, a.maxValue));
        if (a.type == CfgAnimType::Hide) n.lines.push_back(std::format("hide {:g} / unhide {:g}", a.hideValue, a.unHideValue));
        else if (a.type == CfgAnimType::Translation || a.type == CfgAnimType::TranslationX || a.type == CfgAnimType::TranslationY || a.type == CfgAnimType::TranslationZ)
            n.lines.push_back(std::format("offset {:g} .. {:g}", a.offset0, a.offset1));
        else if (a.type == CfgAnimType::Direct) n.lines.push_back(std::format("angle {:.1f} deg, offset {:g}", a.directAngle * 57.2958f, a.directOffset));
        else n.lines.push_back(std::format("angle {:.1f} .. {:.1f} deg", a.angle0 * 57.2958f, a.angle1 * 57.2958f));
        if (i < int(in_.status.size())) { n.severity = in_.status[i].severity; n.message = in_.status[i].message; }
        int self = int(nodes_.size());
        nodes_.push_back(n);
        if (a.sourceIndex >= 0 && a.sourceIndex < int(sourceNode.size())) addLink(sourceNode[a.sourceIndex], self, n.severity);
        if (a.bone >= 0 && a.bone < int(bn.size())) addLink(self, boneNodeFor(a.bone), n.severity);
    }

    // layers: bones/chains start right of the animations, parents always right of their children
    for (auto& n : nodes_)
        if (n.kind == Kind::Bone || n.kind == Kind::Chain) n.layer = 2;
    for (bool changed = true; changed;)
    {
        changed = false;
        for (const auto& l : links_)
            if (nodes_[l.a].layer >= 2 && nodes_[l.b].layer <= nodes_[l.a].layer)
            {
                nodes_[l.b].layer = nodes_[l.a].layer + 1;
                changed = true;
            }
    }

    Layout();
    int restored = 0;
    for (auto& n : nodes_)
        if (auto it = saved_.find(n.id); it != saved_.end())
        {
            n.x = it->second.first;
            n.y = it->second.second;
            n.fromSaved = true;
            ++restored;
        }
    applyPositions_ = true;
    measureFrames_ = restored < int(nodes_.size()) ? 2 : 0; // new nodes: refine their layout once sizes are known
    navigate_ = true;
    filterDirty_ = true;
    Log::Get().Debug("model.cfg graph: {} nodes, {} links ({} positions from {})", nodes_.size(), links_.size(), restored,
                     PathToUtf8(saveFile_.filename()));
}

// ============================================================================================ layout

void CfgGraph::Layout(const std::vector<bool>* mask)
{
    // Lay out every connected component on its own (layers left to right), then pack the components in rows
    // so the full graph fills a wide area instead of one very tall column.
    auto vis = [&](int i) { return !mask || (*mask)[i]; };
    const int N = int(nodes_.size());
    std::vector<int> comp(N, -1);
    std::vector<std::vector<int>> adj(N);
    for (const auto& l : links_)
        if (vis(l.a) && vis(l.b)) { adj[l.a].push_back(l.b); adj[l.b].push_back(l.a); }
    int nc = 0;
    for (int i = 0; i < N; ++i)
    {
        if (!vis(i) || comp[i] >= 0) continue;
        std::vector<int> st{ i };
        comp[i] = nc;
        while (!st.empty())
        {
            int v = st.back();
            st.pop_back();
            for (int w : adj[v])
                if (comp[w] < 0) { comp[w] = nc; st.push_back(w); }
        }
        ++nc;
    }
    struct Box { int c; float x0, y0, x1, y1; };
    std::vector<Box> boxes;
    std::vector<bool> m(N);
    double area = 0;
    for (int c = 0; c < nc; ++c)
    {
        for (int i = 0; i < N; ++i) m[i] = comp[i] == c;
        LayoutColumns(m);
        Box b{ c, 1e30f, 1e30f, -1e30f, -1e30f };
        for (int i = 0; i < N; ++i)
            if (m[i])
            {
                float w = nodes_[i].w > 0 ? nodes_[i].w : (nodes_[i].kind == Kind::Anim ? 330.0f : 230.0f);
                float h = nodes_[i].h > 0 ? nodes_[i].h : 120.0f;
                b.x0 = std::min(b.x0, nodes_[i].x); b.y0 = std::min(b.y0, nodes_[i].y);
                b.x1 = std::max(b.x1, nodes_[i].x + w); b.y1 = std::max(b.y1, nodes_[i].y + h);
            }
        area += double(b.x1 - b.x0 + 120) * double(b.y1 - b.y0 + 120);
        boxes.push_back(b);
    }
    // shelf packing, tallest first, rows about 16:9 overall
    std::stable_sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.y1 - a.y0 > b.y1 - b.y0; });
    float rowWidth = float(std::sqrt(area * 16.0 / 9.0));
    for (const auto& b : boxes) rowWidth = std::max(rowWidth, b.x1 - b.x0);
    float x = 0, y = 0, rowH = 0;
    for (const auto& b : boxes)
    {
        float w = b.x1 - b.x0, h = b.y1 - b.y0;
        if (x > 0 && x + w > rowWidth) { x = 0; y += rowH + 160; rowH = 0; }
        for (int i = 0; i < N; ++i)
            if (comp[i] == b.c) { nodes_[i].x += x - b.x0; nodes_[i].y += y - b.y0; }
        x += w + 160;
        rowH = std::max(rowH, h);
    }
}

void CfgGraph::LayoutColumns(const std::vector<bool>& maskRef)
{
    const std::vector<bool>* mask = &maskRef;
    auto vis = [&](int i) { return !mask || (*mask)[i]; };
    auto height = [&](const Node& n) {
        if (n.h > 0) return n.h;
        switch (n.kind)
        {
        case Kind::Anim: return 70.0f + 21.0f * float(n.lines.size()) + (n.message.empty() ? 0.0f : 42.0f);
        case Kind::Source: return 110.0f;
        default: return 60.0f + 21.0f * float(n.lines.size());
        }
    };
    const float gapY = 16.0f, gapX = 90.0f;

    int maxLayer = 0;
    for (int i = 0; i < int(nodes_.size()); ++i)
        if (vis(i)) maxLayer = std::max(maxLayer, nodes_[i].layer);
    std::vector<float> colWidth(maxLayer + 1, 220.0f), colX(maxLayer + 1, 0.0f);
    for (int i = 0; i < int(nodes_.size()); ++i)
        if (vis(i)) colWidth[nodes_[i].layer] = std::max(colWidth[nodes_[i].layer], nodes_[i].w > 0 ? nodes_[i].w : (nodes_[i].kind == Kind::Anim ? 330.0f : 230.0f));
    for (int c = 1; c <= maxLayer; ++c) colX[c] = colX[c - 1] + colWidth[c - 1] + gapX;

    std::vector<std::vector<int>> preds(nodes_.size());
    for (const auto& l : links_)
        if (vis(l.a) && vis(l.b)) preds[l.b].push_back(l.a);
    auto center = [&](int i) { return nodes_[i].y + height(nodes_[i]) * 0.5f; };
    auto stack = [&](std::vector<std::pair<float, int>>& order) { // sorted by desired centre; resolve overlaps
        std::stable_sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first < b.first; });
        float bottom = -1e30f;
        for (auto& [want, i] : order)
        {
            float y = std::max(want - height(nodes_[i]) * 0.5f, bottom + gapY);
            nodes_[i].y = y;
            bottom = y + height(nodes_[i]);
        }
    };

    // layer 1: animations grouped by source (source order), then config order
    std::vector<int> sourceOf(nodes_.size(), -1);
    for (const auto& l : links_)
        if (nodes_[l.a].kind == Kind::Source) sourceOf[l.b] = l.a;
    std::vector<int> anims;
    for (int i = 0; i < int(nodes_.size()); ++i)
        if (vis(i) && nodes_[i].kind == Kind::Anim) anims.push_back(i);
    std::stable_sort(anims.begin(), anims.end(), [&](int a, int b) {
        int sa = sourceOf[a] < 0 ? INT_MAX : sourceOf[a], sb = sourceOf[b] < 0 ? INT_MAX : sourceOf[b];
        return sa != sb ? sa < sb : nodes_[a].index < nodes_[b].index;
    });
    float y = 0;
    int prevSource = -2;
    for (int i : anims)
    {
        if (sourceOf[i] != prevSource && prevSource != -2) y += gapY * 2; // small gap between source groups
        prevSource = sourceOf[i];
        nodes_[i].x = colX[1];
        nodes_[i].y = y;
        y += height(nodes_[i]) + gapY;
    }
    // layer 0: sources next to the middle of their animations
    {
        std::vector<std::pair<float, int>> order;
        for (int i = 0; i < int(nodes_.size()); ++i)
            if (vis(i) && nodes_[i].kind == Kind::Source)
            {
                float sum = 0;
                int n = 0;
                for (const auto& l : links_)
                    if (l.a == i && vis(l.b)) { sum += center(l.b); ++n; }
                nodes_[i].x = colX[0];
                order.push_back({ n ? sum / float(n) : 1e9f, i });
            }
        stack(order);
    }
    // layers 2..: barycentre of predecessors (children / animations), left to right
    for (int c = 2; c <= maxLayer; ++c)
    {
        std::vector<std::pair<float, int>> order;
        for (int i = 0; i < int(nodes_.size()); ++i)
            if (vis(i) && nodes_[i].layer == c)
            {
                float sum = 0;
                int n = 0;
                for (int p : preds[i]) { sum += center(p); ++n; }
                nodes_[i].x = colX[c];
                order.push_back({ n ? sum / float(n) : 1e9f, i });
            }
        stack(order);
    }
}

void CfgGraph::ApplyFilterLayout(bool filtered)
{
    if (filtered)
    {
        if (!compact_) // remember the full-graph positions (possibly moved by the user)
        {
            fullPos_.resize(nodes_.size());
            if (ctx_) ed::SetCurrentEditor(ctx_);
            for (size_t i = 0; i < nodes_.size(); ++i)
            {
                ImVec2 p = ctx_ ? ed::GetNodePosition(ed::NodeId(nodes_[i].id)) : ImVec2(FLT_MAX, FLT_MAX);
                fullPos_[i] = p.x == FLT_MAX ? std::make_pair(nodes_[i].x, nodes_[i].y) : std::make_pair(p.x, p.y);
            }
            if (ctx_) ed::SetCurrentEditor(nullptr);
            compact_ = true;
        }
        for (size_t i = 0; i < nodes_.size(); ++i) { nodes_[i].x = fullPos_[i].first; nodes_[i].y = fullPos_[i].second; }
        Layout(&visible_);
        applyPositions_ = true;
    }
    else if (compact_)
    {
        for (size_t i = 0; i < nodes_.size() && i < fullPos_.size(); ++i) { nodes_[i].x = fullPos_[i].first; nodes_[i].y = fullPos_[i].second; }
        compact_ = false;
        applyPositions_ = true;
    }
}

void CfgGraph::UpdateVisibility()
{
    filterDirty_ = false;
    navigate_ = true; // fit the view to what is shown
    visible_.assign(nodes_.size(), true);

    if (focusNode_ >= 0 && focusNode_ < int(nodes_.size())) // focus: neighbourhood of one node, any direction
    {
        visible_.assign(nodes_.size(), false);
        std::vector<int> frontier{ focusNode_ };
        visible_[focusNode_] = true;
        for (int d = 0; d < focusDepth_; ++d)
        {
            std::vector<int> next;
            for (int i : frontier)
                for (const auto& l : links_)
                {
                    int other = l.a == i ? l.b : l.b == i ? l.a : -1;
                    if (other >= 0 && !visible_[other]) { visible_[other] = true; next.push_back(other); }
                }
            frontier.swap(next);
        }
        ApplyFilterLayout(true);
        return;
    }

    const std::string q = ToLower(search_);
    if (q.empty() && sourceFilter_ < 0 && !onlyProblems_)
    {
        ApplyFilterLayout(false);
        return;
    }
    std::vector<bool> seed(nodes_.size(), false);
    for (int i = 0; i < int(nodes_.size()); ++i)
    {
        const Node& n = nodes_[i];
        bool m = true;
        if (!q.empty()) m = ToLower(n.title).find(q) != std::string::npos;
        if (sourceFilter_ >= 0) m = m && n.kind == Kind::Source && n.index == sourceFilter_;
        if (onlyProblems_) m = (q.empty() && sourceFilter_ < 0 ? true : m) && n.kind == Kind::Anim &&
                               (n.severity == Severity::Error || n.severity == Severity::Warning);
        seed[i] = m;
    }
    // expand: forward (source -> anim -> bone -> parents) and backward (bone/anim -> their sources)
    visible_.assign(nodes_.size(), false);
    std::vector<int> seeds;
    for (int i = 0; i < int(nodes_.size()); ++i)
        if (seed[i]) { visible_[i] = true; seeds.push_back(i); }
    std::vector<int> fwd = seeds;
    while (!fwd.empty())
    {
        int i = fwd.back();
        fwd.pop_back();
        for (const auto& l : links_)
            if (l.a == i && !visible_[l.b]) { visible_[l.b] = true; fwd.push_back(l.b); }
    }
    std::vector<int> back = seeds;
    while (!back.empty())
    {
        int i = back.back();
        back.pop_back();
        for (const auto& l : links_)
            if (l.b == i && !visible_[l.a] && (nodes_[l.a].kind == Kind::Source || nodes_[l.a].kind == Kind::Anim))
            {
                visible_[l.a] = true;
                back.push_back(l.a);
            }
    }
    ApplyFilterLayout(true);
}

// ============================================================================================ drawing

void CfgGraph::DrawMinimap(const MinimapData& mm)
{
    if (mm.rects.empty()) return;
    const ImVec2 size(240, 170);
    const ImVec2 pos(mm.regionMax.x - size.x - 10, mm.regionMax.y - size.y - 10);
    ImVec2 bmin = mm.viewMin, bmax = mm.viewMax;
    for (const auto& r : mm.rects)
    {
        bmin = ImVec2(std::min(bmin.x, r.a.x), std::min(bmin.y, r.a.y));
        bmax = ImVec2(std::max(bmax.x, r.b.x), std::max(bmax.y, r.b.y));
    }
    float sx = (size.x - 8) / std::max(1.0f, bmax.x - bmin.x), sy = (size.y - 8) / std::max(1.0f, bmax.y - bmin.y);
    float s = std::min(sx, sy);
    ImVec2 off(pos.x + 4 + ((size.x - 8) - (bmax.x - bmin.x) * s) * 0.5f, pos.y + 4 + ((size.y - 8) - (bmax.y - bmin.y) * s) * 0.5f);
    auto map = [&](ImVec2 p) { return ImVec2(off.x + (p.x - bmin.x) * s, off.y + (p.y - bmin.y) * s); };

    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginChild("##minimap", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(20, 22, 26, 230));
    for (const auto& r : mm.rects)
    {
        ImVec2 a = map(r.a), b = map(r.b);
        if (b.x - a.x < 1.5f) b.x = a.x + 1.5f;
        if (b.y - a.y < 1.5f) b.y = a.y + 1.5f;
        dl->AddRectFilled(a, b, r.col);
    }
    dl->AddRect(map(mm.viewMin), map(mm.viewMax), IM_COL32(255, 255, 255, 200), 0, 0, 1.5f);
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##mmclick", size);
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        ImVec2 m = ImGui::GetIO().MousePos;
        ImVec2 c(bmin.x + (m.x - off.x) / s, bmin.y + (m.y - off.y) / s);
        float best = 1e30f;
        for (const auto& r : mm.rects)
        {
            float dx = c.x - (r.a.x + r.b.x) * 0.5f, dy = c.y - (r.a.y + r.b.y) * 0.5f;
            if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; pendingNavNode_ = r.node; }
        }
    }
    if (ImGui::IsItemHovered()) ui::SetTooltip("Minimap: click / drag to jump");
    ImGui::EndChild();
}

CfgGraph::Actions CfgGraph::Draw(std::vector<CfgSource>& sources, int selectedBone)
{
    Actions act;
    if (!ctx_)
    {
        ed::Config cfg;
        cfg.SettingsFile = nullptr; // positions are kept in our own per-model file
        // Accept (and drop) the editor's own settings. Without a sink its save "fails", the dirty flag is never cleared
        // and it serializes every node to JSON again each frame (the larger half of the graph's frame time on ~300 nodes).
        cfg.SaveSettings = [](const char*, size_t, ed::SaveReasonFlags, void*) { return true; };
        ctx_ = ed::CreateEditor(&cfg);
    }

    // ---- toolbar
    ImGui::SetNextItemWidth(170);
    if (ImGui::InputTextWithHint("##search", "search name", search_, sizeof(search_))) { focusNode_ = -1; filterDirty_ = true; }
    if (ImGui::IsItemHovered()) ui::SetTooltip("Shows only the nodes whose name contains this text.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    const bool srcOpen = ImGui::BeginCombo("##srcfilter", sourceFilter_ < 0 ? "all sources" : in_.sources[sourceFilter_].c_str());
    if (!srcOpen) if (ImGui::IsItemHovered()) ui::SetTooltip("Shows only the animations driven by this source.");
    if (srcOpen)
    {
        if (ImGui::Selectable("all sources", sourceFilter_ < 0)) { sourceFilter_ = -1; focusNode_ = -1; filterDirty_ = true; }
        for (int i = 0; i < int(in_.sources.size()); ++i)
            if (ImGui::Selectable(in_.sources[i].c_str(), sourceFilter_ == i)) { sourceFilter_ = i; focusNode_ = -1; filterDirty_ = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("only problems", &onlyProblems_)) { focusNode_ = -1; filterDirty_ = true; }
    if (ImGui::IsItemHovered()) ui::SetTooltip("Shows only the nodes with a problem (missing bone, selection, axis or source).");
    ImGui::SameLine();
    bool rebuild = ImGui::Checkbox("collapse parents", &collapseChains_);
    if (ImGui::IsItemHovered()) ui::SetTooltip("Runs of parent bones without animations become one node (\"a > ... > b > (root)\")");
    ImGui::SameLine();
    ImGui::Checkbox("minimap", &showMinimap_);
    if (ImGui::IsItemHovered()) ui::SetTooltip("Shows a small overview of the whole graph in the corner.");
    ImGui::SameLine();
    bool fit = ImGui::Button("Fit");
    if (ImGui::IsItemHovered()) ui::SetTooltip("Zooms the graph so that every node is visible.");
    ImGui::SameLine();
    bool relayout = ImGui::Button("Re-layout");
    if (ImGui::IsItemHovered()) ui::SetTooltip("Arranges the nodes again from scratch.");
    if (focusNode_ >= 0 && focusNode_ < int(nodes_.size()))
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 1.0f, 1.0f), "Focus: %s", nodes_[focusNode_].title.c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        if (ui::SliderInt("depth", &focusDepth_, 1, 3)) filterDirty_ = true;
        if (ImGui::IsItemHovered()) ui::SetTooltip("How many links away from the focused node are shown.");
        ImGui::SameLine();
        if (ImGui::SmallButton("x (Esc)")) { focusNode_ = -1; filterDirty_ = true; }
        if (ImGui::IsItemHovered()) ui::SetTooltip("Leaves the focus: the whole graph again.");
    }
    else
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%zu nodes | double-click: focus, Esc: back", nodes_.size());
    }
    if (focusNode_ >= 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        focusNode_ = -1;
        filterDirty_ = true;
    }
    if (rebuild)
    {
        Save();
        int keepFocus = -1;
        BuildNodes();
        focusNode_ = keepFocus;
    }
    if (filterDirty_ || visible_.size() != nodes_.size()) UpdateVisibility();

    // ---- canvas
    const ImVec2 regionMin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    MinimapData mm;
    mm.regionMin = regionMin;
    mm.regionMax = ImVec2(regionMin.x + avail.x, regionMin.y + avail.y);

    ed::SetCurrentEditor(ctx_);
    const char* nodeTip = nullptr; // a control inside a node is hovered: its tooltip after ed::End
    ed::Begin("cfggraph", ImVec2(0, 0));
    if (relayout) // drop saved positions and lay out again from scratch
    {
        saved_.clear();
        std::error_code ec;
        fs::remove(saveFile_, ec);
        for (auto& n : nodes_) n.fromSaved = false;
        compact_ = false;
        Layout();
        applyPositions_ = true;
        navigate_ = true;
        filterDirty_ = true;
    }
    bool positionsApplied = false;
    if (applyPositions_)
    {
        for (const auto& n : nodes_) ed::SetNodePosition(ed::NodeId(n.id), ImVec2(n.x, n.y));
        applyPositions_ = false;
        positionsApplied = true;
    }

    // Per-node level of detail. Every node keeps its pins and its size (from the last full draw), so links, Re-layout,
    // minimap, focus mode and NodeScreenCenter see the same geometry; only what is drawn inside changes:
    //   full   - on screen and text readable;
    //   bars   - on screen but zoomed out so far that text is a smudge: body lines / widgets become bars (title kept
    //            while it is still readable, a bar too below that);
    //   culled - outside the visible canvas: nothing but the pins and an empty dummy of the node size.
    const float fontSize = ImGui::GetFontSize();
    const float screenScale = 1.0f / std::max(1e-4f, ed::GetCurrentZoom());
    const bool barsBody = fontSize * screenScale < 5.0f, barsTitle = fontSize * screenScale < 3.5f;
    const float cullMargin = 40.0f / screenScale;
    const ImVec2 viewMin = ed::ScreenToCanvas(mm.regionMin), viewMax = ed::ScreenToCanvas(mm.regionMax);
    auto sub = [](ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); };
    auto add = [](ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); };
    auto withAlpha = [](ImVec4 c, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, a)); };
    const ImU32 textBarCol = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.4f);
    for (int i = 0; i < int(nodes_.size()); ++i)
    {
        if (!visible_[i]) continue;
        Node& n = nodes_[i];
        DrawCache& c = n.cache;
        const bool cached = c.fontSize == fontSize && c.stable >= 2;
        bool culled = false;
        if (cached && !positionsApplied && i < int(nodeRects_.size())) // canvas rect of the last frame
        {
            const ImVec2 p = nodeRects_[i].first, sz = nodeRects_[i].second;
            culled = p.x != FLT_MAX && sz.x > 0 &&
                     (p.x > viewMax.x + cullMargin || p.y > viewMax.y + cullMargin || p.x + sz.x < viewMin.x - cullMargin ||
                      p.y + sz.y < viewMin.y - cullMargin);
        }
        const bool full = !cached || (!culled && !barsBody);

        ImVec4 border = SeverityColor(n.severity);
        float bw = n.severity == Severity::Error || n.severity == Severity::Warning ? 3.0f : 1.0f;
        if ((n.bone >= 0 && n.bone == selectedBone && n.kind != Kind::Chain) || i == focusNode_) { border = ImVec4(1.0f, 0.3f, 1.0f, 1.0f); bw = 3.0f; }
        ed::PushStyleColor(ed::StyleColor_NodeBorder, border);
        ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, bw);
        ed::BeginNode(ed::NodeId(n.id));
        ImGui::PushID(i);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::BeginGroup();
        const ImVec4 titleCol = n.kind == Kind::Source ? ImVec4(0.55f, 0.8f, 1.0f, 1) : n.kind == Kind::Anim ? ImVec4(1, 1, 1, 1)
                              : n.kind == Kind::Bone ? ImVec4(1.0f, 0.85f, 0.35f, 1) : ImVec4(0.75f, 0.7f, 0.5f, 1);
        const char* prefix = n.kind == Kind::Source ? "source " : n.kind == Kind::Bone ? "bone " : n.kind == Kind::Chain ? "parents " : "";
        if (full)
        {
            DrawCache r;
            r.fontSize = fontSize;
            auto item = [&](ImVec2& a, ImVec2& b) { a = sub(ImGui::GetItemRectMin(), o); b = sub(ImGui::GetItemRectMax(), o); };
            auto bar = [&](ImU32 col, bool text) { ImVec2 a, b; item(a, b); r.bars.push_back({ a, b, col, text }); };
            if (n.kind != Kind::Source)
            {
                ed::BeginPin(ed::PinId(InPin(n)), ed::PinKind::Input);
                ImGui::TextUnformatted(">");
                ed::EndPin();
                item(r.inA, r.inB);
                ImGui::SameLine();
            }
            ImGui::TextColored(titleCol, "%s%s", prefix, n.title.c_str());
            item(r.titleA, r.titleB);
            for (const auto& l : n.lines)
            {
                ImGui::TextDisabled("%s", l.c_str());
                bar(textBarCol, true);
            }
            if (!n.message.empty())
            {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 330.0f); // keep error nodes narrow
                ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(n.severity));
                ImGui::TextWrapped("%s", n.message.c_str());
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
                bar(withAlpha(SeverityColor(n.severity), 0.4f), true);
            }
            if (n.kind == Kind::Source && n.index < int(sources.size()))
            {
                CfgSource& s = sources[n.index];
                ImGui::SetNextItemWidth(170);
                if (ui::SliderFloat("##v", &s.value, s.minValue, s.maxValue, "%.3f")) act.sourcesChanged = true;
                if (ImGui::IsItemHovered()) nodeTip = "Source value: drives the animations linked to this source.\nCtrl+click: type a value.";
                bar(ImGui::GetColorU32(ImGuiCol_FrameBg), false);
                if (ImGui::Checkbox("oscillate", &s.animate)) act.sourcesChanged = true;
                if (ImGui::IsItemHovered()) nodeTip = "Moves the source value back and forth between its min and max.";
                ImVec2 a, b;
                item(a, b);
                const float h = b.y - a.y;
                r.bars.push_back({ a, ImVec2(a.x + h, b.y), ImGui::GetColorU32(ImGuiCol_FrameBg), false });
                r.bars.push_back({ ImVec2(a.x + h + ImGui::GetStyle().ItemInnerSpacing.x, a.y), b, textBarCol, true });
            }
            if (n.kind == Kind::Anim)
            {
                if (ImGui::SmallButton("open in model.cfg")) act.openAnim = n.index;
                if (ImGui::IsItemHovered()) nodeTip = "Opens model.cfg in the editor at this animation's class.";
                bar(ImGui::GetColorU32(ImGuiCol_Button), false);
            }
            ed::BeginPin(ed::PinId(OutPin(n)), ed::PinKind::Output);
            ImGui::TextUnformatted(n.kind == Kind::Bone || n.kind == Kind::Chain ? "parent >" : ">");
            ed::EndPin();
            item(r.outA, r.outB);
            ImGui::EndGroup();
            r.size = ImGui::GetItemRectSize();
            const bool same = c.fontSize == fontSize && c.size.x == r.size.x && c.size.y == r.size.y && c.outA.x == r.outA.x && c.outA.y == r.outA.y;
            r.stable = same ? std::min(c.stable + 1, 2) : 0;
            c = std::move(r);
        }
        else
        {
            // the same items at the same places as the full draw (so the node size and pin rects do not change)
            ImDrawList* dl = ImGui::GetWindowDrawList();
            auto textBar = [&](ImVec2 a, ImVec2 b, ImU32 col) { // one bar per text line, about the x-height
                const float lh = ImGui::GetTextLineHeightWithSpacing(), th = fontSize * 0.3f;
                for (float y = a.y; y + fontSize * 0.5f < b.y; y += lh)
                    dl->AddRectFilled(add(o, ImVec2(a.x, y + fontSize * 0.4f)), add(o, ImVec2(b.x, y + fontSize * 0.4f + th)), col);
            };
            if (n.kind != Kind::Source)
            {
                ImGui::SetCursorScreenPos(add(o, c.inA));
                ed::BeginPin(ed::PinId(InPin(n)), ed::PinKind::Input);
                ImGui::Dummy(sub(c.inB, c.inA));
                ed::EndPin();
            }
            if (!culled)
            {
                if (!barsTitle)
                {
                    ImGui::SetCursorScreenPos(add(o, c.titleA));
                    ImGui::TextColored(titleCol, "%s%s", prefix, n.title.c_str());
                }
                else textBar(c.titleA, c.titleB, withAlpha(titleCol, 0.55f));
                if (n.kind != Kind::Source) textBar(c.inA, c.inB, textBarCol);
                for (const auto& b : c.bars)
                {
                    if (b.text) textBar(b.a, b.b, b.col);
                    else dl->AddRectFilled(add(o, b.a), add(o, b.b), b.col, ImGui::GetStyle().FrameRounding);
                }
                textBar(c.outA, c.outB, textBarCol);
            }
            ImGui::SetCursorScreenPos(add(o, c.outA));
            ed::BeginPin(ed::PinId(OutPin(n)), ed::PinKind::Output);
            ImGui::Dummy(sub(c.outB, c.outA));
            ed::EndPin();
            ImGui::SetCursorScreenPos(o);
            ImGui::Dummy(c.size);
            ImGui::EndGroup();
        }
        ImGui::PopID();
        ed::EndNode();
        ed::PopStyleVar();
        ed::PopStyleColor();
    }
    for (const auto& l : links_)
    {
        if (!visible_[l.a] || !visible_[l.b]) continue;
        bool boneLink = nodes_[l.a].layer >= 2;
        ImVec4 c = l.severity == Severity::Error || l.severity == Severity::Warning ? SeverityColor(l.severity)
                 : boneLink ? ImVec4(0.8f, 0.7f, 0.35f, 0.8f) : ImVec4(0.7f, 0.75f, 0.85f, 0.8f);
        ed::Link(ed::LinkId(l.id), ed::PinId(l.from), ed::PinId(l.to), c, l.severity == Severity::Error ? 2.5f : 1.5f);
    }

    // second pass of the automatic layout: real node sizes are known now
    if (measureFrames_ > 0 && --measureFrames_ == 0)
    {
        for (auto& n : nodes_)
        {
            ImVec2 sz = ed::GetNodeSize(ed::NodeId(n.id));
            if (sz.x > 0) { n.w = sz.x; n.h = sz.y; }
        }
        Layout();
        for (auto& n : nodes_)
            if (n.fromSaved)
                if (auto it = saved_.find(n.id); it != saved_.end()) { n.x = it->second.first; n.y = it->second.second; }
        applyPositions_ = true;
        navigate_ = true;
        compact_ = false;
        filterDirty_ = true; // a filter / focus re-applies its compact layout next frame
    }

    // double-click: focus mode
    if (ed::NodeId dc = ed::GetDoubleClickedNode(); dc)
        for (int i = 0; i < int(nodes_.size()); ++i)
            if (nodes_[i].id == uintptr_t(dc.Get())) { focusNode_ = i; filterDirty_ = true; }

    // Fit the view to the visible nodes once the layout is final (select them, navigate, clear the selection;
    // NavigateToContent would also count nodes hidden by the filter).
    if (navigate_ && measureFrames_ == 0 && !applyPositions_)
    {
        navigate_ = false;
        navigateDelay_ = 3; // the canvas and node bounds settle after a couple of frames
    }
    auto navigateTo = [&](auto pred, bool zoomIn) {
        ed::ClearSelection();
        for (int i = 0; i < int(nodes_.size()); ++i)
            if (visible_[i] && pred(i)) ed::SelectNode(ed::NodeId(nodes_[i].id), true);
        ed::NavigateToSelection(zoomIn, 0.0f);
        ed::ClearSelection();
        lastSelected_ = 0;
    };
    if ((navigateDelay_ > 0 && --navigateDelay_ == 0) || fit) navigateTo([](int) { return true; }, true);
    if (pendingNavNode_ >= 0)
    {
        int target = pendingNavNode_;
        pendingNavNode_ = -1;
        navigateTo([&](int i) { return i == target; }, false);
    }

    // selection -> bone highlight
    ed::NodeId sel[1];
    int ns = ed::GetSelectedNodes(sel, 1);
    uintptr_t cur = ns == 1 ? uintptr_t(sel[0].Get()) : 0;
    if (cur != lastSelected_)
    {
        lastSelected_ = cur;
        for (const auto& n : nodes_)
            if (n.id == cur && n.bone >= 0) act.selectBone = n.bone;
    }

    // minimap data (canvas coordinates)
    // A layout computed this frame is applied to the editor only on the next one: take positions from nodes_
    // then, so the minimap does not lag a frame behind.
    // (editor position / size lookups are linear searches: one each per node and frame)
    screenRects_.clear();
    nodeRects_.assign(nodes_.size(), { ImVec2(FLT_MAX, FLT_MAX), ImVec2(0, 0) });
    mm.viewMin = ed::ScreenToCanvas(mm.regionMin);
    mm.viewMax = ed::ScreenToCanvas(mm.regionMax);
    for (int i = 0; i < int(nodes_.size()); ++i)
    {
        if (!visible_[i]) continue;
        ImVec2 p = ed::GetNodePosition(ed::NodeId(nodes_[i].id)), sz = ed::GetNodeSize(ed::NodeId(nodes_[i].id));
        nodeRects_[i] = { p, sz };
        if (p.x == FLT_MAX || applyPositions_) p = ImVec2(nodes_[i].x, nodes_[i].y);
        if (sz.x <= 0) sz = ImVec2(nodes_[i].w > 0 ? nodes_[i].w : 200.0f, nodes_[i].h > 0 ? nodes_[i].h : 80.0f);
        if (!applyPositions_)
            screenRects_.push_back({ i, ed::CanvasToScreen(p), ed::CanvasToScreen(ImVec2(p.x + sz.x, p.y + sz.y)),
                                     ed::CanvasToScreen(ImVec2(p.x + sz.x * 0.5f, p.y + 16.0f)) }); // title line
        if (showMinimap_)
        {
            ImVec4 c = nodes_[i].severity == Severity::Error ? SeverityColor(Severity::Error)
                     : nodes_[i].kind == Kind::Source ? ImVec4(0.55f, 0.8f, 1.0f, 1) : nodes_[i].kind == Kind::Anim ? ImVec4(0.8f, 0.8f, 0.85f, 1)
                                                                                     : ImVec4(1.0f, 0.85f, 0.35f, 1);
            mm.rects.push_back({ p, ImVec2(p.x + sz.x, p.y + sz.y), ImGui::ColorConvertFloat4ToU32(c), i });
        }
    }
    ed::End();
    if (nodeTip) ui::SetTooltip(nodeTip);
    ed::SetCurrentEditor(nullptr);
    if (showMinimap_) DrawMinimap(mm);

    // keep positions on disk (only every few seconds while the window is open)
    double now = ImGui::GetTime();
    if (now - lastSave_ > 5.0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        lastSave_ = now;
        Save();
    }
    return act;
}

bool CfgGraph::NodeScreenCenter(const std::string& name, float& x, float& y) const
{
    const std::string q = ToLower(name);
    const ScreenRect* best = nullptr;
    auto title = [&](const ScreenRect& r) { return r.node < int(nodes_.size()) ? ToLower(nodes_[r.node].title) : std::string(); };
    for (const auto& r : screenRects_)
        if (title(r) == q) { best = &r; break; }
    if (!best)
        for (const auto& r : screenRects_)
            if (title(r).find(q) != std::string::npos) { best = &r; break; }
    if (!best) return false;
    // aim at the title line, not at sliders / buttons inside the node
    x = best->title.x;
    y = best->title.y;
    return true;
}
