#pragma once
// P3D MLOD (editable, non-binarized) model reader.
// Layout: "MLOD" ver nLods, then per LOD: "P3DM" 28 256 nPoints nNormals nFaces flags,
// points[16B], normals[12B], faces, "TAGG" + taggs until #EndOfFile#, float resolution.
#include <glm/glm.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Shown for a binarized (game) model or animation: only the source formats are read.
inline constexpr const char* kBinarizedNotSupported =
    "Binarized files are not supported — open the unbinarized source (MLOD / RTM)";

struct P3DPoint
{
    glm::vec3 pos;
    uint32_t flags;
};

struct P3DFaceVertex
{
    uint32_t point;
    uint32_t normal;
    float u, v;
};

struct P3DFace
{
    uint32_t count = 0; // 3 or 4
    P3DFaceVertex v[4]{};
    uint32_t flags = 0;
    std::string texture;
    std::string material;
};

struct P3DSelection
{
    std::string name;
    std::vector<uint8_t> pointBytes; // raw per-point byte: 0 = not selected, 1 = weight 1.0, 2..255 = (256-b)/255
    std::vector<uint8_t> faceBytes;  // per-face byte: non-zero = selected
    size_t pointCount = 0;   // points with weight > 0
    size_t partialCount = 0; // points with 0 < weight < 1
    size_t faceCount = 0;
    bool engineIgnored = false; // name starts with '-' or '.': the engine skips it when loading (CWR ShapeSetup.cpp:928)

    // Encoding verified on a custom character MLOD: weights of all bone selections sum to 1.0 per point (max dev 0.008).
    static float DecodeWeight(uint8_t b) { return b == 0 ? 0.0f : b == 1 ? 1.0f : float(256 - b) / 255.0f; }
    float Weight(size_t point) const { return point < pointBytes.size() ? DecodeWeight(pointBytes[point]) : 0.0f; }
};

struct P3DProperty
{
    std::string name, value;
};

struct P3DUVSet
{
    uint32_t stage = 0;
    std::vector<glm::vec2> uv; // one per face corner, faces in order, face.count corners each
};

struct P3DLod
{
    float resolution = 0.0f;
    uint32_t versionMajor = 0, versionMinor = 0, flags = 0;
    std::vector<P3DPoint> points;
    std::vector<glm::vec3> normals;
    std::vector<P3DFace> faces;
    std::vector<P3DSelection> selections;
    std::vector<P3DProperty> properties;
    std::vector<float> mass;                                   // #Mass#, per point
    std::vector<std::pair<uint32_t, uint32_t>> sharpEdges;     // #SharpEdges#, point index pairs
    std::vector<P3DUVSet> uvSets;                              // #UVSet#
    size_t animationFrames = 0;                                // #Animation# count (not stored)
    std::vector<std::string> skippedTaggs;                     // "name (N bytes)"

    std::string Name() const;
    size_t TriangleCount() const;
    float TotalMass() const;
};

// An animation clip of an imported model (glTF): model-space deformation matrices per frame and bone (as RTM_0101).
struct ImportedClip
{
    std::string name;
    float duration = 0.0f;              // seconds
    std::vector<float> times;           // phase 0..1 per frame
    std::vector<glm::mat4> poses;       // frames x bones (P3DModel::importedBones order)
};

struct P3DModel
{
    std::filesystem::path path;
    uint32_t version = 0;
    std::vector<P3DLod> lods;
    bool complete = false; // all LODs from the header were read (false: truncated / half-written file)
    // imported formats (glTF): the format name, and the skeleton and clips that travel with the model (no model.cfg)
    std::string format;                                               // empty: MLOD
    std::string importedSkeleton;
    std::vector<std::pair<std::string, std::string>> importedBones;   // bone, parent ("" root)
    std::vector<ImportedClip> importedClips;
};

// Recomputes normals from geometry, one per face corner (index face*4 + corner).
// Corners around a point are averaged (area-weighted) across faces connected by edges;
// with useSharpEdges, edges listed in #SharpEdges# split the smoothing.
// Face orientation follows the file convention: cross(p1 - p0, p2 - p0) points along the stored normal.
std::vector<glm::vec3> ComputeCornerNormals(const P3DLod& lod, bool useSharpEdges);

// Name for a LOD resolution value (e.g. 1e13 -> "Geometry", 1.0 -> "1.000").
std::string LodResolutionName(float resolution);

// Loads an MLOD file. Never throws; problems go to the Log. Returns false if nothing usable was read.
// On a mid-file error the LODs parsed so far are kept.
bool LoadP3D(const std::filesystem::path& path, P3DModel& out);
bool LoadP3D(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, P3DModel& out); // the file already read
