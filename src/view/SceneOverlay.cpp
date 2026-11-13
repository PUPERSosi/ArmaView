#include "SceneOverlay.h"

#include <imgui.h>

#include <algorithm>
#include <iterator>

void SceneOverlay::Draw()
{
    const float dpi_ = host_.Dpi();
    float sceneX_ = 0, sceneY_ = 0, sceneW_ = 1, sceneH_ = 1;
    viewport_.SceneRect(sceneX_, sceneY_, sceneW_, sceneH_);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 origin = ImGui::GetMainViewport()->Pos;
    float ox = origin.x + sceneX_, oy = origin.y + sceneY_;

    // Axis triad (bottom-left of the scene): +X red, +Y green, +Z blue. Front of an MLOD model is -Z.
    if (display_.showAxes)
    {
        glm::mat3 v(camera_.View());
        float len = 38.0f * dpi_;
        ImVec2 c(ox + 55.0f * dpi_, oy + sceneH_ - 55.0f * dpi_);
        struct Ax { glm::vec3 d; ImU32 col; const char* name; float depth; } axes[3] = {
            // Blender labels: a3ob imports P3D (x, y, z) as Blender (x, z, y)
            { v * glm::vec3(1, 0, 0), IM_COL32(235, 70, 70, 255), "+X", 0 },
            { v * glm::vec3(0, 1, 0), IM_COL32(80, 220, 80, 255), display_.axisLabelsBlender ? "+Z" : "+Y", 0 },
            { v * glm::vec3(0, 0, 1), IM_COL32(80, 130, 255, 255), display_.axisLabelsBlender ? "+Y" : "+Z", 0 },
        };
        for (auto& a : axes) a.depth = a.d.z;
        if (!light_.headlight)
        {
            glm::vec3 sd = v * (-lit_.lightDir);
            ImVec2 e(c.x + sd.x * len * 1.15f, c.y - sd.y * len * 1.15f);
            ImU32 col = lit_.moon ? IM_COL32(150, 170, 255, 255) : IM_COL32(255, 210, 60, 255);
            dl->AddLine(c, e, col, 1.5f * dpi_);
            dl->AddCircleFilled(e, 4.0f * dpi_, col);
            dl->AddText(ImVec2(e.x + 5, e.y - 8 * dpi_), col, lit_.moon ? "moon" : "sun");
        }
        std::sort(std::begin(axes), std::end(axes), [](const Ax& a, const Ax& b) { return a.depth > b.depth; });
        for (const auto& a : axes)
        {
            ImVec2 e(c.x + a.d.x * len, c.y - a.d.y * len);
            dl->AddLine(c, e, a.col, 2.5f * dpi_);
            dl->AddText(ImVec2(e.x + 3, e.y - 8 * dpi_), a.col, a.name);
        }
    }

    if (s_.hasModel)
    {
        PointOverlay::View pv;
        pv.viewProj = viewport_.ViewProj();
        pv.cameraRight = camera_.Right();
        pv.x = ox;
        pv.y = oy;
        pv.w = sceneW_;
        pv.h = sceneH_;
        pv.dpi = dpi_;
        if (points_.WantsDepth() && renderer_.HasDepth())
            pv.depthAt = [this, origin](float x, float y) { return renderer_.DepthAt(int(x - origin.x), int(y - origin.y)); };
        points_.Draw(dl, pv, s_.model, s_.lod, scene_.pointMats);
    }

    // Proxy / crew stubs (no drawable model): name and reason
    if (s_.hasModel)
        for (const auto& l : s_.attach.StubLabels())
        {
            if (!l.crew && !crewWindow_) continue; // names of binarized / missing proxies: only while the Crew window is open
            glm::vec4 clip = viewport_.ViewProj() * glm::vec4(l.pos, 1.0f);
            if (clip.w <= 0.0f) continue;
            float x = ox + (clip.x / clip.w * 0.5f + 0.5f) * sceneW_, y = oy + (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * sceneH_;
            if (x < ox || y < oy || x > ox + sceneW_ || y > oy + sceneH_) continue;
            ImVec2 ts = ImGui::CalcTextSize(l.text.c_str()), at(x - ts.x * 0.5f, y - ts.y);
            dl->AddRectFilled(ImVec2(at.x - 3, at.y - 1), ImVec2(at.x + ts.x + 3, at.y + ts.y + 1), IM_COL32(15, 15, 18, 170), 3.0f);
            dl->AddText(at, l.crew ? IM_COL32(255, 170, 80, 255) : IM_COL32(150, 200, 255, 255), l.text.c_str());
        }

    // Selected bone label
    if (scene_.selectedBone >= 0 && scene_.selectedBone < int(s_.skeleton.bones.size()) && display_.showSkeleton)
    {
        const Bone& b = s_.skeleton.bones[scene_.selectedBone];
        if (b.pivotSource != PivotSource::None)
        {
            glm::vec4 clip = viewport_.ViewProj() * glm::vec4(scene_.BonePosition(scene_.selectedBone), 1.0f);
            if (clip.w > 0.0f)
            {
                float x = ox + (clip.x / clip.w * 0.5f + 0.5f) * sceneW_;
                float y = oy + (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * sceneH_;
                dl->AddText(ImVec2(x + 8, y - 8), IM_COL32(255, 120, 255, 255), b.name.c_str());
            }
        }
    }
}
