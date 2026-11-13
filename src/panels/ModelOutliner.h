#pragma once
// Outliner of the Model / Vehicle / Materials / weapon modes: the model and its model.cfg, the LODs (eye: hide the
// mesh of the current LOD, or draw another LOD over it), the selections of the current LOD (click: highlight its
// weights; eye: hide / isolate), picked faces and viewer-only face sets.
#include "InspectorPanel.h"
#include "Session.h"
#include "ViewHost.h"
#include "Visibility.h"

#include <cstdint>
#include <string>
#include <vector>

// What the outliner asks of the viewer (implemented by App).
class IOutlinerView
{
public:
    virtual ~IOutlinerView() = default;
    virtual ModelVisibility& Visibility() = 0;
    virtual void VisibilityChanged() = 0;              // saved + geometry again
    virtual void ShowAllHidden() = 0;
    virtual void HideSelected() = 0;
    virtual void IsolateSelected() = 0;
    virtual void RebuildLodOverlay() = 0;              // other LODs drawn over the current one
    virtual std::vector<uint32_t>& PickedFaces() = 0;  // Ctrl+click in the scene
    virtual const std::vector<uint8_t>& FaceHidden() const = 0;
    virtual int HighlightedSelection() const = 0;
    virtual void HighlightSelection(int selection) = 0; // its weights on the mesh, the Inspector shows it (-1: none)
};

// a small eye toggle (icon font when there is one)
bool EyeButton(const char* id, bool visible, const char* tip, bool icons, float dpi);

class ModelOutliner
{
public:
    ModelOutliner(Session& session, InspectorPanel& inspector, const bool& icons, IViewHost& host, IOutlinerView& view)
        : s_(session), inspector_(inspector), icons_(icons), host_(host), view_(view) {}
    void Draw(); // inside the Outliner window

private:
    void DrawLods();
    void DrawSelections();
    void DrawVisibility();

    Session& s_;
    InspectorPanel& inspector_;
    const bool& icons_;
    IViewHost& host_;
    IOutlinerView& view_;
    std::string filter_;
    char setName_[64] = {};
};
