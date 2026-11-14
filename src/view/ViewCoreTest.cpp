#include "ViewCoreTest.h"
#include "ArmaConfig.h"
#include "AppMode.h"
#include "FileSystem.h"
#include "IStatesGraphHost.h"
#include "Log.h"
#include "Moves.h"
#include "MovesSim.h"
#include "Playback.h"
#include "PointOverlaySettings.h"
#include "RTM.h"
#include "Retarget.h" // WriteRtm0101
#include "SceneInteraction.h"
#include "Session.h"
#include "Settings.h"

#include <cmath>
#include <format>
#include <fstream>

#include "ViewCoreGuard.h"

namespace fs = std::filesystem;

namespace {

// the viewer's side for the test: what the interaction asked for, and the CfgMoves player driven by it
struct FakeHost : ISceneInteractionHost
{
    MovesSim* sim = nullptr;
    double time = 0.0;
    std::vector<std::string> played;
    int clicks = 0, faces = 0, bone = -1;
    float clickX = 0, clickY = 0;
    InspectTarget inspected = InspectTarget::None;
    glm::vec2 grid{ 0.0f };

    bool HasModel() const override { return true; }
    bool ClickPoint(float x, float y) override { ++clicks; clickX = x; clickY = y; return true; }
    void ToggleFace(const glm::mat4&, float, float) override { ++faces; }
    InspectTarget Inspected() const override { return inspected; }
    void Inspect(InspectTarget what) override { inspected = what; }
    int BoneCount() const override { return 3; }
    int SelectedBone() const override { return bone; }
    void SetSelectedBone(int b) override { bone = b; }
    void BoneExtent(int b, glm::vec3& mn, glm::vec3& mx) const override { mn = glm::min(mn, glm::vec3(float(b))); mx = glm::max(mx, glm::vec3(float(b) + 0.5f)); }
    void GridFollow(const glm::vec2& cell) override { grid = cell; }
    void MovesPlay(const std::string& action) override
    {
        played.push_back(action);
        if (const int t = sim->Resolve(action, false, false); t >= 0) sim->Go(action, t, false, time);
    }
    void SetMovesDrive(bool on) override { sim->SetDrive(on, time, nullptr); }
};

} // namespace

