#pragma once
// Layers window: the animation layers project (core LayersJob): base clip, clip operations, correction layers with
// masks / keys / weight curves, preview (bake) with A/B, bake to a new RTM_0101, the batch over a folder with its
// progress, the Blender bridge. This window only edits the project and sends commands.
#include "LayersJob.h"
#include "Session.h"
#include "ViewHost.h"

#include <string>
#include <vector>

// What the Layers window asks of the viewer (implemented by App): the preview changes what plays and is drawn.
class ILayersView
{
public:
    virtual ~ILayersView() = default;
    virtual bool LayersPreview() = 0;                             // bake + show the result (A/B with the base)
    virtual void LayersStopPreview() = 0;
    virtual void ShowMaskHeat(std::vector<float> boneWeights) = 0; // the mask of a layer as a heat map on the mesh
    virtual bool LayersOpenInBlender() = 0;
};

class LayersPanel
{
public:
    LayersPanel(Session& session, LayersJob& job, AnimViewOptions& options, IViewHost& host, ILayersView& view)
        : s_(session), job_(job), opt_(options), host_(host), view_(view) {}
    void Draw(const std::string& windowName, const std::string& dockWith);

    int selLayer = -1; // the correction layer being edited

private:
    void DrawProject();
    void DrawClipOps();
    void DrawLayerList();
    bool DrawSelectedLayer(); // false: the layer was deleted
    void DrawActions();
    void DrawBatch();
    void DrawBatchProgress(); // shown while the batch runs, also with its section closed

    Session& s_;
    LayersJob& job_;
    AnimViewOptions& opt_;
    IViewHost& host_;
    ILayersView& view_;
};
