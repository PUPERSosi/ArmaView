#include "SceneInput.h"
#include "Lang.h"
#include "Text.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace {
std::string Slug(const char* s)
{
    std::string out;
    for (const char* p = s; *p; ++p)
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (std::isalnum(c)) out += char(std::tolower(c));
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}
} // namespace

void SceneInput::Bind(int chord, const char* group, const char* description, std::function<void()> run, bool needsModel)
{
    if (!run) // handled before the keyboard capture check (F1, the palette's own key): listed only
    {
        hotkeys_.push_back({ ImGuiKeyChord(chord), nullptr, group, description, nullptr, needsModel });
        return;
    }
    std::string name = "key." + Slug(group) + "." + Slug(description);
    while (commands_.Find(name)) name += "_"; // two keys for one action (Left / Comma)
    commands_.Add(name, "", description, [run, needsModel, this](std::istringstream&) -> std::string {
        if (needsModel && !s_.hasModel) return "needs a model";
        run();
        return "ok";
    });
    hotkeys_.push_back({ ImGuiKeyChord(chord), nullptr, group, description,
                         [this, name] {
                             if (const auto* e = commands_.Find(name))
                             {
                                 std::istringstream none;
                                 e->run(none);
                             }
                         },
                         needsModel });
}

void SceneInput::Info(const char* keys, const char* group, const char* description)
{
    hotkeys_.push_back({ ImGuiKey_None, keys, group, description, nullptr, false });
}

void SceneInput::PublishKeys()
{
    std::vector<std::pair<std::string, std::string>> keys;
    for (const auto& h : hotkeys_)
        keys.emplace_back(h.description, h.keys ? std::string(h.keys) : h.chord != ImGuiKey_None ? ChordName(h.chord) : std::string());
    SetCommandKeys(std::move(keys));
}

// ImGui's IO of this frame as an InputState (the ui-script and raw Win32 keys feed ImGui's IO, so they arrive here too)
InputState SceneInput::ReadInput(const IViewport& viewport)
{
    const ImGuiIO& io = ImGui::GetIO();
    InputState in;
    const ImVec2 origin = ImGui::GetMainViewport()->Pos;
    in.originX = origin.x;
    in.originY = origin.y;
    in.mouseX = io.MousePos.x;
    in.mouseY = io.MousePos.y;
    in.mouseDX = io.MouseDelta.x;
    in.mouseDY = io.MouseDelta.y;
    in.wheel = io.MouseWheel;
    for (int b = 0; b < 3; ++b)
    {
        in.down[size_t(b)] = ImGui::IsMouseDown(b);
        in.pressed[size_t(b)] = ImGui::IsMouseClicked(b);
        in.released[size_t(b)] = ImGui::IsMouseReleased(b);
    }
    in.ctrl = io.KeyCtrl;
    in.shift = io.KeyShift;
    in.alt = io.KeyAlt;
    for (int k = 0; k < 26; ++k) in.letters[size_t(k)] = ImGui::IsKeyDown(ImGuiKey(ImGuiKey_A + k));
    float sx = 0, sy = 0, sw = 1, sh = 1;
    viewport.SceneRect(sx, sy, sw, sh);
    const float mx = in.mouseX - in.originX, my = in.mouseY - in.originY;
    in.overScene = !io.WantCaptureMouse && mx >= sx && my >= sy && mx < sx + sw && my < sy + sh;
    in.textInput = io.WantTextInput;
    return in;
}

