#pragma once
// model.cfg graph window: source -> animation class -> bone (selection) -> parent bones.
// Built on imgui-node-editor. Layered left-to-right layout (barycentric ordering to limit crossings), computed once per
// (re)build; node positions are kept in graphs\<model>.graph next to the exe. Nothing is processed while the window is closed.
#include "Skeleton.h"
#include "Validation.h" // AnimStatus

#include <imgui.h>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace ax::NodeEditor { struct EditorContext; }

class CfgGraph
{
public:
    CfgGraph() = default;
    CfgGraph(const CfgGraph&) = delete;
    CfgGraph& operator=(const CfgGraph&) = delete;
    ~CfgGraph();

    // Rebuild the nodes. Positions: saved file if present, otherwise automatic layers.
    void Rebuild(const std::filesystem::path& saveFile, const Skeleton& skel, const std::vector<CfgAnimation>& anims,
                 const std::vector<CfgSource>& sources, const std::vector<AnimStatus>& status,
                 const std::vector<size_t>& bonePoints);
    void Save();
    void Shutdown(); // save + destroy the editor context (before ImGui is destroyed)
    void SetSearch(const std::string& text);
    bool Empty() const { return nodes_.empty(); }
    // Screen position of a visible node (title match, exact first, then substring); for --ui-script focus_node.
    bool NodeScreenCenter(const std::string& name, float& x, float& y) const;

    struct Actions
    {
        int selectBone = -1;     // a bone / animation node was clicked
        bool sourcesChanged = false;
        int openAnim = -1;       // "open in editor" pressed on an animation node
    };
    // Draws inside the current ImGui window.
    Actions Draw(std::vector<CfgSource>& sources, int selectedBone);

private:
    enum class Kind { Source, Anim, Bone, Chain };
    // Layout of a node's content from its last full draw, relative to the content origin (canvas units, zoom independent).
    // Off-screen nodes and nodes at a zoom where text is unreadable are submitted from it: same pin rects and same size,
    // body text replaced by bars (see Draw).
    struct DrawCache
    {
        struct Bar { ImVec2 a, b; ImU32 col; bool text; };
        float fontSize = 0;       // 0 = not recorded yet
        int stable = 0;           // full draws in a row with the same size (the first frames after a build differ)
        ImVec2 size, inA, inB, titleA, titleB, outA, outB;
        std::vector<Bar> bars;    // body lines and widgets
    };
    struct Node
    {
        uintptr_t id = 0;
        Kind kind = Kind::Source;
        int index = 0;          // source / anim / bone index
        float x = 0, y = 0;
        std::string title;
        std::vector<std::string> lines;
        Severity severity = Severity::Ok;
        std::string message;
        int bone = -1;          // bone moved by this node (anim, bone, first bone of a chain)
        int layer = 0;          // layout column
        float w = 0, h = 0;     // measured size (0 = not drawn yet)
        bool fromSaved = false; // position restored from the file
        DrawCache cache;
    };
    struct Link
    {
        uintptr_t id = 0, from = 0, to = 0; // pin ids
        int a = 0, b = 0;                   // node indices
        Severity severity = Severity::Ok;
    };
    // Copy of the inputs so the graph can be rebuilt locally (e.g. toggling chain collapse).
    struct Input
    {
        std::vector<std::string> boneNames;
        std::vector<int> boneParent;
        std::vector<size_t> bonePoints;
        std::vector<CfgAnimation> anims;
        std::vector<AnimStatus> status;
        std::vector<std::string> sources;
    };

    void BuildNodes();
    void Layout(const std::vector<bool>* mask = nullptr);
    void LayoutColumns(const std::vector<bool>& mask); // one connected component
    void ApplyFilterLayout(bool filtered);
    void UpdateVisibility();
    void DrawMinimap(const struct MinimapData& mm);
    uintptr_t InPin(const Node& n) const { return n.id ^ 0x1111111111111111ull; }
    uintptr_t OutPin(const Node& n) const { return n.id ^ 0x2222222222222222ull; }

    ax::NodeEditor::EditorContext* ctx_ = nullptr;
    Input in_;
    std::vector<Node> nodes_;
    std::vector<Link> links_;
    std::vector<bool> visible_;
    struct ScreenRect { int node; ImVec2 a, b, title; };
    std::vector<ScreenRect> screenRects_; // last drawn frame
    std::vector<std::pair<ImVec2, ImVec2>> nodeRects_; // last frame: canvas position, size per node (for culling)
    std::filesystem::path saveFile_;
    std::unordered_map<uintptr_t, std::pair<float, float>> saved_;
    bool applyPositions_ = false;
    int measureFrames_ = 0;    // > 0: lay out again from real node sizes after this many frames
    bool navigate_ = false;
    int navigateDelay_ = 0;
    int pendingNavNode_ = -1;  // minimap click: navigate to this node next frame
    bool compact_ = false;                         // filtered / focused view uses a temporary compact layout
    std::vector<std::pair<float, float>> fullPos_; // full-graph positions kept while compact
    double lastSave_ = 0.0;
    uintptr_t lastSelected_ = 0;
    // view options
    bool collapseChains_ = true;
    bool showMinimap_ = true;
    int focusNode_ = -1;
    int focusDepth_ = 1;
    // filters
    char search_[128] = {};
    int sourceFilter_ = -1;
    bool onlyProblems_ = false;
    bool filterDirty_ = true;
};
