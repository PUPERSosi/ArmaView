#pragma once
// What the animation commands do to the view around the core: game mode on / off, playMove / switchMove, reset, the
// control mode, gestures with their mask heat map, the layers preview (bake, A/B with the base, Blender), the retarget
// run / preview (the source skeleton beside the model) / export, the CfgMoves graph applied to the States window.
// The windows (Moves, States, Move editor, Layers, Retarget) send these; the logic stays in the core services.
#include "Camera.h"
#include "LayersJob.h"
#include "LayersPanel.h"
#include "MovesLoader.h"
#include "MovesPanel.h"
#include "MovesSim.h"
#include "Paths.h"
#include "Playback.h"
#include "Renderer.h"
#include "RetargetJob.h"
#include "SceneView.h"
#include "Session.h"
#include "StatesPanel.h"
#include "ViewHost.h"

#include <functional>
#include <string>
#include <vector>

class AnimationView : public IMovesView, public ILayersView
{
public:
    AnimationView(Session& session, Playback& playback, MovesSim& sim, MovesLoader& loader, GesturePlayer& gestures, MovesControl& control, LayersJob& layers,
                  RetargetJob& retarget, SceneView& scene, Renderer& renderer, const OrbitCamera& camera, StatesPanel& states, const IPaths& paths,
                  IViewHost& host, std::function<bool()> gameConfigsLoading)
        : session_(session), playback_(playback), movesSim_(sim), movesLoader_(loader), gesturePlayer_(gestures), control_(control), layers_(layers),
          rt_(retarget), scene_(scene), renderer_(renderer), camera_(camera), statesPanel_(states), paths_(paths), host_(host),
          gameConfigsLoading_(std::move(gameConfigsLoading)) {}

    bool PollMovesBuild(bool wait);    // a finished graph applied: States graph, gestures stopped, watch list
    bool EnsureMoves(bool wait);
    void UpdateMovesPlayback();
    void UpdateRetargetPreview();      // per frame: the source skeleton lines beside the model
    bool RetargetRun();
    void RetargetStopPreview();
    bool RetargetExport(bool overwrite = false); // overwrite: the old file moves to .bak (.bak2 ... never overwritten)
    std::filesystem::path RetargetPresetFile() const;
    void RetargetLoadSource();

    // IMovesView
    void SetMovesDrive(bool on) override;
    // user: an explicit click / command; false (control mode keys, automatic) never enters or crosses a death state
    void MovesPlay(const std::string& stateOrAction, bool switchMove, bool user = true) override;
    void MovesReset() override;
    void SetControlMode(bool on) override;
    void PlayGesture(const std::string& name) override;
    void SetMaskHeat(bool on, const std::string& mask) override;
    bool MaskHeat() const override { return scene_.maskHeat; }
    const std::string& MaskView() const override { return scene_.maskView; }
    void ShowState(int state) override { statesPanel_.Show(state); }
    void ToggleStatesGraph() override { statesPanel_.open = !statesPanel_.open; }
    bool GameConfigsLoading() const override { return gameConfigsLoading_(); }

    // ILayersView
    bool LayersPreview() override;     // bake + the result as the only layer, the base as B
    void LayersStopPreview() override;
    void ShowMaskHeat(std::vector<float> boneWeights) override;
    bool LayersOpenInBlender() override;

private:
    Session& session_;
    Playback& playback_;
    MovesSim& movesSim_;
    MovesLoader& movesLoader_;
    GesturePlayer& gesturePlayer_;
    MovesControl& control_;
    LayersJob& layers_;
    RetargetJob& rt_;
    SceneView& scene_;
    Renderer& renderer_;
    const OrbitCamera& camera_;
    StatesPanel& statesPanel_;
    const IPaths& paths_;
    IViewHost& host_;
    std::function<bool()> gameConfigsLoading_;
    bool rtLinesShown_ = false; // the source skeleton lines are on screen
};
