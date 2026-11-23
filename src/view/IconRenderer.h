#pragma once
// The Icons mode's picture of the open model (MLOD or glTF): the scene drawn offscreen with the camera of the preset
// (IconStandards), supersampled, then fitted into the picture with BI's free border, coloured, as a white silhouette
// or on the VR ground of the editor previews. No UI: the Icons panel and the ui-script both use it.
#include "IconImage.h"
#include "IconStandards.h"
#include "DisplaySettings.h"
#include "Lighting.h"
#include "Renderer.h"
#include "SceneView.h"
#include "Session.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <functional>
#include <string>

struct IconSettings
{
    IconPreset preset = IconPreset::Weapon;
    int width = 1024, height = 512;
    IconBackground background = IconBackground::Transparent;
    bool whiteSilhouette = false;
    float yawDeg = 90.0f, pitchDeg = 0.0f, fovDeg = 6.0f;
    float marginL = 0.05f, marginR = 0.03f, marginT = 0.05f, marginB = 0.05f;
    IconAlign align = IconAlign::Center;
    bool characterCloseUp = false;  // editor preview of a character: the torso, as BI's
    // weapons / attachments: the view across the barrel with the muzzle on the left (from the memory points
    // muzzlePos -> muzzleEnd, else the longer horizontal axis); yawDeg turns from there
    bool barrelSide = false;
    // the camera beyond the preset: roll; zoom (1: the fitted distance) and the target moved (metres, model axes) - these
    // two frame the picture themselves (fitToBorder off: no cut to the item's bounds)
    float rollDeg = 0.0f, zoom = 1.0f;
    float offsetX = 0.0f, offsetY = 0.0f, offsetZ = 0.0f;
    bool fitToBorder = true;
    float exposure = 0.0f, saturation = 1.0f, contrast = 1.0f;
    int outline = 0;                 // pixels of the final picture
    uint8_t outlineColor[4] = { 0, 0, 0, 200 };
    int supersample = 4;
};
// the BI numbers of a preset; vehicle / character: the editor preview's camera (BI turns vehicles and props differently)
IconSettings DefaultIconSettings(IconPreset p, bool vehicle = true, bool character = false);

class IconRenderer
{
public:
    // the front end's SceneDraw of a view rectangle (light, layers, sun shadow) with other display settings
    using FillScene = std::function<void(SceneDraw&, const LightingResult&, float x, float y, float w, float h, const DisplaySettings*)>;
    IconRenderer(Session& session, SceneView& view, Renderer& renderer, FillScene fill)
        : s_(session), view_(view), renderer_(renderer), fill_(std::move(fill)) {}
    // the open model's picture; false + error: no model, nothing visible, the offscreen target failed
    bool Render(const IconSettings& settings, RgbaImage& out, std::string& error);
    // a texture of the picture for a UI (kept until the next Upload)
    ID3D11ShaderResourceView* Upload(const RgbaImage& img);
    // the last picture's camera: its right / up axes and the model's size (moving the target with the mouse)
    glm::vec3 lastRight{ 1, 0, 0 }, lastUp{ 0, 1, 0 };
    float lastSize = 1.0f;

private:
    Session& s_;
    SceneView& view_;
    Renderer& renderer_;
    FillScene fill_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> preview_;
};