std::string RunViewCoreTest(const fs::path& dir, int* failuresOut)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string out;
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        out += std::format("\n    {} {}", ok ? "ok  " : "FAIL", what);
        if (!ok) { ++failures; Log::Get().Error("view coretest: {}", what); }
    };
    check(std::string(AppModeName(AppMode::Animation)) == "Skeleton & Anim" && std::string(AppModeName(AppMode::Optics)) == "Optics",
          "AppMode: the mode names (no UI header)");

    // a CfgMoves of two states: idle and a looped walk whose RTM steps 1 m per cycle (engine space: -z = forward)
    auto writeRtm = [&](const char* name, float step) {
        RtmAnimation a;
        a.info.bones = { "root" };
        a.info.move = glm::vec3(0, 0, -step);
        a.times = { 0.0f, 1.0f };
        a.poses.resize(2);
        std::string err;
        fs::remove(dir / name, ec);
        return WriteRtm0101(a, dir / name, err);
    };
    const bool rtms = writeRtm("vc_idle.rtm", 0.0f) && writeRtm("vc_walk.rtm", 1.0f);
    const fs::path cpp = dir / "vc_config.cpp";
    std::ofstream(cpp) << "class CfgMovesBasic { class Default { file = \"\"; actions = \"NoActions\"; speed = 1; looped = 1; }; class Actions { class NoActions {}; }; };\n"
                          "class CfgMovesVc: CfgMovesBasic { skeletonName = \"VcSkel\";\n"
                          "  class States { class idle: Default { actions = \"VcActions\"; file = \"vc_idle.rtm\"; InterpolateTo[] = { \"walk\", 0.1 }; interpolationSpeed = 100; };\n"
                          "    class walk: Default { actions = \"VcActions\"; file = \"vc_walk.rtm\"; speed = 1; InterpolateTo[] = { \"idle\", 0.1 }; interpolationSpeed = 100; }; };\n"
                          "  class Actions { class VcActions: NoActions { upDegree = -1; Default = \"idle\"; Stop = \"idle\"; WalkF = \"walk\"; }; }; };\n";
    {
        DiskFileSystem disk;
        EventBus bus;
        Session doc(disk, bus);
        ArmaConfig cfg;
        cfg.Load(cpp, dir, false);
        const bool built = rtms && doc.moves.Build(cfg, "CfgMovesVc", dir);
        MovesSim sim(doc);
        Playback pb(doc);
        pb.playing = true;
        MovesControl ctl;
        ctl.DefaultKeymap();
        ctl.on = true;
        OrbitCamera cam;
        FakeHost host;
        host.sim = &sim;
        SceneInteraction si(cam, host);
        // W held: the walk action, then the character goes forward (+z, the way it faces)
        InputState in;
        in.SetLetter('W', true);
        bool stepped = built;
        for (int f = 0; f < 60 && stepped; ++f)
        {
            stepped = si.Control(in, 1.0f / 30.0f, ctl, sim, pb, doc.moves);
            pb.time += 1.0 / 30.0;
            host.time = pb.time;
            sim.Update(pb.time);
        }
        const bool walked = !host.played.empty() && host.played.front() == "WalkF";
        check(built && stepped && walked && sim.drive && ctl.rootPos.z > 0.5f && std::abs(ctl.rootPos.x) < 1e-3f,
              std::format("control: W plays WalkF and the character goes forward (z {:+.2f} m, x {:+.3f} m after 2 s)", ctl.rootPos.z, ctl.rootPos.x));
        // Q held (W released: Stop): the character turns to its left (Arma: x right, z forward)
        ctl.rootPos = glm::vec3(0); // (not ResetRoot: it would forget that W was held - no Stop then)
        ctl.rootYaw = 0;
        InputState q;
        q.SetLetter('Q', true);
        si.Control(q, 0.5f, ctl, sim, pb, doc.moves); // 45 degrees
        const glm::vec3 fwd = glm::vec3(ctl.RootMatrix() * glm::vec4(0, 0, 1, 0));
        check(fwd.x < -0.5f && fwd.z > 0.5f && !host.played.empty() && host.played.back() == "Stop",
              std::format("control: Q turns left (forward {:+.2f} {:+.2f}), releasing W stops", fwd.x, fwd.z));
        // a text field has the keyboard: nothing moves
        const glm::vec3 before = ctl.rootPos;
        InputState typing = in;
        typing.textInput = true;
        si.Control(typing, 0.5f, ctl, sim, pb, doc.moves);
        check(ctl.rootPos == before && ctl.rootYaw == -45.0f, "control: no key works while a text field has the keyboard");
        ctl.on = false;
        check(!si.Control(in, 0.1f, ctl, sim, pb, doc.moves), "control: off - no step");
    }
    // mouse: a left click without dragging picks (the point under it -> the Inspector), a drag orbits, the wheel zooms
    {
        OrbitCamera cam;
        FakeHost host;
        SceneInteraction si(cam, host);
        const SceneRect rect{ 0, 0, 800, 600 };
        InputState in;
        in.overScene = true;
        in.mouseX = 400;
        in.mouseY = 300;
        in.pressed[InputState::Left] = in.down[InputState::Left] = true;
        si.Mouse(in, rect, glm::mat4(1.0f));
        in.pressed[InputState::Left] = in.down[InputState::Left] = false;
        in.released[InputState::Left] = true;
        si.Mouse(in, rect, glm::mat4(1.0f));
        check(host.clicks == 1 && host.clickX == 400 && host.inspected == InspectTarget::Point, "mouse: a click picks the point under the cursor, the Inspector shows it");
        InputState c = in;
        c.ctrl = true;
        c.released = {};
        c.pressed[InputState::Left] = c.down[InputState::Left] = true;
        si.Mouse(c, rect, glm::mat4(1.0f));
        c.pressed[InputState::Left] = c.down[InputState::Left] = false;
        si.Mouse(c, rect, glm::mat4(1.0f));
        check(host.faces == 1 && host.clicks == 1, "mouse: Ctrl+click picks a face, not a point");
        const float yaw = cam.yaw, dist = cam.distance;
        InputState d;
        d.overScene = true;
        d.mouseX = 100;
        d.pressed[InputState::Left] = d.down[InputState::Left] = true;
        si.Mouse(d, rect, glm::mat4(1.0f));
        d.pressed = {};
        d.mouseX = 160;
        d.mouseDX = 60;
        si.Mouse(d, rect, glm::mat4(1.0f));
        d.down = {};
        d.mouseDX = 0;
        si.Mouse(d, rect, glm::mat4(1.0f));
        InputState w;
        w.overScene = true;
        w.wheel = 2;
        si.Mouse(w, rect, glm::mat4(1.0f));
        check(cam.yaw != yaw && host.clicks == 1 && cam.distance < dist, "mouse: a drag orbits (no pick), the wheel zooms in");
        // bones: select (Inspector: bone), focus frames it, again deselects
        si.SelectBone(2, true, false);
        const bool selected = host.bone == 2 && host.inspected == InspectTarget::Bone && std::abs(cam.target.x - 2.25f) < 1e-3f;
        si.SelectBone(2, false, false);
        check(selected && host.bone == -1 && host.inspected == InspectTarget::None, "bones: select + focus (the camera frames it), again deselects");
    }
    // the point overlay's settings through the settings file
    {
        PointOverlaySettings s;
        s.labels = false;
        s.hitpointsView = true;
        s.occludeByMesh = true;
        s.filter = "wheel";
        Settings file;
        file.SetPairs(s.ToPairs(), "points");
        PointOverlaySettings back;
        back.FromPairs(file.Pairs("points"));
        PointOverlaySettings partial;
        partial.FromPairs({ { "Filter", "x" } });
        check(back == s && partial.labels && partial.filter == "x", "point overlay settings: through the settings file and back; missing keys keep the defaults");
    }
    if (failuresOut) *failuresOut = failures;
    return std::format("{} check(s) failed{}", failures, out);
}
