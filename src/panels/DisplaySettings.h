#pragma once
// How the scene is drawn (shading, wireframe, overlays, shadows, background): view settings, owned by App, set by the
// toolbar, the View / Light popups and the hotkeys, read by the renderer setup each frame.
#include "Renderer.h"

enum class NormalsMode { FromFile, Recomputed };
enum class FaceDiag { None, MirroredUV, FlippedNormals, RvmatPerFace, TexturePerFace, Count };
const char* FaceDiagName(FaceDiag d);

struct DisplaySettings
{
    ShadingMode shading = ShadingMode::Textured;
    WireMode wire = WireMode::Off;
    CullMode cull = CullMode::None;
    NormalsMode normalsMode = NormalsMode::FromFile;
    FaceDiag faceDiag = FaceDiag::None;  // vertex colours over any shading mode
    bool twoSided = true;
    bool showGrid = true;
    bool showNormals = false;
    bool showProxies = false;
    bool showSkeleton = true;
    bool skeletonOnTop = true;
    bool skeletonOnly = false;           // hide mesh and floor grid
    bool showAxes = true;
    bool axisLabelsBlender = false;      // gizmo names: Arma (Y-up) or Blender (Z-up)
    float normalLength = 0.03f;
    bool reflections = true;             // env map (sky) x fresnel of super-shader materials
    bool shadows = false;
    float shadowSoftness = 1.5f;
    int shadowQuality = 1;
    bool backgroundFromSky = true;
    float background[4] = { 0.20f, 0.22f, 0.25f, 1.0f };
    bool flipNormalGreen = false;
    bool applySwizzle = true;            // PAA SWIZ on normal maps (off = old behaviour, A/B)
};
