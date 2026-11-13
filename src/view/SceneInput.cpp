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

void SceneInput::Frame()
{
    ImGuiIO& io = ImGui::GetIO();
    float sx = 0, sy = 0, sw = 1, sh = 1;
    viewport_.SceneRect(sx, sy, sw, sh);
    const ImVec2 origin = ImGui::GetMainViewport()->Pos;
    const float mx = io.MousePos.x - origin.x, my = io.MousePos.y - origin.y;
    const bool overScene = !io.WantCaptureMouse && mx >= sx && my >= sy && mx < sx + sw && my < sy + sh;

    if (dragButton_ < 0 && overScene)
        for (int b = 0; b < 3; ++b)
            if (ImGui::IsMouseClicked(b))
            {
                dragButton_ = b;
                pressX_ = io.MousePos.x;
                pressY_ = io.MousePos.y;
                break;
            }
    if (dragButton_ >= 0)
    {
        if (!ImGui::IsMouseDown(dragButton_))
        {
            // left click without dragging: pick a point, or with Ctrl a face
            if (dragButton_ == 0 && s_.hasModel && std::hypot(io.MousePos.x - pressX_, io.MousePos.y - pressY_) < 4.0f)
            {
                uint32_t face = 0;
                if (io.KeyCtrl)
                {
                    const float nx = (mx - sx) / sw * 2.0f - 1.0f, ny = 1.0f - (my - sy) / sh * 2.0f;
                    if (scene_.PickFace(viewport_.ViewProj(), nx, ny, face))
                    {
                        auto it = std::find(scene_.pickFaces.begin(), scene_.pickFaces.end(), face);
                        if (it == scene_.pickFaces.end()) scene_.pickFaces.push_back(face);
                        else scene_.pickFaces.erase(it);
                        scene_.RebuildLodGeometry();
                    }
                }
                else if (points_.Click(io.MousePos.x, io.MousePos.y, s_.model, s_.lod))
                    inspector_.what = InspectorPanel::Kind::Point;
            }
            dragButton_ = -1;
        }
        else if (dragButton_ == 0)
            camera_.Rotate(io.MouseDelta.x, io.MouseDelta.y);
        else
            camera_.Pan(io.MouseDelta.x, io.MouseDelta.y);
    }
    if (overScene && io.MouseWheel != 0.0f) camera_.Zoom(io.MouseWheel);

    // nothing fires while a text field has the keyboard, except the palette's own toggle and F1
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
    if (!control.on || s_.moves.Empty()) return false;
    if (!sim.drive) moves.SetMovesDrive(true);
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return false;
    MovesControl::KeyState keys;
    keys.shift = io.KeyShift;
    keys.ctrl = io.KeyCtrl;
    for (int k = 0; k < 26; ++k) keys.letter[size_t(k)] = ImGui::IsKeyDown(ImGuiKey(ImGuiKey_A + k));
    if (const std::string act = control.KeyAction(keys, s_.moves, sim.player.Current()); !act.empty())
        moves.MovesPlay(act, false, false); // a key: never into a death / unconscious state
    control.Move(sim, s_.moves, playback.time, dt, playback.speed * (playback.playing ? 1.0f : 0.0f), ImGui::IsKeyDown(ImGuiKey_Q), ImGui::IsKeyDown(ImGuiKey_E));
    // camera follows, grid around the character
    camera_.target = glm::mix(camera_.target, control.rootPos + glm::vec3(0, 1.0f, 0), std::min(1.0f, dt * 4.0f));
    const glm::vec2 cell(std::round(control.rootPos.x), std::round(control.rootPos.z));
    if (cell != scene_.gridCell) { scene_.gridCell = cell; scene_.RebuildGrid(); }
    return true; // the pose follows
}

// camera on a bone (the points it moves, posed), bone selection with its highlight, the standard views
void SceneInput::FocusBone(int bone)
{
    if (bone < 0 || bone >= int(s_.skeleton.bones.size())) return;
    // Frame the points the bone moves (posed if animating); fall back to the pivot.
    glm::vec3 mn(1e30f), mx(-1e30f);
    if (s_.hasModel && bone < int(scene_.boneSelection.size()) && scene_.boneSelection[bone] >= 0)
    {
        const P3DLod& l = s_.model.lods[s_.lod];
        const P3DSelection& s = l.selections[scene_.boneSelection[bone]];
        for (size_t p = 0; p < s.pointBytes.size() && p < l.points.size(); ++p)
            if (s.pointBytes[p])
            {
                glm::vec3 q = l.points[p].pos;
                if (p < scene_.pointMats.size()) q = glm::vec3(scene_.pointMats[p] * glm::vec4(q, 1.0f));
                mn = glm::min(mn, q);
                mx = glm::max(mx, q);
            }
    }
    glm::vec3 pivot = scene_.BonePosition(bone);
    mn = glm::min(mn, pivot);
    mx = glm::max(mx, pivot);
    glm::vec3 pad(0.12f);
    camera_.Frame(mn - pad, mx + pad);
}

void SceneInput::SelectBone(int bone, bool focus, bool materialsMode)
{
    if (focus && bone >= 0)
    {
        scene_.selectedBone = -1; // always (re)select when focusing
        FocusBone(bone);
    }
    if (bone == scene_.selectedBone || bone < 0)
    {
        scene_.selectedBone = scene_.selection = -1;
        scene_.highlight = SceneView::Highlight::None;
        if (inspector_.what == InspectorPanel::Kind::Bone) inspector_.what = InspectorPanel::Kind::None;
    }
    else
    {
        if (!materialsMode) inspector_.what = InspectorPanel::Kind::Bone;
        scene_.selectedBone = bone;
        scene_.selection = bone < int(scene_.boneSelection.size()) ? scene_.boneSelection[bone] : -1;
        scene_.highlight = scene_.selection >= 0 ? SceneView::Highlight::Selection : SceneView::Highlight::None;
    }
    scene_.RebuildLodGeometry();
    scene_.RebuildSkeletonLines();
}

void SceneInput::SetView(float yaw, float pitch)
{
    camera_.yaw = yaw;
    camera_.pitch = pitch;
}
