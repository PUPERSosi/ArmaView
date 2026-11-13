#pragma once
// Inspector window: what was clicked last - a selection, a bone, a picked point, a section (its texture, RVMAT with
// the editor, stages), a crew seat - or the current LOD's summary. Shading check of the faces of a selection.
#include "CrewPanel.h"
#include "MaterialSet.h"
#include "MaterialsPanel.h"
#include "Renderer.h"
#include "Session.h"
#include "ValidationPanel.h"
#include "ViewHost.h"

#include <glm/glm.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

class TextureCache;
struct FaceDiagInfo;

// What the Inspector asks of the viewer (implemented by App).
class IInspectorView
{
public:
    virtual ~IInspectorView() = default;
    virtual int HighlightedSelection() const = 0;           // the selection highlighted on the mesh (-1 none)
    virtual void ClearHighlight() = 0;
    virtual IValidationView::BoneUsage UsageOfBone(int bone) const = 0;
    virtual bool PickedPoint(std::string& info) const = 0;  // the point picked in the scene, described
    virtual glm::vec3 BoundsSize() const = 0;               // of the posed model
    virtual const std::vector<std::string>& ProxyNames() const = 0; // of the current LOD
    virtual const std::vector<MeshSectionInfo>& Sections() const = 0;
    virtual const FaceDiagInfo& FaceInfo() = 0;             // per-face shading analysis of the current LOD
    virtual ShadingMode Shading() const = 0;
    virtual void SetShading(ShadingMode m) = 0;
    virtual void MaterialsEdited() = 0;                     // an RVMAT changed: the drawn sections again
    virtual std::string PickTexture(const std::string& current) = 0; // file dialog -> Arma path ("" cancelled)
};

class InspectorPanel
{
public:
    enum class Kind { None, Selection, Bone, Point, Section, Seat };
    InspectorPanel(Session& session, MaterialSet& mats, TextureCache& textures, CrewPanel& crew, AnimViewOptions& options, IViewHost& host,
                   IInspectorView& view)
        : s_(session), mats_(mats), tex_(textures), crew_(crew), opt_(options), host_(host), view_(view) {}
    void Draw(const std::string& windowName, bool materialsMode);
    void DrawShadingCheck(const std::vector<uint32_t>& faces); // also in the Model outliner
    void LodChanged() { statsOf_ = nullptr; }

    Kind what = Kind::None;  // what the Inspector shows (last thing clicked)
    int section = -1;        // the section inspected (Materials)
    int previewStage = -1;   // its texture shown: -1 the face texture, else an RVMAT stage

private:
    void DrawSelection(const std::function<void(const std::string&)>& header);
    void DrawBone(const std::function<void(const std::string&)>& header);
    void DrawLod();
    void DrawSection();
    bool DrawRvmatEditor(const std::string& name, const RvMaterial& original); // true: changed

    Session& s_;
    MaterialSet& mats_;
    TextureCache& tex_;
    CrewPanel& crew_;
    AnimViewOptions& opt_;
    IViewHost& host_;
    IInspectorView& view_;
    // LOD summary, counted once per LOD
    const P3DLod* statsOf_ = nullptr;
    size_t statsFaces_ = 0, triangles_ = 0;
    float mass_ = 0;
    std::map<std::string, int> textures_, materials_;
};
