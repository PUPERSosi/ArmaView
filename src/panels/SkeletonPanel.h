#pragma once
// Outliner of the Skeleton & Anim mode: the skeleton (name, inheritance, pivots) and its bone tree / filtered list.
#include "Session.h"
#include "ViewHost.h"

#include <string>

class SkeletonPanel
{
public:
    SkeletonPanel(Session& session, IViewHost& host) : session_(session), host_(host) {}
    void Draw();

private:
    void DrawBoneNode(int bone, int depth);
    Session& session_;
    IViewHost& host_;
    std::string filter_; // non-empty: a flat list of the matching bones instead of the tree
};
