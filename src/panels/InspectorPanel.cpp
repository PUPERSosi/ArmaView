#include "InspectorPanel.h"
#include "Lang.h"
#include "Material.h"
#include "Text.h"
#include "Validation.h"

#include <imgui.h>

#include <algorithm>
#include <format>

namespace {
const ImVec4 kOk = { 0.45f, 0.85f, 0.45f, 1 };
const ImVec4 kInfo = { 0.9f, 0.9f, 0.9f, 1 };
const ImVec4 kWarn = { 1.0f, 0.8f, 0.3f, 1 };
const ImVec4 kErr = { 1.0f, 0.4f, 0.4f, 1 };

ImVec4 StatusColor(TextureEntry::Status s)
{
    return s == TextureEntry::Status::Ok ? kOk : s == TextureEntry::Status::Procedural ? kInfo : s == TextureEntry::Status::Missing ? kWarn : kErr;
}
const char* StatusName(TextureEntry::Status s)
{
    return s == TextureEntry::Status::Ok ? "ok" : s == TextureEntry::Status::Procedural ? "procedural" : s == TextureEntry::Status::Missing ? "missing" : s == TextureEntry::Status::Loading ? "loading" : "error";
}
} // namespace

void InspectorPanel::Draw(const std::string& windowName, bool materialsMode)
{
    if (!ui::Begin(windowName.c_str()))
    {
        ImGui::End();
        return;
    }
    if (!s_.hasModel)
    {
        ui::TextDisabled("No model loaded.");
        ImGui::End();
        return;
    }
    const P3DLod& l = s_.model.lods[size_t(s_.lod)];
    const int sel = view_.HighlightedSelection();
    const int bone = host_.SelectedBone();
    std::string pointInfo;
    Kind k = materialsMode ? Kind::Section : what;
    if (k == Kind::Selection && (sel < 0 || sel >= int(l.selections.size()))) k = Kind::None;
    if (k == Kind::Bone && (bone < 0 || bone >= int(s_.skeleton.bones.size()))) k = Kind::None;
    if (k == Kind::Point && !view_.PickedPoint(pointInfo)) k = Kind::None;
    if (k == Kind::Seat && (crew_.selected < 0 || crew_.selected >= int(s_.attach.Seats().size()))) k = Kind::None;

    auto header = [&](const std::string& title) {
        ui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1), "%s", title.c_str());
        if (k != Kind::None && k != Kind::Section)
        {
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("LOD info").x - ImGui::GetStyle().FramePadding.x * 2);
            if (ui::SmallButton("LOD info")) what = Kind::None;
            if (ui::Hovered()) ui::SetTooltip("Show the current LOD instead");
        }
        ImGui::Separator();
    };
    switch (k)
    {
    case Kind::Selection: DrawSelection(header); break;
    case Kind::Bone: DrawBone(header); break;
    case Kind::Point:
        header("Picked point");
        ui::TextWrapped("%s", pointInfo.c_str());
        break;
    case Kind::Section:
        header(section >= 0 ? std::format("Section {}", section) : std::string("Section"));
        DrawSection();
        break;
    case Kind::Seat:
        header("Crew seat " + s_.attach.Seats()[size_t(crew_.selected)].title);
        crew_.DrawSeatInspector();
        break;
    default:
        header(std::format("LOD {} - {}", s_.lod, l.Name()));
        DrawLod();
        break;
    }
    ImGui::End();
}

void InspectorPanel::DrawSelection(const std::function<void(const std::string&)>& header)
{
    const P3DLod& l = s_.model.lods[size_t(s_.lod)];
    const P3DSelection& s = l.selections[size_t(view_.HighlightedSelection())];
    header("Selection " + s.name);
    ui::Text("Points %zu   partial weight %zu   faces %zu", s.pointCount, s.partialCount, s.faceCount);
    if (s.engineIgnored) ui::TextColored(kWarn, "Name starts with '-' or '.': the engine skips it");
    const Skeleton& sk = s_.skeleton;
    if (const int bone = sk.Find(s.name); bone >= 0)
    {
        ui::Text("Bone of %s", sk.name.c_str());
        ImGui::SameLine();
        if (ui::SmallButton("inspect bone")) host_.PickBone(bone);
    }
    else if (!sk.Empty())
        ui::TextDisabled("not a bone of %s", sk.name.c_str());
    std::vector<uint32_t> faces;
    for (uint32_t f = 0; f < s.faceBytes.size(); ++f)
        if (s.faceBytes[f]) faces.push_back(f);
    DrawShadingCheck(faces);
    if (ui::Button("Clear highlight")) view_.ClearHighlight();
}

