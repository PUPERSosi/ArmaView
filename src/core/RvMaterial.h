#pragma once
// RVMAT materials: parse, save edited values back, config classes that re-texture a model.
#include "ArmaConfig.h"
#include "Texture.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct UvTransform
{
    glm::vec2 aside{ 1, 0 }, up{ 0, 1 }, pos{ 0, 0 };
    int source = 0; // 0 = "tex" (UV set 0), 1 = "tex1" (UV set 1)
};

struct RvStage
{
    int index = 0;
    std::string texture; // as written
    std::string uvSourceName;
    UvTransform uv;
    TexRole role = TexRole::Count;
    std::string uvClass;              // class holding uvSource / uvTransform: "StageN" or "TexGenN"
    std::vector<float> asideRaw, upRaw, dirRaw, posRaw; // uvTransform arrays as written (3 components)
};

struct RvMaterial
{
    std::filesystem::path path;
    bool loaded = false;
    std::string error;
    glm::vec4 ambient{ 1 }, diffuse{ 1 }, forcedDiffuse{ 0 }, emissive{ 0 }, specular{ 0 };
    float specularPower = 1.0f;
    std::string pixelShader, vertexShader;
    std::vector<RvStage> stages;
    int errors = 0; // config parse errors (hot reload treats > 0 as a half-written file)
    // reflections of the super shader: procedural #(ai,..)fresnel(n,k) stage and an environment map stage
    // (useWorldEnvMap / env_*.paa); the viewer replaces the env map by a sky built from the current lighting
    bool hasFresnel = false, hasEnvMap = false;
    float fresnelN = 1.5f, fresnelK = 0.0f;
    bool rapified = false;  // binary RVMAT: read only
    int64_t fileTime = 0;   // modification time when loaded (kept across model reloads while unchanged)
    bool includes = false;  // #include / macros pulled in other files: entries from them are not edited
    // Where each entry is written in the file (1-based line), key "ambient", "stage1.texture",
    // "texgen0.uvtransform.aside" ... (lower case); and the first line of each class ("stage1").
    std::map<std::string, int> entryLines, classLines;
};

// Writes the values that differ between `original` (as loaded from disk) and `edited` back into the text RVMAT:
// only the value text of each changed entry is replaced, so order, comments and formatting stay; entries missing in
// the file are added to their class. The previous file is kept as <name>.rvmat.bak. Binary RVMATs are refused.
bool SaveRvMaterial(const RvMaterial& original, const RvMaterial& edited, std::string& error, int& changed);

// Parses an .rvmat (text config). Problems are logged with file(line).
bool LoadRvMaterial(const std::filesystem::path& path, const std::filesystem::path& projectRoot, RvMaterial& out);

// A config class that uses this model and re-textures it: hiddenSelections + hiddenSelectionsTextures/Materials.
struct ConfigSkin
{
    std::string cfgRoot, className;   // e.g. CfgVehicles, B_Soldier_F
    std::vector<std::string> selections, textures, materials;
    std::string where;                // file(line)
    std::filesystem::path file;
    int line = 0;
};

// Scans every class one level below the root Cfg* classes for model == modelPath (Arma path, any case/extension form).
// A class counts when it has hiddenSelections and a hiddenSelectionsTextures or hiddenSelectionsMaterials array, even if
// every entry is "" (a re-texture that hides the selections).
std::vector<ConfigSkin> FindConfigSkins(const ArmaConfig& cfg, const std::string& modelPath);
// Hidden selection i of a class on a face (as the game, and setObjectTexture [i, ""]): texture "" hides the face; a set
// texture / material replaces the face's; an index past an array (or no array) keeps the model's own; material ""
// changes nothing. false: the face is not drawn.
bool SkinFace(const std::vector<std::string>& textures, const std::vector<std::string>& materials, int i, std::string& texture, std::string& material);
inline bool SkinFace(const ConfigSkin& s, int i, std::string& texture, std::string& material) { return SkinFace(s.textures, s.materials, i, texture, material); }
std::string NormalizeArmaPath(std::string_view p);
// CfgVehicles class whose model= is modelPath (own entry preferred over an inherited one); null if none.
const CfgClass* FindVehicleClass(const ArmaConfig& cfg, const std::string& modelPath); // lower case, '\', no leading '\', ".p3d" added if no extension

