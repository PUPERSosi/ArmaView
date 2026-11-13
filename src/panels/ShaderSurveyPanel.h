#pragma once
// The PixelShaderID survey of the work drive as a table (in the Materials window).
#include "Session.h"
#include "ShaderSurvey.h"
#include "ViewHost.h"

class ShaderSurveyPanel
{
public:
    ShaderSurveyPanel(Session& session, ShaderSurvey& survey, IViewHost& host) : s_(session), survey_(survey), host_(host) {}
    void Draw();

private:
    Session& s_;
    ShaderSurvey& survey_;
    IViewHost& host_;
};
