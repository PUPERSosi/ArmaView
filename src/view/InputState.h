#pragma once
// One frame of input for the scene, independent of where it comes from: the ImGui adapter (view/SceneInput) fills it
// from ImGui's IO, ui-script and raw keys reach it the same way, another UI (Qt events) can fill it as well.
// SceneInteraction reads only this. No ImGui.
#include <array>

struct InputState
{
    // mouse, in the coordinates of the window (screen pixels as the UI reports them); origin = the window's client corner
    float mouseX = 0, mouseY = 0;
    float mouseDX = 0, mouseDY = 0; // move since the last frame
    float wheel = 0;                // wheel steps this frame (+ = away from the user)
    float originX = 0, originY = 0;
    enum Button { Left, Right, Middle };
    std::array<bool, 3> down{}, pressed{}, released{}; // held / went down this frame / went up this frame
    bool ctrl = false, shift = false, alt = false;
    // letter keys held, by physical key: the US letter at that place (A..Z), whatever the keyboard layout
    std::array<bool, 26> letters{};
    bool overScene = false; // the mouse is over the 3D view (not over a window of the UI)
    bool textInput = false; // a text field has the keyboard: no key does anything in the scene

    bool Letter(char c) const { return c >= 'A' && c <= 'Z' && letters[size_t(c - 'A')]; }
    void SetLetter(char c, bool held) { if (c >= 'A' && c <= 'Z') letters[size_t(c - 'A')] = held; }
};