void InspectorPanel::DrawBone(const std::function<void(const std::string&)>& header)
{
    const Skeleton& sk = s_.skeleton;
    const int bone = host_.SelectedBone();
    const Bone& b = sk.bones[size_t(bone)];
    const P3DLod& l = s_.model.lods[size_t(s_.lod)];
    header("Bone " + b.name);
    ui::Text("Parent");
    ImGui::SameLine();
    if (b.parent >= 0)
    {
        if (ui::SmallButton(b.parentName.c_str())) host_.PickBone(b.parent);
    }
    else
        ui::TextDisabled("- (root)");
    if (!b.children.empty())
    {
        ui::Text("Children");
        for (int c : b.children)
        {
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(sk.bones[size_t(c)].name.c_str()).x + 20 * host_.Dpi()) ImGui::NewLine();
            ImGui::PushID(c);
            if (ui::SmallButton(sk.bones[size_t(c)].name.c_str())) host_.PickBone(c);
            ImGui::PopID();
        }
    }
    ui::Text("Pivot %.3f %.3f %.3f", b.pivot.x, b.pivot.y, b.pivot.z);
    ImGui::SameLine();
    ui::TextDisabled(b.pivotSource == PivotSource::PivotsModel ? "(pivotsModel)" : b.pivotSource == PivotSource::SelectionCentroid ? "(centroid)" : "(none)");
    const auto& boneSel = host_.BoneSelection();
    const int sel = bone < int(boneSel.size()) ? boneSel[size_t(bone)] : -1;
    if (sel >= 0) ui::Text("Selection: %zu points in LOD %d", l.selections[size_t(sel)].pointCount, s_.lod);
    else ui::TextColored(kWarn, "No selection in LOD %d", s_.lod);
    const auto u = view_.UsageOfBone(bone);
    ui::Text("Moves %zu mesh points, %zu proxies", u.meshPoints, u.proxies.size());
    for (const auto& p : u.proxies) ui::BulletText("%s", p.c_str());
    int anims = 0;
    for (const auto& a : s_.cfgAnims) anims += a.bone == bone ? 1 : 0;
    if (anims) ui::Text("model.cfg animations on this bone: %d", anims);
    if (ui::Button("Focus camera")) host_.PickBone(bone, true);
    ImGui::SameLine();
    ui::Checkbox("Bone trail", &opt_.showTrail);
}

