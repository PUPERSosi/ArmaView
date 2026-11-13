#pragma once
// Materials: config skins (hiddenSelectionsTextures) and their sections, Damage state (mat[] / tex[]), the RVMAT shader
// survey, and the sections of the current LOD (click: the section Inspector with the RVMAT editor).
#include "Session.h"
#include "ViewHost.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// One drawn section of the current LOD: texture + material, its faces and its range in the vertex buffer
struct MeshSectionInfo
{
    std::string texture, material;
    size_t faces = 0;
    uint32_t first = 0, count = 0;
};

// What the Materials panel reads of the view besides the document (implemented by App).
class IMaterialsView
{
public:
    virtual ~IMaterialsView() = default;
    virtual const std::vector<MeshSectionInfo>& Sections() const = 0;
    virtual size_t CachedTextures() const = 0;
    virtual size_t DamageMaterials() const = 0; // RVMATs with damaged / destroyed variants (class Damage mat[])
    virtual size_t DamageTextures() const = 0;  // textures with a damaged variant (tex[])
    virtual int SelectedSection() const = 0;
    virtual void SelectSection(int section) = 0; // also shows it in the Inspector
};

class MaterialsPanel
{
public:
    MaterialsPanel(Session& session, IViewHost& host, IMaterialsView& view) : session_(session), host_(host), view_(view) {}
    void Draw(const std::string& windowName, const std::function<void()>& shaderSurvey);

    int damageState = 0; // 0 intact, 1 damaged, 2 destroyed (which Damage variants are drawn)

private:
    Session& session_;
    IViewHost& host_;
    IMaterialsView& view_;
};
