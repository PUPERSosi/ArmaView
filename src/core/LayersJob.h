#pragma once
// The animation layers project as a job on the open document: the base clip (an RTM, or "#retarget" = the Retarget
// result) with clip operations and correction layers baked into one clip, previewed as the only layer of the
// Session's AnimStack, written as a NEW RTM_0101 (a .bak when overwriting) and read back, or a whole folder in a batch.
#include "AnimProject.h"
#include "RTM.h"
#include "RetargetJob.h"
#include "Session.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class LayersJob
{
public:
    LayersJob(Session& session, RetargetJob& retarget) : s_(session), rt_(retarget) {}

    AnimProject proj;
    std::shared_ptr<RtmAnimation> base, baked;
    std::string msg;
    char src[400] = {}, out[400] = {}, batchDir[400] = {}, batchOut[400] = {};
    bool previewOn = false;
    bool previewAB = true;     // the preview shows the base clip as B of the A/B compare
    bool overwriteAsk = false; // the output exists: the panel asks before overwriting
    char blenderExe[400] = {}, blendFile[400] = {}, blendArmature[64] = {}; // Blender bridge (a3ob import)

    // Batch: every .rtm of batchDir through the project into batchOut, one file per step (the UI shows the progress)
    struct BatchProgress
    {
        bool running = false;
        int index = 0, total = 0, written = 0, skipped = 0, failed = 0;
        std::string current; // file name being processed
    } batch;

    // an RTM in the model space of the open skeleton ("#retarget": the Retarget result)
    std::shared_ptr<RtmAnimation> LoadModelSpace(const std::string& path);
    bool Bake();                    // src + project -> baked (no preview)
    void StartPreview();            // the baked clip as the only layer (tag 4); the layers before come back on Stop
    void StopPreview();
    bool Write(const std::filesystem::path& file, bool overwrite);
    bool BatchStart();              // lists the files; false: no such folder (msg)
    bool BatchStep();               // the next file; false: done (msg = summary)
    void BatchCancel();
    void Batch();                   // start + every step (scripts); never overwrites
    std::string ProjectText() const { return AnimProjectText(proj); }
    std::filesystem::path ProjectFile() const;  // <base clip>.avanim.json next to the base clip (or the model)
    bool SaveProject();
    bool LoadProject();
    bool ProjectDirty() const { const std::string t = ProjectText(); return !t.empty() && t != savedText_; }
    void MarkProjectSaved() { savedText_ = ProjectText(); }
    // Blender: the baked clip as a temp RTM + an import script (a3ob into the armature); run starts Blender (platform):
    // exe, command line arguments, wait for it, its output. background: waits and checks the import (test run).
    using BlenderRunner = std::function<bool(const std::wstring& exe, const std::wstring& args, bool wait, std::string& output)>;
    bool OpenInBlender(bool background, const BlenderRunner& run);
    float BakedDuration() const;    // the preview layer's cycle (trim / speed applied)

private:
    Session& s_;
    RetargetJob& rt_;
    std::vector<AnimLayer> saved_;
    std::string savedText_;
    std::vector<std::filesystem::path> batchFiles_;
};