void InspectorPanel::DrawLod()
{
    const P3DLod& l = s_.model.lods[size_t(s_.lod)];
    if (statsOf_ != &l || statsFaces_ != l.faces.size())
    {
        statsOf_ = &l;
        statsFaces_ = l.faces.size();
        triangles_ = l.TriangleCount();
        mass_ = l.TotalMass();
        textures_.clear();
        materials_.clear();
        for (const auto& f : l.faces)
        {
            ++textures_[f.texture.empty() ? "<none>" : f.texture];
            ++materials_[f.material.empty() ? "<none>" : f.material];
        }
    }
    ui::Text("Points %zu   Normals %zu   Faces %zu   Triangles %zu", l.points.size(), l.normals.size(), l.faces.size(), triangles_);
    ui::Text("Sharp edges %zu   UV sets %zu", l.sharpEdges.size(), l.uvSets.size());
    if (!l.mass.empty()) ui::Text("Mass %.3f (%zu points)", mass_, l.mass.size());
    if (l.animationFrames) ui::Text("#Animation# frames %zu", l.animationFrames);
    for (const auto& s : l.skippedTaggs) ui::TextColored(kWarn, "Skipped TAGG %s", s.c_str());
    const glm::vec3 size = view_.BoundsSize();
    ui::Text("Bounds %.2f x %.2f x %.2f m", size.x, size.y, size.z);
    if (!l.properties.empty() && ui::CollapsingHeader(TF("Properties ({})", l.properties.size()).c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        for (const auto& p : l.properties) ui::BulletText("%s = %s", p.name.c_str(), p.value.c_str());
    if (ui::CollapsingHeader(TF("Textures ({}) / Materials ({})", textures_.size(), materials_.size()).c_str()))
    {
        for (const auto& [t, n] : textures_) ui::BulletText("%s  [%d faces]", t.c_str(), n);
        ImGui::Separator();
        for (const auto& [m, n] : materials_) ui::BulletText("%s  [%d faces]", m.c_str(), n);
    }
    const auto& proxies = view_.ProxyNames();
    if (!proxies.empty() && ui::CollapsingHeader(TF("Proxies ({})", proxies.size()).c_str()))
        for (const auto& p : proxies) ui::BulletText("%s", p.c_str());
    ImGui::Spacing();
    ui::TextDisabled("Click a selection, bone, point, section or crew seat to inspect it.");
}

// face texture, what the scene shows of it, the RVMAT (editor + stages), a texture preview
void InspectorPanel::DrawSection()
{
    const auto& sections = view_.Sections();
    if (section < 0 || section >= int(sections.size()))
    {
        ui::TextDisabled("Select a section in Materials to inspect its texture and RVMAT.");
        return;
    }
    const MeshSectionInfo& sec = sections[size_t(section)];
    ID3D11ShaderResourceView* preview = nullptr;
    std::string previewName;
    ui::SeparatorText("Face texture (stage 0)");
    if (sec.texture.empty())
        ui::TextDisabled("<none>");
    else
    {
        const TextureEntry& te = tex_.Get(sec.texture);
        ui::TextWrapped("%s", sec.texture.c_str());
        ui::TextColored(StatusColor(te.status), "%s: %s", StatusName(te.status), te.info.c_str());
        if (!te.resolved.empty()) ui::TextDisabled("%s", PathToUtf8(te.resolved).c_str());
        if (previewStage < 0)
        {
            preview = te.srv.Get();
            previewName = sec.texture;
        }
        if (ui::SmallButton("preview##face")) previewStage = -1;
    }
    ui::SeparatorText("Show");
    struct M { const char* label; ShadingMode m; } modes[] = { { "full", ShadingMode::Textured }, { "_co", ShadingMode::MapDiffuse }, { "_nohq", ShadingMode::MapNormal },
                                                                { "_smdi", ShadingMode::MapSpecular }, { "_as", ShadingMode::MapAmbientShadow },
                                                                { "macro", ShadingMode::MapMacro }, { "detail", ShadingMode::MapDetail } };
    for (const auto& m : modes)
    {
        ImGui::SameLine();
        if (ui::RadioButton(m.label, view_.Shading() == m.m)) view_.SetShading(m.m);
    }
    ui::SeparatorText("RVMAT");
    const RvMaterial* rv = mats_.Get(sec.material);
    if (!rv)
        ui::TextDisabled("<none> (default material)");
    else
    {
        ui::TextWrapped("%s", sec.material.c_str());
        if (!rv->loaded)
            ui::TextColored(kWarn, "%s: %s", rv->error.c_str(), PathToUtf8(rv->path).c_str());
        else if (ui::CollapsingHeader("Edit RVMAT", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (DrawRvmatEditor(sec.material, *rv)) view_.MaterialsEdited();
            rv = mats_.Get(sec.material); // may have been re-read after Save
        }
        if (rv && rv->loaded && ui::CollapsingHeader("Stages (read only)"))
        {
            ui::Text("PixelShaderID %s   VertexShaderID %s", rv->pixelShader.c_str(), rv->vertexShader.c_str());
            auto col = [](const char* n, const glm::vec4& c) { ui::Text("%-14s %.3f %.3f %.3f %.3f", n, c.r, c.g, c.b, c.a); };
            col("ambient", rv->ambient);
            col("diffuse", rv->diffuse);
            col("forcedDiffuse", rv->forcedDiffuse);
            col("emmisive", rv->emissive);
            col("specular", rv->specular);
            ui::Text("%-14s %.2f", "specularPower", rv->specularPower);
            if (ImGui::BeginTable("stages", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp))
            {
                ui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthFixed);
                ui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed);
                ui::TableSetupColumn("Texture");
                ui::TableSetupColumn("UV", ImGuiTableColumnFlags_WidthFixed);
                ui::TableSetupColumn("Status");
                ImGui::TableHeadersRow();
                for (int i = 0; i < int(rv->stages.size()); ++i)
                {
                    const RvStage& st = rv->stages[size_t(i)];
                    const TextureEntry& te = tex_.Get(st.texture);
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (ui::SelectableRaw(std::to_string(st.index).c_str(), previewStage == i, ImGuiSelectableFlags_SpanAllColumns)) previewStage = i;
                    ImGui::TableNextColumn();
                    ui::TextUnformatted(TexRoleName(st.role));
                    ImGui::TableNextColumn();
                    ui::TextUnformatted(st.texture.c_str());
                    if (ImGui::IsItemHovered() && !te.resolved.empty()) ui::SetTooltip("%s", PathToUtf8(te.resolved).c_str());
                    ImGui::TableNextColumn();
                    ui::Text("%s x%.3g", st.uvSourceName.c_str(), st.uv.aside.x);
                    ImGui::TableNextColumn();
                    ui::TextColored(StatusColor(te.status), "%s", te.info.c_str());
                    ImGui::PopID();
                    if (previewStage == i)
                    {
                        preview = te.srv.Get();
                        previewName = st.texture;
                    }
                }
                ImGui::EndTable();
            }
        }
    }
    if (preview)
    {
        ui::SeparatorText("Preview");
        ui::TextDisabled("%s", previewName.c_str());
        const float sz = std::min(ImGui::GetContentRegionAvail().x, 320.0f * host_.Dpi());
        ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(preview)), ImVec2(sz, sz));
    }
}

