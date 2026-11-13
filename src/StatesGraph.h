#pragma once
// Whole CfgMoves state graph on a custom canvas (thousands of states): culling to the visible area, level of detail
// by zoom (dots -> name plates -> full cards), layout in a background thread (columns by state family, barycentric
// ordering), positions kept by state name across rebuilds / hot reload and saved per move config, instant search and
// filters. Editing hooks (links by mouse, selection) for the move editor.
// Actions: a view "States / Actions"; an actions class filters the graph to the states that use it (+ its table action ->
// state with "show"); an action highlights every state it leads to; the Actions view is the matrix actions classes x
// actions (red: the action leads to a state that does not exist).
#include "Moves.h"

#include <imgui.h>

#include <atomic>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class StatesGraph
{
public:
    ~StatesGraph();
    // New / changed graph: positions of known state names stay, new states are placed by the layout thread.
    void SetGraph(const MovesGraph& g, const std::filesystem::path& positionsFile);
    void Save() const;

    struct Actions
    {
        int select = -1;           // clicked
        int play = -1;             // double click: playMove
        int switchTo = -1;         // context menu: switchMove
        int linkFrom = -1, linkTo = -1; bool linkInterpolate = true; // Alt+drag from a node to another: new link (editor)
        int focusRequest = -1;
    };
    // Draws into the current window (fills the remaining area). current: playing state, path: last playMove path.
    Actions Draw(const MovesGraph& g, int current, const std::vector<int>& path, float dpi);
    void FocusOn(int state, bool zoomIn = true);
    int Selected() const { return selected_; }
    void Select(int s) { selected_ = s; }

    // filters (UI above the canvas is drawn by Draw)
    char search[96] = {};
    int view = 0;              // 0 states graph, 1 actions (matrix)
    std::string actionsClass;  // lower case; "" all states
    std::string action;        // lower case; "" none highlighted
    char matrixFilter[64] = {};
    // the states an action leads to (of the chosen class, or of every class): (class, state index or -1 missing)
    std::vector<std::pair<std::string, int>> ActionTargets(const MovesGraph& g, const std::string& action) const;
    int filterSource = 0; // 0 all, 1 my mod only, 2 BI only
    bool filterRtm = false, filterAction = false; // search also matches the RTM path / the actions class
    double lastDrawMs = 0;
    int visibleNodes = 0, drawnEdges = 0;

private:
    struct Node { ImVec2 pos{ 0, 0 }; bool placed = false; };
    void DrawActionsBar(const MovesGraph& g, float dpi);
    void DrawActionsView(const MovesGraph& g, Actions& act, float dpi);
    void DrawClassTable(const MovesGraph& g, Actions& act, float dpi, float height);
    void BuildMatrix(const MovesGraph& g);
    // matrix cache: classes (lower) as rows, actions (lower) as columns
    const MovesGraph* matrixFor_ = nullptr;
    size_t matrixKey_ = 0;
    std::vector<std::string> mClasses_, mActions_;
    std::vector<uint8_t> actTarget_;
    std::string lastClass_, lastActionHl_, lastHlClass_;
    std::map<std::string, int> mUsed_; // actions class -> states using it
    int actTargets_ = 0;
    void StartLayout(const MovesGraph& g);
    void PollLayout();
    std::vector<Node> nodes_;
    std::vector<std::string> names_;
    std::filesystem::path file_;
    std::future<std::vector<ImVec2>> layoutJob_;
    std::vector<uint8_t> needLayout_;
    std::atomic<bool> cancel_{ false };
    // view
    ImVec2 pan_{ 0, 0 };
    float zoom_ = 0.15f;
    bool fitPending_ = true;
    int selected_ = -1;
    int dragLinkFrom_ = -1, dragNode_ = -1, ctxNode_ = -1, focusTarget_ = -1;
    bool movedNode_ = false;
    // search cache
    std::string lastSearch_;
    int lastFilter_ = -1;
    bool lastRtm_ = false, lastAct_ = false;
    std::vector<uint8_t> match_;
    int matches_ = 0, matchCursor_ = 0;
};
