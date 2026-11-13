#pragma once
// Opening files and hot reload of the open document: a .p3d is loaded by the viewer (it keeps its view across a
// reload), a .rtm becomes a layer (or updates its layer); the RTM folder is checked against the skeleton. The watch
// list follows what is loaded (model, model.cfg + includes, config.cpp, pivots, RTMs, RVMATs, textures, proxy / crew
// files); a change is re-read once the file is stable, a model-level change reloads the model once, a failed read is
// retried. Files are seen through the IFileSystem of the FileWatcher.
#include "FileWatcher.h"
#include "MaterialSet.h"
#include "MovesSim.h"
#include "Playback.h"
#include "RTM.h"
#include "Session.h"

#include <filesystem>
#include <string>
#include <vector>

// What hot reload asks of the viewer (implemented by App): the model reload keeps the view, the GPU caches are the
// viewer's, the bone coverage log needs the drawn LOD.
class IAssetView
{
public:
    virtual ~IAssetView() = default;
    virtual bool LoadModel(const std::filesystem::path& path, bool keepView, bool requireComplete) = 0; // now (hot reload)
    virtual void OpenModel(const std::filesystem::path& path, bool keepView) = 0; // in the background
    virtual bool TextureChanged(const std::filesystem::path& file) = 0;  // re-read; false: still broken
    virtual void GeometryChanged() = 0;                                  // RVMAT / proxy / crew changed: drawn geometry again
    virtual void AttachmentsChanged() = 0;                               // proxy slots + crew seats again
    virtual std::vector<std::filesystem::path> TextureFiles() const = 0; // textures the GPU cache read
    virtual void LogLayerCoverage(const AnimLayer& layer) const = 0;
    virtual void LogMissingBones(const std::string& rtmName, const std::vector<int>& bones, bool indent) const = 0;
};

class AssetSession
{
public:
    AssetSession(Session& session, Playback& playback, MaterialSet& mats, MovesSim& sim, const IFileSystem& files, IAssetView& view)
        : watcher(files), s_(session), pb_(playback), mats_(mats), sim_(sim), view_(view) {}

    FileWatcher watcher;
    bool watchDirty = true;                  // the watch list is rebuilt on the next Poll
    std::filesystem::path rtmFolder;         // RTMs checked against the skeleton
    std::vector<RtmBoneCheck> rtmChecks;

    void OpenFile(const std::filesystem::path& path);  // .p3d / .gltf / .glb / .rtm
    bool ReloadRtm(const std::filesystem::path& file); // every layer (and CfgMoves state) playing it
    void RunRtmCheck();
    void Poll(double clock);                           // hot reload: changed files re-read

private:
    void UpdateWatchList();

    Session& s_;
    Playback& pb_;
    MaterialSet& mats_;
    MovesSim& sim_;
    IAssetView& view_;
};
