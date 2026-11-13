#pragma once
// Validation: the rig of the current LOD (weights, selections, points), model.cfg animations / skins, RTM bones of a
// folder against the skeleton, the loop seam, crew problems; the CfgMoves section comes from the Moves panel.
#include "RTM.h"
#include "Session.h"
#include "ViewHost.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// What the Validation panel reads of the view besides the document (implemented by App).
class IValidationView
{
public:
    enum class PointProblem { None, TooManyBones, BadWeightSum, NoBone };
    // What a bone actually moves in the current LOD: mesh vertices vs proxy points (proxy triangles)
    struct BoneUsage
    {
        size_t meshPoints = 0;
        std::vector<std::string> proxies; // proxy paths whose triangle uses the bone
    };
    virtual ~IValidationView() = default;
    virtual const ValidationReport& LodReport() const = 0;    // the current LOD against the skeleton
    virtual PointProblem HighlightedProblem() const = 0;
    virtual void HighlightProblem(PointProblem p) = 0;        // red points on the model (None: off)
    virtual BoneUsage UsageOfBone(int bone) const = 0;
    virtual const std::vector<SeamJump>& LoopSeam() const = 0; // of the primary animation layer
    virtual int PrimaryLayer() const = 0;                      // -1: no layer
    virtual const std::filesystem::path& RtmFolder() const = 0;
    virtual const std::vector<RtmBoneCheck>& RtmChecks() const = 0;
    virtual void PickRtmFolder() = 0;
    virtual void RecheckRtm() = 0;
};

class ValidationPanel
{
public:
    using PointProblem = IValidationView::PointProblem;
    ValidationPanel(Session& session, IViewHost& host, IValidationView& view) : session_(session), host_(host), view_(view) {}
    void Draw(const std::string& windowName, const std::function<void()>& movesSection);

private:
    Session& session_;
    IViewHost& host_;
    IValidationView& view_;
};
