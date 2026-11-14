#pragma once
// What the animation view asks of the window that shows the whole CfgMoves state graph (the ImGui StatesPanel here; any
// other UI can implement it). No ImGui.
#include <filesystem>

class MovesGraph;

class IStatesGraphHost
{
public:
    virtual ~IStatesGraphHost() = default;
    // a new / rebuilt graph (null: none); positionsFile keeps the node positions of this move config
    virtual void SetGraph(const MovesGraph* graph, const std::filesystem::path& positionsFile) = 0;
    virtual void Show(bool on) = 0;
    virtual void Toggle() = 0;
    virtual void ShowState(int state) = 0; // shown and centred on a state (index in the graph)
};
