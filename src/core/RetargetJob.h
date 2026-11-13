#pragma once
// Retargeting as a job on the open document: the source rig (a character of another skeleton), the bone mapping
// (automatic by name + presets per skeleton pair), the retargeted animation as a preview layer of the Session's
// AnimStack (the previous layers come back on Stop), and the export to a NEW RTM_0101 that is read back and compared.
#include "RTM.h"
#include "Retarget.h"
#include "Session.h"

#include <filesystem>
#include <string>
#include <vector>

class RetargetJob
{
public:
    explicit RetargetJob(std::filesystem::path presetDir) : presetDir_(std::move(presetDir)) {}
    void SetPresetDir(std::filesystem::path dir) { presetDir_ = std::move(dir); }

    // settings (edited by the Retarget panel, ui-script)
    char srcModel[400] = {}; // source character .p3d (default: the samples man)
    char anim[400] = {};     // source .rtm
    char out[400] = {};      // output .rtm (a new file)
    bool outAuto = true;     // out was proposed by the job (a new free name on the next run)
    float duration = 1.0f;   // cycle seconds of the preview layer
    int previewMode = 1;     // source skeleton: 0 hidden, 1 beside, 2 overlaid
    RetargetOptions opt;

    // state
    RetargetRig src;
    BoneMapping map;
    RetargetResult res;
    std::string msg;         // last result / error for the panel
    bool previewOn = false;  // the result is the only layer of session.anim

    std::filesystem::path PresetFile(const Skeleton& target) const;
    void LoadSource(const Session& s);   // rig of srcModel + automatic mapping (+ the preset of the pair)
    bool Prepare(const Session& s);      // reads anim and retargets it onto s.skeleton (no preview yet)
    void StartPreview(Session& s);       // the result becomes the only animation layer (tag 2)
    void StopPreview(Session& s);        // the layers from before StartPreview come back
    bool Export(bool overwrite);         // overwrite: the old file moves to .bak (.bak2...), never lost

private:
    std::filesystem::path presetDir_;
    std::vector<AnimLayer> saved_;
};
