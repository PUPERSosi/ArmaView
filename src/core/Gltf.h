#pragma once
// glTF 2.0 import (.gltf / .glb, read with cgltf): meshes (positions, normals, first UV set), materials (base color
// texture or factor, normal texture), the skeleton of the skins (joints, inverse bind matrices, vertex weights) and the
// animations (translation / rotation / scale channels of the joints), resampled at 30 fps.
// The result is the viewer's own model data: LOD 0 = every triangle primitive (non-skinned meshes baked with their node
// transform), a Memory LOD = one point per joint (the bone pivots), one selection per joint with the vertex weights, the
// skeleton and the clips in P3DModel::imported* (model-space deformation matrices per frame, as RTM_0101 stores them).
// Axes: glTF is right-handed (+X left of a model facing +Z), the viewer left-handed (+X right): x is mirrored and the
// triangle order reversed. Textures (embedded or external) are written to <tempdir>\ArmaView\gltf\<name>\ with the
// suffixes the viewer knows (_co, _nohq); a normal texture gets a small generated RVMAT there.
#include "P3D.h"
#include "RTM.h"

#include <filesystem>
#include <memory>
#include <string>

bool IsGltfFile(const std::filesystem::path& path); // .gltf / .glb
// False + error when the file cannot be read; warnings (skipped primitives, channels ...) go to the log.
bool LoadGltf(const std::filesystem::path& path, P3DModel& out, std::string& error);
// A clip of an imported model as an animation layer (bones = the skeleton's names, path "<file>#<clip>").
std::shared_ptr<RtmAnimation> ImportedClipAnimation(const P3DModel& model, const ImportedClip& clip);
// Writes a small self-contained glTF (selftest): a 2 m bar on two joints (bone_base, bone_tip at 1 m), weights 1 / 0.5 /
// 1 over its three rings, an embedded 2x2 base color PNG and a flat normal PNG, one clip "bend": bone_tip turns 90
// degrees about +Z (glTF axes) in 1 s.
bool WriteGltfSample(const std::filesystem::path& file);
