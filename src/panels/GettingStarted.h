#pragma once
// Getting started: scenarios step by step. A step switches to its mode, brings its panel to the front and highlights the
// control to use (yellow frame, ui::SetHighlight).
#include "ViewHost.h"

class GettingStarted
{
public:
    explicit GettingStarted(IViewHost& host) : host_(host) {}
    void Draw(float dpi);
    void Go(int scenario, int step); // scenario < 0: stop the guide
    int Scenario() const { return guide_; }
    int Step() const { return step_; }

    bool open = false;

private:
    IViewHost& host_;
    int guide_ = -1, step_ = 0;
};
