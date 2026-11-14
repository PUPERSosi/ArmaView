# Embedding the viewer in another UI (Qt + ImGui in the viewport)

The view logic no longer depends on the viewer's ImGui windows or on `UiShell`. What another UI needs to provide is
three small pieces; everything else (core, renderer, view classes) is reused as it is.

The UI-free part lives in the static library **ArmaViewViewCore** (`src/view`, no ImGui / Win32 / D3D: CMake refuses
to configure when it links them, `ViewCoreGuard.h` stops their headers):

| File | What it is |
|---|---|
| `AppMode.h` | `enum class AppMode` + `AppModeName()` - the viewer's modes, no dependencies |
| `IStatesGraphHost.h` | what the animation view asks of the window showing the CfgMoves state graph |
| `InputState.h` | one frame of input: mouse, buttons, wheel, modifiers, held letter keys (physical), "over scene", "text field has the keyboard" |
| `SceneInteraction.h` | orbit / pan / zoom, click picking, bone selection and framing, the control mode step (WASD actions, Q / E, root motion, camera follows) - reads only `InputState` |
| `PointOverlaySettings.h` | what the point overlay shows; `PointOverlay::Settings()` / `SetSettings()` |

## 1. States graph window: `IStatesGraphHost`

```cpp
class MyStatesDock : public IStatesGraphHost
{
public:
    void SetGraph(const MovesGraph* graph, const std::filesystem::path& positionsFile) override { /* rebuild your view, null = none */ }
    void Show(bool on) override { dock_->setVisible(on); }
    void Toggle() override { dock_->setVisible(!dock_->isVisible()); }
    void ShowState(int state) override { Show(true); /* centre on graph->states[state] */ }
};
```

`AnimationView` takes it in its constructor instead of the ImGui `StatesPanel` (which is one implementation of it).

## 2. The current mode: a source of `AppMode`

`Recorder` takes `std::function<AppMode()>`, `WeaponScene` asks its `IWeaponSceneHost::Mode()` / `SetMode(AppMode)`:

```cpp
Recorder recorder{ ..., [this] { return currentMode_; }, host, pickFolder };
```

## 3. Input: an `InputState` adapter

`view/SceneInput` is the ImGui adapter (it fills an `InputState` from ImGui's IO; the ui-script and raw keys arrive
through ImGui's IO, so through the same path). A Qt adapter fills the same structure from its events once per frame
and calls `SceneInteraction`:

```cpp
// a QWidget / QWindow showing the scene; host: your ISceneInteractionHost (picks, inspector, bone selection, grid, CfgMoves)
SceneInteraction interaction{ camera, host };

void MyViewport::frame(float dt)
{
    InputState in;
    in.mouseX = mouse_.x(); in.mouseY = mouse_.y();          // window pixels; originX / originY = 0 for a widget
    in.mouseDX = mouse_.x() - last_.x(); in.mouseDY = mouse_.y() - last_.y();
    in.wheel = wheelSteps_;                                   // QWheelEvent::angleDelta().y() / 120
    for (int b = 0; b < 3; ++b) { in.down[b] = held_[b]; in.pressed[b] = went_down_[b]; in.released[b] = went_up_[b]; }
    in.ctrl = mods_ & Qt::ControlModifier; in.shift = mods_ & Qt::ShiftModifier; in.alt = mods_ & Qt::AltModifier;
    for (char c = 'A'; c <= 'Z'; ++c) in.SetLetter(c, lettersHeld_.contains(c)); // from nativeScanCode(): the physical key
    in.overScene = underMouse();                              // not over an ImGui window drawn inside the viewport
    in.textInput = ImGui::GetIO().WantTextInput;              // or your own text field focus

    interaction.Mouse(in, SceneRect{ 0, 0, float(width()), float(height()) }, camera.Projection(aspect) * camera.View());
    if (interaction.Control(in, dt, control, sim, playback, session.moves)) posePending_ = true;
    wheelSteps_ = 0; went_down_ = {}; went_up_ = {};
}
```

`ISceneInteractionHost` (in `SceneInteraction.h`) is what the interaction changes in your viewer: point / face picking,
which item the inspector shows, the selected bone and what it moves (for framing), the floor grid, and the CfgMoves
player (`MovesPlay`, `SetMovesDrive`). `view/SceneInput.cpp` is a complete example implementation.

## Point overlay settings

```cpp
PointOverlaySettings s = points.Settings();
s.hitpointsView = true;
s.filter = "wheel";
points.SetSettings(s);           // no ImGui widget needed; stored in the settings file ("points" block) the same way
```

## Checks

`armaview-cli selftest` (and the viewer's `coretest`) run the view checks without any UI: W walks forward, Q turns
left, a click picks, bone selection, drag / wheel on the camera, the overlay settings round trip.