// one RVMAT: edits live in the MaterialSet until Save (text RVMAT, values replaced in place, .bak kept) or Revert
bool InspectorPanel::DrawRvmatEditor(const std::string& name, const RvMaterial& original)
{
    const float dpi = host_.Dpi();
    const auto& edits = mats_.edits;
    auto it = edits.find(ToLower(name));
    RvMaterial cur = it != edits.end() ? it->second : original;
    bool changed = false;
    const bool edited = it != edits.end();
    if (original.rapified) ui::TextColored(kWarn, "Binary (rapified) RVMAT: edits are previewed only, Save is not available.");
    if (original.includes) ui::TextColored(kWarn, "Uses #include: entries from other files are not written.");
    auto combo = [&](const char* label, std::string& v, const std::vector<const char*>& list) {
        bool ch = false;
        ImGui::SetNextItemWidth(200 * dpi);
        if (ui::BeginCombo(label, v.c_str()))
        {
            for (const char* item : list)
                if (ui::SelectableRaw(item, IEquals(v, item)))
                {
                    v = item;
                    ch = true;
                }
            ImGui::EndCombo();
        }
        return ch;
    };
    changed |= combo("PixelShaderID", cur.pixelShader, MaterialSet::PixelShaders());
    ImGui::SameLine();
    changed |= combo("VertexShaderID", cur.vertexShader, MaterialSet::VertexShaders());
    auto color = [&](const char* label, glm::vec4& c) {
        ImGui::SetNextItemWidth(260 * dpi);
        return ui::ColorEdit4(label, &c.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_AlphaBar);
    };
    changed |= color("ambient", cur.ambient);
    changed |= color("diffuse", cur.diffuse);
    changed |= color("forcedDiffuse", cur.forcedDiffuse);
    changed |= color("emmisive", cur.emissive);
    changed |= color("specular", cur.specular);
    ImGui::SetNextItemWidth(260 * dpi);
    changed |= ui::DragFloat("specularPower", &cur.specularPower, 0.5f, 0.0f, 1000.0f, "%.1f");
    for (size_t i = 0; i < cur.stages.size(); ++i)
    {
        RvStage& st = cur.stages[i];
        ImGui::PushID(int(i));
        ui::SeparatorText(TF("Stage{}  {}", st.index, TexRoleName(st.role)).c_str());
        const TextureEntry& te = tex_.Get(st.texture);
        if (te.srv)
        {
            ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(te.srv.Get())), ImVec2(44 * dpi, 44 * dpi));
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(te.srv.Get())), ImVec2(256 * dpi, 256 * dpi));
                ui::TextUnformatted(te.info.c_str());
                ImGui::EndTooltip();
            }
            ImGui::SameLine();
        }
        ImGui::BeginGroup();
        char buf[260];
        strncpy_s(buf, st.texture.c_str(), _TRUNCATE);
        ImGui::SetNextItemWidth(-90 * dpi);
        bool stageChanged = false;
        if (ui::InputText("##tex", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            st.texture = buf;
            stageChanged = true;
        }
        ImGui::SameLine();
        if (ui::Button("..."))
            if (std::string p = view_.PickTexture(st.texture); !p.empty())
            {
                st.texture = p;
                stageChanged = true;
            }
        ImGui::SetNextItemWidth(110 * dpi);
        if (ui::BeginCombo("uvSource", st.uvSourceName.c_str()))
        {
            for (const char* src : MaterialSet::UvSources())
                if (ui::SelectableRaw(src, IEquals(st.uvSourceName, src)))
                {
                    st.uvSourceName = src;
                    st.uv.source = IEquals(src, "tex1") ? 1 : 0;
                    stageChanged = true;
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120 * dpi);
        stageChanged |= ui::DragFloat2("aside", &st.uv.aside.x, 0.01f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120 * dpi);
        stageChanged |= ui::DragFloat2("up", &st.uv.up.x, 0.01f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120 * dpi);
        stageChanged |= ui::DragFloat2("pos", &st.uv.pos.x, 0.01f);
        ui::TextColored(te.status == TextureEntry::Status::Ok ? kOk : kWarn, "%s", te.info.c_str());
        ImGui::EndGroup();
        if (stageChanged || changed) MaterialSet::UpdateRole(st);
        changed |= stageChanged;
        ImGui::PopID();
    }
    if (changed) mats_.Edit(name, cur);
    ImGui::Separator();
    if (edited || changed)
    {
        if (ui::Button("Revert##rvmat"))
        {
            mats_.Revert(name);
            changed = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(original.rapified);
        if (ui::Button("Save (.bak kept)") && mats_.Save(name, original)) changed = true; // .bak kept, re-read from disk
        ImGui::EndDisabled();
        ImGui::SameLine();
        ui::TextDisabled("A/B: M (now %s)", mats_.showOriginal ? "original" : "edited");
    }
    if (!mats_.saveMsg.empty()) ui::TextWrapped("%s", mats_.saveMsg.c_str());
    return changed;
}

// orientation of the normals against the faces, mirrored / degenerate UVs, materials and textures of some faces
void InspectorPanel::DrawShadingCheck(const std::vector<uint32_t>& faces)
{
    const P3DLod& l = s_.model.lods[size_t(s_.lod)];
    const FaceDiagInfo& fi = view_.FaceInfo();
    int flippedAll = 0, flippedSome = 0, mirrored = 0, degenerate = 0;
    std::map<std::string, int> mats, texs;
    for (uint32_t f : faces)
    {
        if (f >= l.faces.size()) continue;
        const uint8_t fc = fi.flippedCorners[f];
        if (fc >= l.faces[f].count) ++flippedAll;
        else if (fc) ++flippedSome;
        mirrored += fi.mirrored[f];
        degenerate += fi.degenerateUV[f];
        ++mats[l.faces[f].material.empty() ? "<none>" : ToLower(l.faces[f].material)];
        ++texs[l.faces[f].texture.empty() ? "<none>" : ToLower(l.faces[f].texture)];
    }
    ui::SeparatorText("Shading check");
    ui::TextColored(flippedAll || flippedSome ? kErr : kOk, "Normals against the face: %d faces all corners, %d some", flippedAll, flippedSome);
    if (ui::Hovered())
        ui::SetTooltip("Vertex normal vs the face plane (orientation of the LOD majority, %s).\nAll corners: the face is lit from behind - dark under the same light.",
                       fi.windingSign > 0 ? "normals follow the winding" : "normals opposite to the winding");
    ui::TextColored(mirrored ? kWarn : kOk, "Mirrored UV: %d faces", mirrored);
    if (ui::Hovered())
        ui::SetTooltip("UV handedness opposite to the LOD majority. The viewer builds the tangent frame per pixel from UV derivatives,\n"
                       "so mirrored islands are lit correctly here; the game uses binarize tangents (also handedness-aware).");
    if (degenerate) ui::TextColored(kWarn, "Zero UV area: %d faces (no tangent frame: normal map undefined)", degenerate);
    ui::Text("RVMAT:");
    for (const auto& [m, c] : mats) ui::BulletText("%s  [%d]", m.c_str(), c);
    ui::Text("Texture:");
    for (const auto& [t, c] : texs) ui::BulletText("%s  [%d]", t.c_str(), c);
}
