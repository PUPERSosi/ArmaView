#pragma once
// Viewer-only visibility per model: hidden selections / proxies / components, isolation, hidden faces, temporary
// named face sets and LOD overlays. Saved in the viewer's own settings file (never in the P3D).
#include "Settings.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

struct P3DLod;

struct LodVisibility
{
    std::set<std::string> hidden;                        // lower-case selection / set names
    std::set<uint32_t> hiddenFaces;                      // faces hidden from a mouse pick
    std::string isolate;                                 // selection / set name ("#faces": isolateFaces)
    std::vector<uint32_t> isolateFaces;
    std::map<std::string, std::vector<uint32_t>> sets;   // temporary named face sets (name as typed)
    bool meshHidden = false;                             // eye of the LOD itself
    bool Empty() const { return hidden.empty() && hiddenFaces.empty() && isolate.empty() && sets.empty() && !meshHidden; }
};

class ModelVisibility
{
public:
    // the "models" block of the settings (shared by all models); model: key of this model's lines
    void Load(Settings& settings, const std::filesystem::path& model);
    void Save() const; // this model's lines replaced, the settings file written
    LodVisibility& Lod(float resolution);
    const LodVisibility* Find(float resolution) const;
    std::set<std::string> overlays; // resolutions (as keys) of LODs drawn as a wire overlay
    static std::string Key(float resolution);
    // per face: 1 = hidden (hidden names, hidden faces, isolation, LOD eye)
    std::vector<uint8_t> HiddenFaces(const P3DLod& lod, float resolution) const;
    // faces of a selection or a temporary set
    static bool FaceInNamed(const P3DLod& lod, const LodVisibility& v, const std::string& name, uint32_t face);
    bool AnyHidden(float resolution) const;
    void ShowAll(float resolution);

    // another visibility in place of this model's for a while (the Icons mode's own hiding): the state as a whole,
    // put back later; persist = false keeps it out of the settings file meanwhile
    struct Snapshot
    {
        std::map<std::string, LodVisibility> lods;
        std::set<std::string> overlays;
    };
    Snapshot Take() const { return { lods_, overlays }; }
    void Restore(const Snapshot& s) { lods_ = s.lods; overlays = s.overlays; }
    bool persist = true;
    // hidden names / isolation / LOD eye / hidden faces of every LOD as one line ("" when nothing) and back
    static std::string SnapshotText(const Snapshot& s);
    static Snapshot SnapshotFromText(const std::string& text);

private:
    Settings* settings_ = nullptr;
    std::filesystem::path model_;
    std::map<std::string, LodVisibility> lods_;
};
