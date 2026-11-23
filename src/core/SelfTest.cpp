// Core self test: the parsers, CfgMoves, the document's command stack and the string table on files written here -
// needs no game data (CI, armaview-cli selftest, ui-script coretest).
#include "SelfTest.h"
#include "ArmaConfig.h"
#include "AssetSession.h"
#include "Gltf.h"
#include "LayersJob.h"
#include "Paths.h"
#include "Settings.h"
#include "Visibility.h"
#include "Log.h"
#include "Moves.h"
#include "MaterialSet.h"
#include "MovesEditor.h"
#include "MovesSim.h"
#include "Playback.h"
#include "MovesEdit.h"
#include "RTM.h"
#include "RvMaterial.h"
#include "Retarget.h" // WriteRtm0101
#include "Session.h"
#include "Skeleton.h"
#include "Strings.h"

#include <cmath>
#include <format>
#include <fstream>
#include <thread>

#include "CoreGuard.h"
#include "Unit.h"
#include "Attachments.h"
#include "RvMaterial.h"
#include "Texture.h"

namespace fs = std::filesystem;

std::string RunCoreTest(const fs::path& dir, int* failuresOut)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string out;
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        out += std::format("\n    {} {}", ok ? "ok  " : "FAIL", what);
        if (!ok) { ++failures; Log::Get().Error("coretest: {}", what); }
    };
    // model.cfg: skeleton with inheritance, sections, an animation class
    const fs::path cfgFile = dir / "model.cfg";
    std::ofstream(cfgFile) << "class CfgSkeletons { class Default { isDiscrete = 1; skeletonInherit = \"\"; skeletonBones[] = {}; };\n"
                              "  class TestSkel: Default { skeletonBones[] = { \"root\", \"\", \"arm\", \"root\", \"hand\", \"arm\" }; }; };\n"
                              "class CfgModels { class Default { sections[] = {}; sectionsInherit = \"\"; skeletonName = \"\"; };\n"
                              "  class core: Default { skeletonName = \"TestSkel\"; sections[] = { \"camo\" };\n"
                              "    class Animations { class arm_rot { type = \"rotation\"; source = \"time\"; selection = \"arm\"; axis = \"arm_axis\";\n"
                              "      minValue = 0; maxValue = 1; angle0 = 0; angle1 = \"rad 90\"; }; }; }; };\n";
    {
        ArmaConfig cfg;
        cfg.Load(cfgFile, dir, false);
        Skeleton sk;
        ModelCfgInfo info;
        const bool ok = BuildSkeleton(cfg, "core", sk, info);
        check(ok && sk.bones.size() == 3 && sk.Find("hand") >= 0 && sk.bones[size_t(sk.Find("hand"))].parent == sk.Find("arm"), "model.cfg: skeleton of 3 bones with parents");
        std::vector<CfgAnimation> anims;
        std::vector<CfgSource> sources;
        BuildCfgAnimations(cfg, info, sk, anims, sources);
        check(anims.size() == 1 && anims[0].type == CfgAnimType::Rotation && std::abs(anims[0].angle1 - 1.5707964f) < 1e-4f, "model.cfg: rotation animation, angle1 = rad 90");
    }
    // config.cpp: a move config whose first state is a death state: the start state must still be the idle one
    const fs::path cpp = dir / "config.cpp";
    std::ofstream(cpp) << "class CfgMovesBasic { class Default { file = \"\"; actions = \"NoActions\"; speed = 1; looped = 1; }; class Actions { class NoActions {}; }; };\n"
                          "class CfgMovesCore: CfgMovesBasic { skeletonName = \"TestSkel\";\n"
                          "  class States { class DeadState: Default { actions = \"DeadActions\"; file = \"core.rtm\"; terminal = 1; };\n"
                          "    class idle: Default { actions = \"CoreActions\"; file = \"core.rtm\"; InterpolateTo[] = { \"walk\", 0.1 }; };\n"
                          "    class walk: Default { actions = \"CoreActions\"; file = \"core.rtm\"; InterpolateTo[] = { \"idle\", 0.1 }; }; };\n"
                          "  class Actions { class DeadActions: NoActions { upDegree = \"ManPosDead\"; Default = \"DeadState\"; };\n"
                          "    class CoreActions: NoActions { upDegree = -1; Default = \"idle\"; Stop = \"idle\"; WalkF = \"walk\"; Die = \"\"; }; }; };\n";
    // RTM: written by the viewer's own writer, read back
    {
        RtmAnimation a;
        a.info.bones = { "root", "arm", "hand" };
        for (int f = 0; f < 3; ++f)
        {
            a.times.push_back(f / 2.0f);
            for (int b = 0; b < 3; ++b)
            {
                BonePose p;
                p.pos = glm::vec3(0.1f * f, 0.2f * b, 0.0f);
                a.poses.push_back(p);
            }
        }
        std::string err;
        fs::remove(dir / "core.rtm", ec); // the writer never overwrites
        const bool w = WriteRtm0101(a, dir / "core.rtm", err);
        RtmAnimation r;
        std::vector<std::string> warn;
        const bool ok = w && LoadRtm(dir / "core.rtm", r, err, warn);
        check(ok && r.Frames() == 3 && r.Bones() == 3 && std::abs(r.Sample(1, 1.0f).pos.x - 0.2f) < 1e-4f, "RTM_0101: write + read back, 3 frames x 3 bones");
    }
    {
        ArmaConfig cfg;
        cfg.Load(cpp, dir, false);
        MovesGraph g;
        const bool built = g.Build(cfg, "CfgMovesCore", dir);
        std::string why;
        const int s = built ? g.StartState(why) : -1;
        check(built && g.states.size() == 3, "config.cpp: CfgMoves with 3 states");
        check(s >= 0 && g.states[size_t(s)].name == "idle", "CfgMoves start state: idle, not the death state listed first (" + why + ")");
        check(g.StateOfAction(g.Find("idle"), "Die") < 0, "an action set to \"\" is no action (does not fall back to ManActions)");
        check(g.IsDeathLike(g.Find("DeadState")), "DeadState is recognised as death");
        // the graph's disk cache: the same graph back, another key (a config changed) is a miss; one state by name
        const fs::path cacheFile = dir / "moves_cache.bin";
        const std::string key = MovesCacheKey(cfg, "CfgMovesCore", dir);
        MovesGraph back;
        const bool roundTrip = g.SaveCache(cacheFile, key) && back.LoadCache(cacheFile, key);
        check(roundTrip && back.states.size() == g.states.size() && back.Find("walk") == g.Find("walk") &&
                  back.StateOfAction(back.Find("idle"), "WalkF") == g.Find("walk") && back.actions.size() == g.actions.size(),
              "CfgMoves graph cache: written and read back the same (states, links, actions)");
        check(!back.LoadCache(cacheFile, key + "|changed"), "CfgMoves graph cache: another key is a miss");
        MoveState one;
        check(ReadMoveState(cfg, "CfgMovesCore", "walk", dir, one) && one.name == "walk" && one.actions == "CoreActions",
              "one state by name without building the graph");
    }
    // document edits go through the Session's command stack: undo / redo restore the config text, a load clears it;
    // events reach their subscribers (a Session of its own on the generated config - never a user file)
    {
        DiskFileSystem disk;
        EventBus bus;
        int published = 0;
        bus.Subscribe([&](SessionEvent e) { published += e == SessionEvent::ModelLoaded; });
        bus.Publish(SessionEvent::ModelLoaded);
        check(published == 1, "EventBus: a subscriber receives the published event");
        Session doc(disk, bus);
        std::string err;
        const bool loaded = doc.movesDoc.Load(cpp, err);
        const std::string original = doc.movesDoc.Text();
        const bool edited = loaded && doc.movesDoc.SetEntry("CfgMovesCore/States/idle", "speed", "0.25", false, err);
        const std::string changed = doc.movesDoc.Text();
        check(edited && changed != original && doc.commands.CanUndo(), "config edit: recorded on the command stack (" + err + ")");
        check(doc.commands.Undo() && doc.movesDoc.Text() == original && doc.commands.CanRedo(), "undo restores the config text");
        check(doc.commands.Redo() && doc.movesDoc.Text() == changed, "redo applies the edit again");
        check(doc.movesDoc.Load(cpp, err) && !doc.commands.CanUndo() && !doc.commands.CanRedo(), "loading the file clears the stack");
    }
    // game mode (MovesSim) on the generated CfgMoves: start state, an action as a playMove along InterpolateTo, the log,
    // the player's states as layers of the Session's AnimStack, death only by an explicit command
    {
        DiskFileSystem disk;
        EventBus bus;
        Session doc(disk, bus);
        ArmaConfig cfg;
        cfg.Load(cpp, dir, false);
        doc.moves.Build(cfg, "CfgMovesCore", dir);
        MovesSim sim(doc);
        int started = -2;
        sim.SetDrive(true, 0.0, &started);
        check(sim.drive && started >= 0 && doc.moves.states[size_t(started)].name == "idle", "game mode starts in the start state idle");
        const int walk = sim.Resolve("WalkF", false, false);
        if (walk >= 0) sim.Go("WalkF", walk, false, 0.5);
        check(walk >= 0 && doc.moves.states[size_t(walk)].name == "walk" && !sim.log.empty() && sim.log.back().kinds == "I",
              "action WalkF: playMove idle > walk along InterpolateTo, logged (" + sim.msg + ")");
        check(sim.Update(0.6) >= 0 && !doc.anim.layers.empty(), "the player's states become animation layers");
        check(sim.Resolve("DeadState", false, false) < 0 && sim.Resolve("DeadState", true, true) >= 0,
              "a death state only by an explicit command");
        sim.SetDrive(false, 1.0, nullptr);
        check(!sim.drive && doc.anim.layers.empty(), "game mode off: the user's layers come back");
    }
    // CfgMoves edits in terms of states (MovesEditor) on the generated config: own states, a link with its cost, a new
    // state, a rename that follows the links; each is one undo step
    {
        DiskFileSystem disk;
        EventBus bus;
        Session doc(disk, bus);
        ArmaConfig cfg;
        cfg.Load(cpp, dir, false);
        doc.moves.Build(cfg, "CfgMovesCore", dir);
        MovesEditor ed(doc.movesDoc, doc.moves);
        const int idle = doc.moves.Find("idle"), walk = doc.moves.Find("walk");
        ed.Open(&cfg);
        check(ed.IsOwn(idle) && doc.movesDoc.Loaded(), "the generated CfgMoves is the user's own config (" + ed.msg + ")");
        const bool linked = ed.Link(&cfg, idle, walk, false);
        check(linked && doc.movesDoc.Text().find("connectTo[] = {\"walk\",0.05}") != std::string::npos, "link idle -> walk: connectTo with cost 0.05");
        const bool added = ed.AddState("run", "walk");
        check(added && doc.movesDoc.Text().find("class run: walk") != std::string::npos, "new state run: walk");
        const bool renamed = ed.Rename(walk, "stroll");
        const std::string t = doc.movesDoc.Text();
        check(renamed && t.find("class stroll") != std::string::npos && t.find("\"walk\"") == std::string::npos, "rename walk -> stroll, links too");
        check(doc.commands.Size() == 3 && doc.commands.Undo() && doc.movesDoc.Text().find("class walk") != std::string::npos, "three edits, undo the rename");
    }
    // RVMAT edit (MaterialSet): a value changed and saved into the text - its comment and layout kept, a .bak written
    {
        const fs::path rv = dir / "test.rvmat";
        std::error_code rec;
        fs::remove(rv.string() + ".bak", rec);
        std::ofstream(rv) << "// my material\nambient[] = {1,1,1,1};\nspecularPower = 40; // shiny\nPixelShaderID = \"Super\";\n";
        MaterialSet mats(dir);
        const RvMaterial* m = mats.Get(rv.string());
        check(m && m->loaded && std::abs(m->specularPower - 40.0f) < 1e-4f, "RVMAT read: specularPower 40");
        if (m)
        {
            RvMaterial e = *m;
            e.specularPower = 12.5f;
            mats.Edit(rv.string(), e);
            const RvMaterial original = *m;
            const bool saved = mats.Save(rv.string(), original);
            std::ifstream in(rv);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            check(saved && text.find("specularPower = 12.5; // shiny") != std::string::npos && text.find("// my material") != std::string::npos &&
                      fs::exists(rv.string() + ".bak", rec),
                  "RVMAT save: value replaced in place, comments kept, .bak written (" + mats.saveMsg + ")");
            const RvMaterial* again = mats.Get(rv.string());
            check(again && std::abs(again->specularPower - 12.5f) < 1e-4f && !mats.HasEdit(rv.string()), "RVMAT re-read after save");
        }
    }
    // the player (Playback): frame steps and scrubbing on the primary layer, a layer to the A/B compare and back
    {
        DiskFileSystem disk;
        EventBus bus;
        Session doc(disk, bus);
        auto a = std::make_shared<RtmAnimation>();
        std::string err;
        std::vector<std::string> warn;
        if (LoadRtm(dir / "core.rtm", *a, err, warn))
        {
            AnimLayer l1, l2;
            l1.anim = a; l1.duration = 1.0f;
            l2.anim = a; l2.duration = 1.0f;
            doc.anim.layers = { l1, l2 };
            Playback pb(doc);
            pb.StepFrame(1);
            const float ph1 = doc.anim.Phase(doc.anim.layers[0], pb.time);
            pb.ScrubTo(0.5f);
            check(std::abs(ph1 - 0.5f) < 1e-3f && std::abs(doc.anim.Phase(doc.anim.layers[0], pb.time) - 0.5f) < 1e-3f && !pb.playing,
                  "player: a frame step (3 frames -> phase 0.5), scrubbing, pauses");
            check(pb.MoveToCompare(1) && doc.anim.layers.size() == 1 && pb.compare.layers.size() == 1 && pb.compareMode == 2 &&
                      pb.ReturnCompare() && doc.anim.layers.size() == 2,
                  "player: a layer to the A/B compare and back");
        }
        else check(false, "player: core.rtm readable (" + err + ")");
        int added = 0;
        bus.Subscribe([&](SessionEvent e) { added += e == SessionEvent::LayerAdded; });
        Playback pb(doc);
        doc.anim.layers.clear();
        check(pb.AddLayer(dir / "core.rtm") && added == 1 && doc.anim.layers.size() == 1 && pb.playing &&
                  std::abs(doc.anim.layers[0].duration - 2.0f / pb.fps) < 1e-4f,
              "player: AddLayer loads an RTM as a layer (duration from fps), starts playing, publishes LayerAdded");
        // the log substituted in a test: the failure is reported (captured here), not counted as an app error
        {
            const int errorsBefore = Log::Get().Errors();
            LogCapture cap;
            const size_t layers = doc.anim.layers.size();
            const bool loaded = pb.AddLayer(dir / "missing.rtm");
            check(!loaded && doc.anim.layers.size() == layers && cap.Count(LogLevel::Error) >= 1 && cap.Has(LogLevel::Error, "missing.rtm") &&
                      Log::Get().Errors() == errorsBefore,
                  "log: AddLayer of a missing file adds nothing, its error is captured and not counted");
        }
    }
    // unit: a soldier class read from an addon config over game configs (face by identity, worn items by ItemInfo type,
    // NVG up = modelOff, backpack); why parts are not shown (missing item class, model not on disk, faceType = a face)
    {
        const fs::path game = dir / "unit_game.cpp", addon = dir / "unit_addon.cpp";
        std::ofstream(game) << "class CfgWeapons { class H_Test { scope = 2; hiddenSelectionsTextures[] = {\"x_co.paa\"};\n"
                               "    class ItemInfo { type = 605; uniformModel = \"\\core_test\\helmet\"; hiddenSelections[] = {\"camo\"}; }; };\n"
                               "  class NVG_Test { scope = 2; class ItemInfo { type = 616; uniformModel = \"\\core_test\\nvg_down\"; modelOff = \"\\core_test\\nvg_up\"; }; };\n"
                               "  class ItemMap { class ItemInfo { type = 608; }; }; };\n"
                               "class CfgFaces { class Man_Test { class F1 { head = \"H1\"; identityTypes[] = {\"Other\"}; };\n"
                               "  class F2 { head = \"H1\"; identityTypes[] = {\"Test_Id\"}; texture = \"face_co.paa\"; }; }; };\n"
                               "class CfgHeads { class H1 { model = \"\\core_test\\head\"; }; };\n";
        std::ofstream(addon) << "class CfgVehicles { class Soldier_Test { model = \"\\core_test\\body\"; uniformClass = \"U_Test\"; faceType = \"Man_Test\";\n"
                                "    identityTypes[] = {\"Test_Id\"}; linkedItems[] = {\"H_Test\", \"NVG_Test\", \"ItemMap\", \"Missing_Item\"}; backpack = \"B_Test\";\n"
                                "    hiddenSelections[] = {\"camo\"}; hiddenSelectionsTextures[] = {\"body_co.paa\"}; };\n"
                                "  class B_Test { model = \"\\core_test\\pack\"; isbackpack = 1; scope = 2; };\n"
                                "  class Custom_Test: Soldier_Test { faceType = \"F2\"; linkedItems[] = {}; backpack = \"\"; }; };\n";
        auto g = std::make_shared<ArmaConfig>();
        g->Load(game, dir, false);
        ArmaConfig cfg;
        cfg.SetFallback(g);
        cfg.Load(addon, dir, false);
        Unit u;
        LogCapture cap;
        const bool read = u.Read(cfg, "Soldier_Test");
        auto part = [&](UnitSlot s) -> const UnitPart* {
            for (const auto& p : u.Parts())
                if (p.slot == s) return &p;
            return nullptr;
        };
        const UnitPart *head = part(UnitSlot::Head), *hg = part(UnitSlot::Headgear), *nvg = part(UnitSlot::NVG), *pack = part(UnitSlot::Backpack);
        check(read && u.Parts().size() == 4 && head && head->cls == "F2" && hg && hg->cls == "H_Test" && nvg && nvg->armaModel.find("nvg_up") != std::string::npos &&
                  pack && pack->armaModel.find("pack") != std::string::npos && u.UniformSkin().textures.size() == 1,
              "unit: head by identityTypes, headgear / NVG (modelOff) from linkedItems[] via the game configs, backpack=, uniform skin");
        check(cap.Has(LogLevel::Warn, "Missing_Item") && !cap.Has(LogLevel::Warn, "ItemMap"), "unit: a linked item not in CfgWeapons is reported, the map is skipped quietly");
        u.Load(cfg, Skeleton{}, dir);
        check(hg->problem.find("file not on disk") != std::string::npos && !hg->Shown() && cap.Has(LogLevel::Warn, "not shown"),
              "unit: a part whose model is not on disk is not shown, with the reason");
        u.Replace(cfg, UnitSlot::Vest, "No_Vest");
        check(part(UnitSlot::Vest) && part(UnitSlot::Vest)->problem.find("not found") != std::string::npos, "unit: a replaced part of a missing class says so");
        const bool custom = u.Read(cfg, "Custom_Test");
        check(custom && u.Parts().size() == 1 && u.Parts()[0].cls == "F2" && cap.Has(LogLevel::Warn, "is a face"),
              "unit: faceType naming a face (not a CfgFaces class) is found and reported");
        const auto soldiers = Unit::SoldierClasses(cfg);
        const auto hats = Unit::Candidates(cfg, UnitSlot::Headgear);
        check(soldiers.size() == 2 && hats.size() == 1 && hats[0] == "H_Test", "unit: soldier classes and headgear candidates");
        // outfit: a P3D body, a file part, the preset text round trip (class parts and files, hidden flag)
        u.StartOutfit(fs::path(L"C:\\body\\man.p3d"));
        u.ApplyPresetParts(&cfg, {});
        u.AddFile(UnitSlot::Other, fs::path(L"C:\\gear\\pouch.p3d"));
        u.Replace(cfg, UnitSlot::Headgear, "H_Test");
        u.Parts().back().enabled = false;
        Unit::Preset back;
        std::string perr;
        const bool parsed = Unit::ParsePreset(Unit::SavePreset(u.MakePreset()), back, perr);
        check(u.Outfit() && parsed && back.bodyFile == fs::path(L"C:\\body\\man.p3d") && back.parts.size() == 2 && back.parts[0].slot == UnitSlot::Headgear &&
                  !back.parts[0].file && back.parts[0].what == "H_Test" && back.parts[1].file && back.parts[1].what == "C:\\gear\\pouch.p3d" &&
                  back.parts[1].enabled != back.parts[0].enabled,
              "unit: outfit preset round trip (body file, class part, file part, hidden flag) " + perr);
        Unit::Preset bad;
        check(!Unit::ParsePreset("part=vest|class|V_X|1\n", bad, perr) && !Unit::ParsePreset("body=class:X\npart=hat|class|H|1\n", bad, perr),
              "unit: a preset without a body or with an unknown slot is refused");
    }
    // log: a scoped sink receives other threads' entries, and is gone after its scope
    {
        MemoryLogSink mem;
        {
            ScopedLogSink reg(&mem);
            std::thread([] { Log::Get().Info("coretest: from a worker"); }).join();
        }
        Log::Get().Debug("coretest: after the sink");
        const auto e = mem.Entries();
        check(e.size() == 1 && e[0].level == LogLevel::Info && e[0].text == "coretest: from a worker",
              "log: a ScopedLogSink gets a worker's entry, nothing after it is removed");
    }
    // opening an RTM and its hot reload (AssetSession): the layer gets the new file; paths through IPaths
    {
        struct View : IAssetView
        {
            int covered = 0;
            bool LoadModel(const fs::path&, bool, bool) override { return false; }
            void OpenModel(const fs::path&, bool) override {}
            bool TextureChanged(const fs::path&) override { return true; }
            void GeometryChanged() override {}
            void AttachmentsChanged() override {}
            std::vector<fs::path> TextureFiles() const override { return {}; }
            void LogLayerCoverage(const AnimLayer&) const override { ++const_cast<View*>(this)->covered; }
            void LogMissingBones(const std::string&, const std::vector<int>&, bool) const override {}
        } view;
        DiskFileSystem disk;
        EventBus bus;
        Session doc(disk, bus);
        Playback pb(doc);
        MaterialSet mats(doc.projectRoot);
        MovesSim sim(doc);
        AssetSession assets(doc, pb, mats, sim, disk, view);
        const FixedPaths paths(dir);
        const fs::path rtm = paths.Data() / "hot" / "hot.rtm";
        fs::create_directories(rtm.parent_path(), ec);
        fs::remove(rtm, ec);
        fs::copy_file(dir / "core.rtm", rtm, ec);
        assets.OpenFile(rtm);
        const bool opened = doc.anim.layers.size() == 1 && assets.rtmFolder == rtm.parent_path();
        const auto before = opened ? doc.anim.layers[0].anim : nullptr;
        assets.watcher.debounce = 0.0;
        assets.Poll(1.0); // the watch list
        { // a re-export with one more frame
            RtmAnimation a;
            std::string err;
            std::vector<std::string> warn;
            LoadRtm(rtm, a, err, warn);
            a.times.push_back(1.5f);
            for (int b = 0; b < int(a.Bones()); ++b) a.poses.push_back(a.poses[size_t(b)]);
            fs::remove(rtm, ec);
            WriteRtm0101(a, rtm, err);
        }
        assets.Poll(2.0); // noticed
        assets.Poll(3.0); // stable: re-read
        check(opened && !doc.anim.layers.empty() && doc.anim.layers[0].anim != before && doc.anim.layers[0].anim->Frames() == 4 && view.covered >= 1,
              "hot reload: an opened RTM is re-read into its layer after a change on disk (IPaths data folder)");
    }
    // glTF import: the generated sample (a bar on two joints, embedded PNGs, one clip) -> mesh, weights, joints as pivots,
    // the clip as deformation matrices in the viewer's axes (x mirrored); a binarized-looking file is refused with the message
    {
        const fs::path gf = dir / "gltf" / "sample.gltf";
        P3DModel gm;
        std::string gerr;
        const bool read = WriteGltfSample(gf) && LoadGltf(gf, gm, gerr);
        bool weights = read && gm.lods.size() == 2 && gm.lods[0].selections.size() == 2;
        if (weights)
            for (size_t p = 0; p < gm.lods[0].points.size(); ++p)
                weights &= std::abs(gm.lods[0].selections[0].Weight(p) + gm.lods[0].selections[1].Weight(p) - 1.0f) < 0.01f;
        const bool textures = read && !gm.lods[0].faces.empty() && fs::exists(fs::path(Utf8ToWide(gm.lods[0].faces[0].texture)), ec) &&
                              fs::exists(fs::path(Utf8ToWide(gm.lods[0].faces[0].material)), ec);
        check(read && gm.format == "glTF" && gm.lods[0].points.size() == 12 && gm.lods[0].faces.size() == 20 && weights && textures &&
                  gm.importedBones.size() == 2 && gm.importedBones[1].second == "bone_base" && std::abs(gm.lods[1].points[1].pos.y - 1.0f) < 1e-4f,
              "glTF: mesh, UV, embedded base color / normal textures (+ RVMAT), weights per joint, joints as pivots (" + gerr + ")");
        // the clip: at the end bone_tip has turned 90 degrees about +Z (glTF): the bar's top (0, 2, 0) goes to glTF (-1, 1, 0) = (1, 1, 0) here
        bool clipOk = read && gm.importedClips.size() == 1 && gm.importedClips[0].times.size() == 31 && std::abs(gm.importedClips[0].duration - 1.0f) < 1e-4f;
        if (clipOk)
        {
            const auto anim = ImportedClipAnimation(gm, gm.importedClips[0]);
            const glm::vec3 top = glm::vec3(anim->poses[30 * 2 + 1].Matrix() * glm::vec4(0, 2, 0, 1));
            const glm::vec3 still = glm::vec3(anim->poses[0 * 2 + 1].Matrix() * glm::vec4(0, 2, 0, 1));
            clipOk = glm::length(top - glm::vec3(1, 1, 0)) < 1e-3f && glm::length(still - glm::vec3(0, 2, 0)) < 1e-3f && anim->Bones() == 2;
        }
        check(clipOk, "glTF: clip resampled at 30 fps, deformation matrices (G x inverse bind) in the viewer's axes");
        // binarized files: refused with the message (only the source formats are read)
        std::ofstream(dir / "bin_model.p3d", std::ios::binary) << "BINx" << std::string(64, '\0');
        std::ofstream(dir / "bin_anim.rtm", std::ios::binary) << "BINx" << std::string(64, '\0');
        P3DModel bm;
        RtmAnimation ba;
        std::string berr;
        std::vector<std::string> bw;
        bool p3dRefused;
        {
            std::vector<LogEntry> cap;
            Log::CaptureScope scope(cap);
            p3dRefused = !LoadP3D(dir / "bin_model.p3d", bm);
            bool said = false;
            for (const auto& e : cap) said |= e.text.find(kBinarizedNotSupported) != std::string::npos;
            p3dRefused &= said;
        }
        check(p3dRefused && !LoadRtm(dir / "bin_anim.rtm", ba, berr, bw) && berr == kBinarizedNotSupported && ba.info.binarized,
              "binarized model / animation refused: \"" + std::string(kBinarizedNotSupported) + "\"");
    }
    // settings: one file of named blocks (atomic save, export / import), visibility in it
    {
        const FixedPaths paths(dir / "settings");
        fs::remove_all(paths.Data(), ec);
        fs::create_directories(paths.Data() / "old" / "graphs", ec);
        std::ofstream(paths.Data() / "old" / "graphs" / "x.graph") << "pos";
        Settings st(paths.Data() / "settings.txt");
        const bool fresh = !st.Load();
        st.Set("UiScale", "1.25");
        st.Set("Lang", "en");
        st.SetBlock("imgui", "[Window][Log]\nPos=1,2\n");
        st.SetBlock("models", "[model]\tx.p3d\noverlay\t1.000\n");
        st.Set("RecFps", "25");
        st.SetBlock("layout work", "[Window][Log]\nPos=5,6\n");
        const bool saved = st.Save();
        Settings back(st.File());
        const bool loaded = back.Load();
        check(fresh && saved && loaded && back.Get("UiScale") == "1.25" && back.Get("Lang") == "en" && back.Get("RecFps") == "25" &&
                  back.Block("imgui").find("Pos=1,2") != std::string::npos &&
                  back.BlocksWithPrefix("layout ") == std::vector<std::string>{ "work" } && back.Block("models").find("overlay") != std::string::npos,
              "settings: blocks + key=value round trip");
        const fs::path exported = paths.Data() / "export.txt";
        Settings other(paths.Data() / "other.txt");
        check(back.SaveTo(exported) && other.LoadFrom(exported) && other.Get("RecFps") == "25" && !other.LoadFrom(paths.Data() / "old" / "graphs" / "x.graph"),
              "settings: export / import; a file that is not a settings file is refused");
        ModelVisibility vis;
        vis.Load(back, "y.p3d");
        vis.Lod(1.0f).hidden.insert("door");
        vis.Save();
        Settings again(back.File());
        again.Load();
        ModelVisibility vis2, visX;
        vis2.Load(again, "y.p3d");
        visX.Load(again, "x.p3d");
        check(vis2.Find(1.0f) && vis2.Find(1.0f)->hidden.count("door") && visX.overlays.count("1.000") == 1,
              "settings: per-model visibility in the models block, other models kept");
    }
    // the layers project (LayersJob): a batch over a folder, one file per step with its progress; existing outputs skipped
    {
        DiskFileSystem disk;
        EventBus bus;
        Session doc(disk, bus);
        ArmaConfig cfg;
        cfg.Load(cfgFile, dir, false);
        ModelCfgInfo info;
        BuildSkeleton(cfg, "core", doc.skeleton, info);
        const fs::path in = dir / "batch_in", outDir = dir / "batch_out";
        fs::remove_all(in, ec);
        fs::remove_all(outDir, ec);
        fs::create_directories(in, ec);
        fs::create_directories(outDir, ec);
        for (const char* n : { "a.rtm", "b.rtm", "c.rtm" }) fs::copy_file(dir / "core.rtm", in / n, ec);
        fs::copy_file(dir / "core.rtm", outDir / "b.rtm", ec); // exists: skipped
        RetargetJob rt({});
        LayersJob job(doc, rt);
        job.proj.duration = 1.0f;
        strncpy_s(job.batchDir, PathToUtf8(in).c_str(), _TRUNCATE);
        strncpy_s(job.batchOut, PathToUtf8(outDir).c_str(), _TRUNCATE);
        const bool started = job.BatchStart();
        const bool firstStep = job.BatchStep();
        const int afterOne = job.batch.index;
        while (job.BatchStep()) {}
        check(started && firstStep && afterOne == 1 && job.batch.total == 3 && !job.batch.running && job.batch.written == 2 &&
                  job.batch.skipped == 1 && job.batch.failed == 0 && fs::exists(outDir / "a.rtm") && fs::exists(outDir / "c.rtm"),
              "layers batch: 3 files stepped one by one, 2 written, 1 skipped (" + job.msg + ")");
    }
    // the RTM folder check (qt-req 1.6): opening an RTM does not read the whole folder on the UI thread - the results come
    // in AssetSession::Poll; re-reading one RTM re-checks that file only
    {
        struct NoView : IAssetView
        {
            bool LoadModel(const fs::path&, bool, bool) override { return false; }
            void OpenModel(const fs::path&, bool) override {}
            bool TextureChanged(const fs::path&) override { return true; }
            void GeometryChanged() override {}
            void AttachmentsChanged() override {}
            std::vector<fs::path> TextureFiles() const override { return {}; }
            void LogLayerCoverage(const AnimLayer&) const override {}
            void LogMissingBones(const std::string&, const std::vector<int>&, bool) const override {}
        } noView;
        const fs::path rtmDir = dir / "rtm_folder";
        fs::create_directories(rtmDir, ec);
        RtmAnimation a;
        a.info.bones = { "root", "arm" };
        a.times = { 0.0f, 1.0f };
        a.poses.resize(4);
        std::string err;
        const int n = 120;
        for (int i = 0; i < n; ++i)
            if (!fs::exists(rtmDir / std::format("a{:03}.rtm", i), ec)) WriteRtm0101(a, rtmDir / std::format("a{:03}.rtm", i), err);
        DiskFileSystem disk;
        EventBus bus;
        Session doc(disk, bus);
        doc.skeleton.name = "two";
        for (const char* b : { "root", "arm" }) { Bone bone; bone.name = b; doc.skeleton.bones.push_back(bone); }
        doc.skeleton.bones[1].parent = 0;
        Playback pb(doc);
        MaterialSet mats(dir);
        MovesSim sim(doc);
        AssetSession assets(doc, pb, mats, sim, disk, noView);
        LogCapture cap;
        assets.OpenFile(rtmDir / "a000.rtm");
        const size_t atOnce = assets.rtmChecks.size();
        for (int k = 0; k < 2000 && assets.rtmChecks.size() < size_t(n); ++k)
        {
            assets.Poll(double(k));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const size_t later = assets.rtmChecks.size();
        LogCapture reloadCap;
        const int filesBefore = assets.rtmFilesChecked;
        assets.ReloadRtm(rtmDir / "a005.rtm");
        const bool wholeFolder = reloadCap.Has(LogLevel::Info, "RTM check '") || assets.rtmFilesChecked != filesBefore + 1;
        check(atOnce == 0 && later == size_t(n) && !wholeFolder && assets.rtmChecks.size() == size_t(n),
              std::format("RTM folder check: {} result(s) right after opening (in the background: 0), {} after Poll, re-reading one RTM {}", atOnce, later,
                          wholeFolder ? "re-checked the whole folder" : "re-checked it alone"));
        // cancel: the check stops, what was checked so far stays; progress while it runs
        assets.RunRtmCheck();
        int done = 0, total = 0;
        assets.RtmCheckProgress(done, total);
        const bool running = assets.RtmCheckRunning();
        assets.CancelRtmCheck();
        check(!assets.RtmCheckRunning() && assets.rtmChecks.size() <= size_t(n) && done <= total && total <= n,
              std::format("RTM folder check: progress {} / {} while {}, cancelled: {} result(s) kept", done, total, running ? "running" : "already done",
                          assets.rtmChecks.size()));
    }
    // string table: every RU translation has the format specifiers of its English text (checked when it was generated)
    const auto [rows, tips] = TableStats();
    check(rows > 500 && tips > 200, std::format("string table: {} rows, {} tooltips", rows, tips));
    // a mod whose prefix is not its folder name and that has no $PBOPREFIX$ (qt-req 1.5): its config's own paths name the
    // prefix (\AL_Mods\<mod>\...): the vehicle class of the model, its skin and its files are found
    {
        const fs::path mod = dir / "mods" / "av_prefix_mod";
        fs::create_directories(mod / "Data", ec);
        fs::create_directories(mod / "Model", ec);
        fs::remove(mod / "$PBOPREFIX$", ec);
        std::ofstream(mod / "Data" / "body_co.paa") << "not a real texture: only its place counts";
        std::ofstream(mod / "Data" / "body.rvmat") << "class StageTI {};\n";
        std::ofstream(mod / "config.cpp")
            << "class CfgPatches { class av_prefix_mod { units[] = { \"AV_Test_Car\" }; }; };\n"
               "class CfgVehicles { class Car; class AV_Test_Car: Car { model = \"\\AL_Mods\\av_prefix_mod\\Model\\car.p3d\"; crew = \"B_Soldier_F\";\n"
               "  hiddenSelections[] = { \"camo\" }; hiddenSelectionsTextures[] = { \"\\AL_Mods\\av_prefix_mod\\Data\\body_co.paa\" };\n"
               "  hiddenSelectionsMaterials[] = { \"\\AL_Mods\\av_prefix_mod\\Data\\body.rvmat\" }; }; };\n";
        DiskFileSystem disk;
        EventBus bus;
        Session doc(disk, bus);
        doc.projectRoot = dir;
        doc.model.path = mod / "Model" / "car.p3d"; // the path only: no model file is written
        LogCapture cap;
        const bool loaded = doc.LoadAddonConfig();
        const std::string rel = ArmaPathOf(doc.model.path, dir);
        const CfgClass* veh = loaded && doc.modCfg ? FindVehicleClass(*doc.modCfg, rel) : nullptr;
        const bool tex = fs::equivalent(ResolveArmaPath("\\AL_Mods\\av_prefix_mod\\Data\\body_co.paa", dir), mod / "Data" / "body_co.paa", ec);
        // a prefix set by hand wins over the guess; $PBOPREFIX$ over both
        std::string why1, why2;
        SetModPrefixOverrides({ { mod, "Hand\\Set" } });
        const std::string byHand = InferModPrefix(mod, doc.modCfg.get(), &why1);
        SetModPrefixOverrides({});
        std::ofstream(mod / "$PBOPREFIX$") << "\\Pbo\\Prefix\\\n";
        const std::string byFile = InferModPrefix(mod, doc.modCfg.get(), &why2);
        fs::remove(mod / "$PBOPREFIX$", ec);
        check(byHand == "Hand\\Set" && byFile == "Pbo\\Prefix", std::format("mod prefix: set by hand {} ({}), $PBOPREFIX$ {} ({})", byHand, why1, byFile, why2));
        check(veh && veh->name == "AV_Test_Car" && !veh->GetString("crew").empty() && doc.configSkins.size() == 1 && tex,
              std::format("mod prefix from the config's paths (no $PBOPREFIX$): model {} -> class {}, {} skin(s), texture {}", rel, veh ? veh->name : "-",
                          doc.configSkins.size(), tex ? "found" : "NOT FOUND"));
    }
    // hiddenSelectionsTextures "": the selection is not drawn (as in the game / setObjectTexture [i, ""]); past the array:
    // the model's texture; material "" changes nothing; a class whose textures are all "" is still a re-texture
    {
        const fs::path cdir = dir / "skin_hide";
        fs::create_directories(cdir, ec);
        std::ofstream(cdir / "config.cpp", std::ios::binary | std::ios::trunc)
            << "class CfgVehicles {\n"
               "  class AV_Hide1 { model = \"\\av\\m.p3d\"; hiddenSelections[] = { \"s1\", \"s2\" }; hiddenSelectionsTextures[] = { \"\", \"x_co.paa\" }; };\n"
               "  class AV_HideAll { model = \"\\av\\m.p3d\"; hiddenSelections[] = { \"s1\", \"s2\" }; hiddenSelectionsTextures[] = { \"\", \"\" }; };\n"
               "  class AV_Short { model = \"\\av\\m.p3d\"; hiddenSelections[] = { \"s1\", \"s2\" }; hiddenSelectionsTextures[] = { \"y_co.paa\" }; };\n"
               "  class AV_MatEmpty { model = \"\\av\\m.p3d\"; hiddenSelections[] = { \"s1\" }; hiddenSelectionsMaterials[] = { \"\" }; };\n"
               "  class AV_NoArrays { model = \"\\av\\m.p3d\"; hiddenSelections[] = { \"s1\" }; };\n};\n";
        ArmaConfig skinCfg;
        skinCfg.Load(cdir / "config.cpp", cdir, false);
        const std::vector<ConfigSkin> skins = FindConfigSkins(skinCfg, "\\av\\m.p3d");
        auto skinOf = [&](const char* cls) -> const ConfigSkin* {
            for (const auto& k : skins)
                if (k.className == cls) return &k;
            return nullptr;
        };
        const ConfigSkin *hide1 = skinOf("AV_Hide1"), *hideAll = skinOf("AV_HideAll"), *shortTex = skinOf("AV_Short"), *matEmpty = skinOf("AV_MatEmpty");
        // a two-face model: s1 = face 0, s2 = face 1, both "orig_co.paa"
        AttachedModel am;
        am.status = AttachedModel::Status::Ok;
        P3DLod lod;
        lod.points.resize(4);
        lod.points[1].pos = glm::vec3(1, 0, 0);
        lod.points[2].pos = glm::vec3(0, 1, 0);
        lod.points[3].pos = glm::vec3(1, 1, 0);
        lod.normals = { glm::vec3(0, 0, 1) };
        for (uint32_t f = 0; f < 2; ++f)
        {
            P3DFace face;
            face.count = 3;
            face.v[0] = { f, 0, 0, 0 };
            face.v[1] = { f + 1, 0, 1, 0 };
            face.v[2] = { f + 2, 0, 0, 1 };
            face.texture = "orig_co.paa";
            lod.faces.push_back(face);
        }
        for (int i = 0; i < 2; ++i)
        {
            P3DSelection sel;
            sel.name = i == 0 ? "s1" : "s2";
            sel.faceBytes = { uint8_t(i == 0), uint8_t(i == 1) };
            sel.faceCount = 1;
            lod.selections.push_back(sel);
        }
        am.model.lods.push_back(lod);
        auto texturesOf = [&](const ConfigSkin* k, int& hidden) {
            std::string list;
            hidden = k ? RebuildWithHiddenSelections(am, k->selections, k->textures, k->materials) : -1;
            for (const auto& s : am.sections) list += (list.empty() ? "" : ",") + s.texture + "/" + std::to_string(s.count / 3);
            return list;
        };
        int h1 = 0, hAll = 0, hShort = 0, hMat = 0;
        const std::string t1 = texturesOf(hide1, h1), tAll = texturesOf(hideAll, hAll), tShort = texturesOf(shortTex, hShort), tMat = texturesOf(matEmpty, hMat);
        std::string mt, tx = "orig_co.paa";
        const bool matKeeps = matEmpty && SkinFace(*matEmpty, 0, tx, mt) && tx == "orig_co.paa" && mt.empty();
        // the default skin of a model: the first class that sets something, not a hide-only one listed before it
        int defaultSkin = -2;
        size_t listed = 0;
        {
            const fs::path root = dir / "skin_root";
            fs::create_directories(root / "av", ec);
            std::ofstream(root / "av" / "config.cpp", std::ios::binary | std::ios::trunc)
                << "class CfgVehicles {\n"
                   "  class AV_HideOnly { model = \"\\av\\m.p3d\"; hiddenSelections[] = { \"s1\" }; hiddenSelectionsTextures[] = { \"\" }; };\n"
                   "  class AV_Paint { model = \"\\av\\m.p3d\"; hiddenSelections[] = { \"s1\" }; hiddenSelectionsTextures[] = { \"x_co.paa\" }; };\n};\n";
            DiskFileSystem sdisk;
            EventBus sbus;
            Session sdoc(sdisk, sbus);
            sdoc.projectRoot = root;
            sdoc.model.path = root / "av" / "m.p3d"; // the path only
            LogCapture quiet;
            sdoc.LoadAddonConfig();
            defaultSkin = sdoc.activeSkin;
            listed = sdoc.configSkins.size();
        }
        Image proc;
        std::string perr;
        const bool alpha0 = MakeProceduralTexture("#(argb,8,8,3)color(1,0,0,0)", proc, perr) && !proc.mips.empty() && proc.mips[0][3] == 0;
        check(skins.size() == 4 && hide1 && hideAll && shortTex && matEmpty && t1 == "x_co.paa/1" && h1 == 1 && tAll.empty() && hAll == 2 &&
                  tShort == "y_co.paa/1,orig_co.paa/1" && hShort == 0 && tMat == "orig_co.paa/2" && hMat == 0 && matKeeps && alpha0 &&
                  listed == 2 && defaultSkin == 1,
              std::format("hiddenSelectionsTextures \"\": {} class(es) found (all-\"\" one {}), {{\"\", x}} -> {} ({} hidden), {{\"\", \"\"}} -> '{}' ({} hidden), "
                          "shorter -> {}, material \"\" -> {}, procedural alpha 0 {}, default skin {} of {} (not the hide-only one)",
                          skins.size(), hideAll ? "included" : "MISSING", t1, h1, tAll, hAll, tShort, tMat, alpha0 ? "ok" : "NOT 0", defaultSkin, listed));
    }
    // a model still being written (qt-req 1.7): while a hot reload retries (requireComplete) its read / parse problems are
    // no Errors (Debug); the last attempt (requireComplete off) reports them as Errors
    {
        struct HalfWritten : IFileSystem
        {
            bool Read(const fs::path&, std::vector<uint8_t>& out) const override
            {
                out = { 'M', 'L', 'O', 'D', 1, 1, 0, 0, 3, 0, 0, 0, 'P', '3', 'D', 'M' }; // header of an MLOD, cut short
                return true;
            }
            FileStamp Stamp(const fs::path&) const override { return {}; }
            bool IsWriteLocked(const fs::path&) const override { return false; }
        } half;
        P3DModel m;
        int retryErrors = 0, lastErrors = 0;
        bool retryRead = true, lastRead = true;
        {
            LogCapture cap;
            retryRead = Session::ReadModel(half, dir / "being_written.p3d", true, m);
            retryErrors = cap.Count(LogLevel::Error);
        }
        {
            LogCapture cap;
            lastRead = Session::ReadModel(half, dir / "being_written.p3d", false, m);
            lastErrors = cap.Count(LogLevel::Error);
        }
        check(!retryRead && retryErrors == 0 && !lastRead && lastErrors >= 1,
              std::format("half-written model: no Error while a retry follows ({} logged), Errors on the last attempt ({})", retryErrors, lastErrors));
    }
    // labels whose table row exists only with the "##id" (qt-req 1.3): the full label first, then the visible part
    {
        const UiLang was = GetUiLang();
        SetUiLang(UiLang::Ru);
        const std::pair<const char*, const char*> withId[] = {
            { "Turn 360##rec", "Поворот 360##rec" }, { "seconds##rec", "секунд##rec" }, { "format##rec", "формат##rec" }, { "size##rec", "размер##rec" },
            { "fps##rec", "fps##rec" }, { "Stop##rec", "Стоп##rec" }, { "Show in folder##reclast", "Показать в папке##reclast" }, { "...##recdir", "...##recdir" },
            { "Check##modcheck", "Проверить##modcheck" }, { "errors##mod", "ошибки##mod" }, { "warnings##mod", "предупреждения##mod" },
            { "only new##mod", "только новые##mod" }, { "...##modfolder", "...##modfolder" }, { "Put away##unit", "Убрать##unit" }, { "Close##replace", "Закрыть##replace" },
        };
        std::string bad;
        for (const auto& [en, ru] : withId)
            if (std::string(T(en)) != ru) bad += std::format(" {} -> {};", en, T(en));
        for (const char* plain : { "override", "blend" })
            if (std::string(T(plain)) == plain) bad += std::format(" {} untranslated;", plain);
        SetUiLang(was);
        check(bad.empty(), "string table: the 15 labels kept only with their ##id and override / blend translate to RU" + bad);
    }
    if (failuresOut) *failuresOut = failures;
    return std::format("{} check(s) failed{}", failures, out);
}