void SceneInput::Frame()
{
    const InputState in = ReadInput(viewport_);
    SceneRect rect;
    viewport_.SceneRect(rect.x, rect.y, rect.w, rect.h);
    interaction_.Mouse(in, rect, viewport_.ViewProj());

    // hotkeys (ImGui key chords): nothing fires while a text field has the keyboard, except the palette's own toggle and F1
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false) && !io.WantTextInput) helpOpen_ = !helpOpen_;
    if ((!io.WantTextInput || palette_.open) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P, ImGuiInputFlags_None, ImGuiKeyOwner_Any))
    {
        palette_.open = !palette_.open;
        palette_.focus = palette_.open;
    }
    if (palette_.open || io.WantTextInput) return;
    // a focused panel keeps its navigation keys (arrows, space, page up/down ...); everything else works everywhere
    auto navKey = [](ImGuiKeyChord c) {
        const ImGuiKey k = ImGuiKey(c & ~ImGuiMod_Mask_);
        return !(c & ImGuiMod_Mask_) && (k == ImGuiKey_Space || k == ImGuiKey_LeftArrow || k == ImGuiKey_RightArrow || k == ImGuiKey_UpArrow ||
                                         k == ImGuiKey_DownArrow || k == ImGuiKey_PageUp || k == ImGuiKey_PageDown || k == ImGuiKey_Home ||
                                         k == ImGuiKey_End || k == ImGuiKey_Enter || k == ImGuiKey_Tab || k == ImGuiKey_Escape);
    };
    auto letter = [](ImGuiKeyChord c) {
        const ImGuiKey k = ImGuiKey(c & ~ImGuiMod_Mask_);
        return !(c & ImGuiMod_Ctrl) && k >= ImGuiKey_A && k <= ImGuiKey_Z;
    };
    for (const auto& h : hotkeys_)
        if (h.run && h.chord != ImGuiKey_None && (!h.needsModel || s_.hasModel) && (!io.WantCaptureKeyboard || !navKey(h.chord)) &&
            !(control_.on && letter(h.chord)) && ImGui::IsKeyChordPressed(h.chord, ImGuiInputFlags_None, ImGuiKeyOwner_Any))
            h.run();
}

// control mode: keys -> actions of the current state, the character walks, the camera and the grid follow
bool SceneInput::Control(float dt, MovesControl& control, MovesSim& sim, Playback& playback, IMovesView& moves)
{
    moves_ = &moves;
    return interaction_.Control(ReadInput(viewport_), dt, control, sim, playback, s_.moves);
}

// ---- ISceneInteractionHost

void SceneInput::ToggleFace(const glm::mat4& viewProj, float nx, float ny)
{
    uint32_t face = 0;
    if (!scene_.PickFace(viewProj, nx, ny, face)) return;
    auto it = std::find(scene_.pickFaces.begin(), scene_.pickFaces.end(), face);
    if (it == scene_.pickFaces.end()) scene_.pickFaces.push_back(face);
    else scene_.pickFaces.erase(it);
    scene_.RebuildLodGeometry();
}

InspectTarget SceneInput::Inspected() const
{
    switch (inspector_.what)
    {
    case InspectorPanel::Kind::None: return InspectTarget::None;
    case InspectorPanel::Kind::Point: return InspectTarget::Point;
    case InspectorPanel::Kind::Bone: return InspectTarget::Bone;
    default: return InspectTarget::Other;
    }
}

void SceneInput::Inspect(InspectTarget what)
{
    if (what == InspectTarget::Point) inspector_.what = InspectorPanel::Kind::Point;
    else if (what == InspectTarget::Bone) inspector_.what = InspectorPanel::Kind::Bone;
    else if (what == InspectTarget::None) inspector_.what = InspectorPanel::Kind::None;
}

void SceneInput::SetSelectedBone(int bone)
{
    if (bone < 0)
    {
        scene_.selectedBone = scene_.selection = -1;
        scene_.highlight = SceneView::Highlight::None;
    }
    else
    {
        scene_.selectedBone = bone;
        scene_.selection = bone < int(scene_.boneSelection.size()) ? scene_.boneSelection[size_t(bone)] : -1;
        scene_.highlight = scene_.selection >= 0 ? SceneView::Highlight::Selection : SceneView::Highlight::None;
    }
    scene_.RebuildLodGeometry();
    scene_.RebuildSkeletonLines();
}

// the points the bone moves (posed if animating) and its pivot
void SceneInput::BoneExtent(int bone, glm::vec3& mn, glm::vec3& mx) const
{
    if (s_.hasModel && bone < int(scene_.boneSelection.size()) && scene_.boneSelection[size_t(bone)] >= 0)
    {
        const P3DLod& l = s_.model.lods[s_.lod];
        const P3DSelection& s = l.selections[size_t(scene_.boneSelection[size_t(bone)])];
        for (size_t p = 0; p < s.pointBytes.size() && p < l.points.size(); ++p)
            if (s.pointBytes[p])
            {
                glm::vec3 q = l.points[p].pos;
                if (p < scene_.pointMats.size()) q = glm::vec3(scene_.pointMats[p] * glm::vec4(q, 1.0f));
                mn = glm::min(mn, q);
                mx = glm::max(mx, q);
            }
    }
    const glm::vec3 pivot = scene_.BonePosition(bone);
    mn = glm::min(mn, pivot);
    mx = glm::max(mx, pivot);
}

void SceneInput::GridFollow(const glm::vec2& cell)
{
    if (cell == scene_.gridCell) return;
    scene_.gridCell = cell;
    scene_.RebuildGrid();
}
