#pragma once
// The RVMATs of the open model: loaded once each (key: the path as referenced, lower case), pending edits over them
// (what the view previews), revert, save into the text RVMAT keeping its format (.bak kept; binary ones are refused),
// and the lists the material editor offers (shader ids, uv sources).
#include "RvMaterial.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

class MaterialSet
{
public:
    explicit MaterialSet(const std::filesystem::path& projectRoot) : root_(projectRoot) {}

    std::map<std::string, RvMaterial> loaded; // as on disk
    bool showOriginal = false;   // A/B: the scene shows the originals, not the edits (M)
    std::map<std::string, RvMaterial> edits;  // pending (not saved) edits
    std::string saveMsg;                      // result of the last save

    const RvMaterial* Get(const std::string& name);
    void Prefetch(const std::vector<std::string>& names); // the ones not loaded yet, read + parsed in parallel              // loads on first use (a "not found" entry when missing)
    const RvMaterial* Current(const std::string& name, const RvMaterial* original) const; // the edit, else original
    bool HasEdit(const std::string& name) const { return edits.count(Key(name)) != 0; }
    void Edit(const std::string& name, const RvMaterial& m) { edits[Key(name)] = m; }
    void Revert(const std::string& name) { edits.erase(Key(name)); saveMsg.clear(); }
    // writes the changed values of the edit into the RVMAT text; true when something was written (cache dropped)
    bool Save(const std::string& name, const RvMaterial& original);
    int DropChanged();                                          // entries whose file changed on disk (model reload)
    int DropFile(const std::filesystem::path& file);            // hot reload of one RVMAT

    static const std::vector<const char*>& PixelShaders();
    static const std::vector<const char*>& VertexShaders();
    static const std::vector<const char*>& UvSources();
    static void UpdateRole(RvStage& st);                        // texture role from the new texture name

private:
    static std::string Key(const std::string& name) { return ToLowerName(name); }
    static std::string ToLowerName(std::string s);
    const std::filesystem::path& root_;
};
