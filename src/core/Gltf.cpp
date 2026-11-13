#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include "Gltf.h"
#include "Log.h"
#include "Text.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <set>

#include "CoreGuard.h"

namespace fs = std::filesystem;

namespace {

constexpr float kFps = 30.0f;
constexpr int kMaxFrames = 30 * 600; // 10 minutes

// glTF (right-handed) <-> viewer (left-handed): x mirrored
const glm::mat4 kMirror = glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f));
glm::vec3 Mirror(const glm::vec3& v) { return { -v.x, v.y, v.z }; }

// cgltf file callbacks: paths are UTF-8, opened as wide paths (non-ASCII folders work)
cgltf_result ReadFile(const cgltf_memory_options*, const cgltf_file_options*, const char* path, cgltf_size* size, void** data)
{
    std::ifstream f(fs::path(Utf8ToWide(path)), std::ios::binary | std::ios::ate);
    if (!f) return cgltf_result_file_not_found;
    const std::streamoff n = f.tellg();
    if (n < 0) return cgltf_result_io_error;
    void* p = std::malloc(size_t(n) ? size_t(n) : 1);
    if (!p) return cgltf_result_out_of_memory;
    f.seekg(0);
    if (n && !f.read(static_cast<char*>(p), n)) { std::free(p); return cgltf_result_io_error; }
    *size = cgltf_size(n);
    *data = p;
    return cgltf_result_success;
}
void ReleaseFile(const cgltf_memory_options*, const cgltf_file_options*, void* data) { std::free(data); }

const char* ResultText(cgltf_result r)
{
    switch (r)
    {
    case cgltf_result_success: return "ok";
    case cgltf_result_data_too_short: return "data too short";
    case cgltf_result_unknown_format: return "not a glTF file";
    case cgltf_result_invalid_json: return "invalid JSON";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "file not found (a buffer or the file itself)";
    case cgltf_result_io_error: return "read error";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "glTF 1.0 (only 2.0 is read)";
    default: return "error";
    }
}

glm::mat4 LocalMatrix(const glm::vec3& t, const glm::quat& r, const glm::vec3& s)
{
    return glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s);
}

struct NodeTrs
{
    glm::vec3 t{ 0.0f }, s{ 1.0f };
    glm::quat r{ 1, 0, 0, 0 };
    bool matrix = false; // node->matrix given (not animatable)
    glm::mat4 m{ 1.0f };
    glm::mat4 Local() const { return matrix ? m : LocalMatrix(t, r, s); }
};

NodeTrs DefaultTrs(const cgltf_node& n)
{
    NodeTrs x;
    if (n.has_matrix)
    {
        x.matrix = true;
        x.m = glm::make_mat4(n.matrix);
        return x;
    }
    if (n.has_translation) x.t = glm::make_vec3(n.translation);
    if (n.has_rotation) x.r = glm::quat(n.rotation[3], n.rotation[0], n.rotation[1], n.rotation[2]);
    if (n.has_scale) x.s = glm::make_vec3(n.scale);
    return x;
}

// global matrices of every node (parents first through recursion with memo)
void Globals(const cgltf_data& d, const std::vector<NodeTrs>& trs, std::vector<glm::mat4>& out)
{
    out.assign(d.nodes_count, glm::mat4(1.0f));
    std::vector<char> done(d.nodes_count, 0);
    std::vector<size_t> chain;
    for (size_t i = 0; i < d.nodes_count; ++i)
    {
        if (done[i]) continue;
        chain.clear();
        for (const cgltf_node* n = &d.nodes[i]; n && !done[size_t(n - d.nodes)]; n = n->parent) chain.push_back(size_t(n - d.nodes));
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            const cgltf_node& n = d.nodes[*it];
            const glm::mat4 local = trs[*it].Local();
            out[*it] = n.parent ? out[size_t(n.parent - d.nodes)] * local : local;
            done[*it] = 1;
        }
    }
}

// one keyframe value of an animation sampler output (cubic spline: the value between its tangents)
void ReadKey(const cgltf_animation_sampler& s, size_t key, float* v, size_t n)
{
    const size_t i = s.interpolation == cgltf_interpolation_type_cubic_spline ? key * 3 + 1 : key;
    cgltf_accessor_read_float(s.output, i, v, n);
}

// samples a channel at time t into the node's TRS (linear / step; cubic spline as linear between the values)
void SampleChannel(const cgltf_animation_channel& c, float t, NodeTrs& x)
{
    const cgltf_animation_sampler& s = *c.sampler;
    const size_t keys = s.input->count;
    if (!keys) return;
    float t0 = 0, t1 = 0;
    size_t k = 0;
    cgltf_accessor_read_float(s.input, 0, &t0, 1);
    float u = 0.0f;
    size_t k1 = 0;
    if (t <= t0 || keys == 1) k = k1 = 0;
    else
    {
        cgltf_accessor_read_float(s.input, keys - 1, &t1, 1);
        if (t >= t1) k = k1 = keys - 1;
        else
        {
            size_t lo = 0, hi = keys - 1; // last key <= t
            while (hi - lo > 1)
            {
                const size_t mid = (lo + hi) / 2;
                float tm = 0;
                cgltf_accessor_read_float(s.input, mid, &tm, 1);
                (tm <= t ? lo : hi) = mid;
            }
            float ta = 0, tb = 0;
            cgltf_accessor_read_float(s.input, lo, &ta, 1);
            cgltf_accessor_read_float(s.input, hi, &tb, 1);
            k = lo;
            k1 = hi;
            u = tb > ta ? (t - ta) / (tb - ta) : 0.0f;
            if (s.interpolation == cgltf_interpolation_type_step) { k1 = k; u = 0.0f; }
        }
    }
    float a[4]{}, b[4]{};
    switch (c.target_path)
    {
    case cgltf_animation_path_type_translation:
    case cgltf_animation_path_type_scale:
    {
        ReadKey(s, k, a, 3);
        ReadKey(s, k1, b, 3);
        const glm::vec3 v = glm::mix(glm::make_vec3(a), glm::make_vec3(b), u);
        (c.target_path == cgltf_animation_path_type_translation ? x.t : x.s) = v;
        break;
    }
    case cgltf_animation_path_type_rotation:
    {
        ReadKey(s, k, a, 4);
        ReadKey(s, k1, b, 4);
        const glm::quat qa(a[3], a[0], a[1], a[2]), qb(b[3], b[0], b[1], b[2]);
        x.r = glm::normalize(glm::slerp(glm::normalize(qa), glm::normalize(qb), u));
        break;
    }
    default: break; // morph weights: not read
    }
}

// weight 0..1 -> MLOD selection byte (1 = 1.0, 2..255 = (256 - b) / 255)
uint8_t WeightByte(float w)
{
    if (w <= 0.5f / 255.0f) return 0;
    if (w >= 1.0f - 0.5f / 255.0f) return 1;
    return uint8_t(std::clamp(int(std::lround(256.0f - w * 255.0f)), 2, 255));
}

std::string SafeName(std::string s)
{
    for (char& c : s)
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || uint8_t(c) < 32) c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? std::string("unnamed") : s;
}

std::string MimeExtension(const char* mime, const std::string& fallback)
{
    const std::string m = mime ? ToLower(mime) : "";
    if (m == "image/png") return ".png";
    if (m == "image/jpeg") return ".jpg";
    return fallback.empty() ? ".png" : fallback;
}

// Writes the image of a texture into dir as <stem><ext>; returns the file ("" if the image cannot be had).
fs::path ExtractImage(const cgltf_image* img, const cgltf_options& opt, const fs::path& gltfDir, const fs::path& dir, const std::string& stem)
{
    if (!img) return {};
    std::vector<uint8_t> bytes;
    std::string ext;
    if (img->buffer_view && img->buffer_view->buffer && img->buffer_view->buffer->data)
    {
        const uint8_t* p = static_cast<const uint8_t*>(img->buffer_view->buffer->data) + img->buffer_view->offset;
        bytes.assign(p, p + img->buffer_view->size);
        ext = MimeExtension(img->mime_type, "");
    }
    else if (img->uri && std::strncmp(img->uri, "data:", 5) == 0)
    {
        const char* comma = std::strchr(img->uri, ',');
        if (!comma || !std::strstr(img->uri, ";base64")) return {};
        const std::string mime(img->uri + 5, size_t(std::strchr(img->uri, ';') - img->uri - 5));
        const char* b64 = comma + 1;
        const size_t len = std::strlen(b64);
        size_t size = len / 4 * 3;
        if (len >= 1 && b64[len - 1] == '=') --size;
        if (len >= 2 && b64[len - 2] == '=') --size;
        void* out = nullptr;
        if (cgltf_load_buffer_base64(&opt, size, b64, &out) != cgltf_result_success) return {};
        bytes.assign(static_cast<uint8_t*>(out), static_cast<uint8_t*>(out) + size);
        std::free(out);
        ext = MimeExtension(mime.c_str(), "");
    }
    else if (img->uri)
    {
        std::string uri = img->uri;
        cgltf_decode_uri(uri.data());
        uri.resize(std::strlen(uri.c_str()));
        const fs::path src = gltfDir / fs::path(Utf8ToWide(uri));
        std::ifstream f(src, std::ios::binary);
        if (!f) return {};
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        ext = ToLower(PathToUtf8(src.extension()));
        if (ext.empty()) ext = MimeExtension(img->mime_type, "");
    }
    if (bytes.empty()) return {};
    const fs::path file = dir / fs::path(Utf8ToWide(stem + ext));
    std::ofstream o(file, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return o ? file : fs::path();
}

std::string Hex(uint64_t v) { return std::format("{:016x}", v); }

} // namespace

bool IsGltfFile(const fs::path& path)
{
    const std::string e = ToLower(PathToUtf8(path.extension()));
    return e == ".gltf" || e == ".glb";
}

bool LoadGltf(const fs::path& path, P3DModel& out, std::string& error)
{
    auto& log = Log::Get();
    out = {};
    out.path = path;
    out.format = "glTF";
    const std::string pathStr = PathToUtf8(path);

    cgltf_options opt{};
    opt.file.read = ReadFile;
    opt.file.release = ReleaseFile;
    cgltf_data* data = nullptr;
    cgltf_result r = cgltf_parse_file(&opt, pathStr.c_str(), &data);
    if (r == cgltf_result_success) r = cgltf_load_buffers(&opt, data, pathStr.c_str());
    if (r == cgltf_result_success) r = cgltf_validate(data);
    if (r != cgltf_result_success)
    {
        error = std::string("glTF: ") + ResultText(r);
        if (data) cgltf_free(data);
        return false;
    }
    struct Free { cgltf_data* d; ~Free() { cgltf_free(d); } } freeData{ data };
    const cgltf_data& d = *data;

    // ---- joints: every node used by a skin; parent = the nearest ancestor that is a joint too
    std::map<const cgltf_node*, int> boneOf;
    std::vector<const cgltf_node*> joints;
    std::vector<glm::mat4> ibm; // per bone (first skin that lists it)
    for (size_t si = 0; si < d.skins_count; ++si)
    {
        const cgltf_skin& sk = d.skins[si];
        for (size_t j = 0; j < sk.joints_count; ++j)
        {
            const cgltf_node* n = sk.joints[j];
            if (boneOf.count(n)) continue;
            boneOf[n] = int(joints.size());
            joints.push_back(n);
            glm::mat4 m(1.0f);
            if (sk.inverse_bind_matrices) cgltf_accessor_read_float(sk.inverse_bind_matrices, j, glm::value_ptr(m), 16);
            ibm.push_back(m);
        }
    }
    std::set<std::string> used;
    for (size_t b = 0; b < joints.size(); ++b)
    {
        std::string name = joints[b]->name && *joints[b]->name ? joints[b]->name : std::format("joint_{}", b);
        for (int k = 2; used.count(ToLower(name)); ++k) name = std::format("{}_{}", joints[b]->name ? joints[b]->name : "joint", k);
        used.insert(ToLower(name));
        out.importedBones.push_back({ name, "" });
    }
    for (size_t b = 0; b < joints.size(); ++b)
        for (const cgltf_node* p = joints[b]->parent; p; p = p->parent)
            if (auto it = boneOf.find(p); it != boneOf.end()) { out.importedBones[b].second = out.importedBones[size_t(it->second)].first; break; }
    out.importedSkeleton = d.skins_count && d.skins[0].name && *d.skins[0].name ? d.skins[0].name : PathToUtf8(path.stem());

    // ---- default pose: node globals
    std::vector<NodeTrs> rest(d.nodes_count);
    for (size_t i = 0; i < d.nodes_count; ++i) rest[i] = DefaultTrs(d.nodes[i]);
    std::vector<glm::mat4> global;
    Globals(d, rest, global);

    // ---- materials -> textures in the temp folder (+ an RVMAT for a normal texture)
    std::error_code ec;
    const fs::path texDir = fs::temp_directory_path(ec) / L"ArmaView" / L"gltf" /
                            fs::path(Utf8ToWide(SafeName(PathToUtf8(path.stem())) + "_" + Hex(std::hash<std::string>{}(ToLower(pathStr))).substr(0, 8)));
    struct Mat { std::string texture, material; };
    std::map<const cgltf_material*, Mat> mats;
    auto material = [&](const cgltf_material* m) -> const Mat& {
        if (auto it = mats.find(m); it != mats.end()) return it->second;
        Mat out;
        float c[4] = { 0.8f, 0.8f, 0.8f, 1.0f };
        const std::string stem = SafeName(m && m->name && *m->name ? m->name : std::format("material_{}", m ? size_t(m - d.materials) : size_t(0)));
        if (m && m->has_pbr_metallic_roughness)
        {
            std::memcpy(c, m->pbr_metallic_roughness.base_color_factor, sizeof(c));
            if (const cgltf_texture* t = m->pbr_metallic_roughness.base_color_texture.texture)
            {
                fs::create_directories(texDir, ec);
                const fs::path f = ExtractImage(t->image, opt, path.parent_path(), texDir, stem + "_co");
                if (!f.empty()) out.texture = PathToUtf8(f);
                else log.Warn("glTF '{}': base color texture of '{}' not readable", PathToUtf8(path.filename()), stem);
            }
        }
        if (out.texture.empty())
            out.texture = std::format("#(argb,8,8,3)color({},{},{},{},CO)", ShortNum(std::clamp(c[0], 0.0f, 1.0f)), ShortNum(std::clamp(c[1], 0.0f, 1.0f)),
                                      ShortNum(std::clamp(c[2], 0.0f, 1.0f)), ShortNum(std::clamp(c[3], 0.0f, 1.0f)));
        if (m && m->normal_texture.texture)
        {
            fs::create_directories(texDir, ec);
            const fs::path f = ExtractImage(m->normal_texture.texture->image, opt, path.parent_path(), texDir, stem + "_nohq");
            if (!f.empty())
            {
                const fs::path rv = texDir / fs::path(Utf8ToWide(stem + ".rvmat"));
                std::ofstream o(rv, std::ios::trunc);
                o << "// generated by ArmaView from a glTF material (normal texture)\n"
                     "ambient[]={1,1,1,1};\ndiffuse[]={1,1,1,1};\nforcedDiffuse[]={0,0,0,0};\nemmisive[]={0,0,0,1};\n"
                     "specular[]={0.1,0.1,0.1,1};\nspecularPower=40;\nPixelShaderID=\"NormalMap\";\nVertexShaderID=\"NormalMap\";\n"
                     "class Stage1\n{\n\ttexture=\"" << PathToUtf8(f) << "\";\n\tuvSource=\"tex\";\n};\n";
                if (o) out.material = PathToUtf8(rv);
            }
            else log.Warn("glTF '{}': normal texture of '{}' not readable", PathToUtf8(path.filename()), stem);
        }
        return mats.emplace(m, std::move(out)).first->second;
    };

    // ---- geometry: LOD 0 (1.000)
    P3DLod lod;
    lod.resolution = 1.0f;
    lod.versionMajor = 28;
    lod.versionMinor = 256;
    std::vector<std::vector<float>> weights(joints.size()); // per bone, per point
    int skipped = 0, noNormals = 0;
    for (size_t ni = 0; ni < d.nodes_count; ++ni)
    {
        const cgltf_node& node = d.nodes[ni];
        if (!node.mesh) continue;
        const glm::mat4 xf = node.skin ? glm::mat4(1.0f) : global[ni]; // skinned: vertices are in bind space
        const glm::mat3 nxf = glm::transpose(glm::inverse(glm::mat3(xf)));
        for (size_t pi = 0; pi < node.mesh->primitives_count; ++pi)
        {
            const cgltf_primitive& prim = node.mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) { ++skipped; continue; }
            const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *jnt = nullptr, *wgt = nullptr;
            for (size_t a = 0; a < prim.attributes_count; ++a)
            {
                const cgltf_attribute& at = prim.attributes[a];
                if (at.type == cgltf_attribute_type_position) pos = at.data;
                else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
                else if (at.type == cgltf_attribute_type_joints && at.index == 0) jnt = at.data;
                else if (at.type == cgltf_attribute_type_weights && at.index == 0) wgt = at.data;
            }
            if (!pos || !pos->count) { ++skipped; continue; }
            const uint32_t base = uint32_t(lod.points.size());
            const size_t nv = pos->count;
            for (size_t v = 0; v < nv; ++v)
            {
                glm::vec3 p(0.0f), n(0, 1, 0);
                cgltf_accessor_read_float(pos, v, glm::value_ptr(p), 3);
                if (nrm) cgltf_accessor_read_float(nrm, v, glm::value_ptr(n), 3);
                p = glm::vec3(xf * glm::vec4(p, 1.0f));
                n = glm::normalize(nxf * n);
                lod.points.push_back({ Mirror(p), 0u });
                lod.normals.push_back(Mirror(n));
            }
            if (!nrm) ++noNormals;
            for (auto& w : weights) w.resize(lod.points.size(), 0.0f);
            if (node.skin && jnt && wgt)
                for (size_t v = 0; v < nv; ++v)
                {
                    cgltf_uint j[4]{};
                    float w[4]{};
                    cgltf_accessor_read_uint(jnt, v, j, 4);
                    cgltf_accessor_read_float(wgt, v, w, 4);
                    const float sum = w[0] + w[1] + w[2] + w[3];
                    for (int k = 0; k < 4; ++k)
                        if (w[k] > 0.0f && j[k] < node.skin->joints_count)
                            weights[size_t(boneOf[node.skin->joints[j[k]]])][base + v] += sum > 0.0f ? w[k] / sum : 0.0f;
                }
            const Mat& m = material(prim.material);
            const size_t ni2 = prim.indices ? prim.indices->count : nv;
            for (size_t t = 0; t + 2 < ni2; t += 3)
            {
                uint32_t idx[3];
                for (int k = 0; k < 3; ++k) idx[k] = uint32_t(prim.indices ? cgltf_accessor_read_index(prim.indices, t + size_t(k)) : t + size_t(k));
                if (idx[0] >= nv || idx[1] >= nv || idx[2] >= nv) continue;
                P3DFace f;
                f.count = 3;
                f.texture = m.texture;
                f.material = m.material;
                static constexpr int kOrder[3] = { 0, 2, 1 }; // mirrored axes: reversed winding
                for (int k = 0; k < 3; ++k)
                {
                    const uint32_t i = idx[kOrder[k]];
                    float tc[2]{};
                    if (uv) cgltf_accessor_read_float(uv, i, tc, 2);
                    f.v[k] = { base + i, base + i, tc[0], tc[1] };
                }
                lod.faces.push_back(std::move(f));
            }
        }
    }
    if (lod.faces.empty())
    {
        error = "glTF: no triangle meshes";
        return false;
    }
    for (size_t b = 0; b < joints.size(); ++b)
    {
        P3DSelection s;
        s.name = out.importedBones[b].first;
        s.pointBytes.resize(lod.points.size(), 0);
        s.faceBytes.resize(lod.faces.size(), 0);
        for (size_t p = 0; p < lod.points.size(); ++p)
            if (const uint8_t byte = WeightByte(weights[b][p]))
            {
                s.pointBytes[p] = byte;
                ++s.pointCount;
                s.partialCount += byte > 1;
            }
        for (size_t f = 0; f < lod.faces.size(); ++f)
        {
            const P3DFace& face = lod.faces[f];
            if (s.pointBytes[face.v[0].point] && s.pointBytes[face.v[1].point] && s.pointBytes[face.v[2].point])
            {
                s.faceBytes[f] = 1;
                ++s.faceCount;
            }
        }
        lod.selections.push_back(std::move(s));
    }
    out.lods.push_back(std::move(lod));

    // ---- Memory LOD: the joints in the bind pose (inverse of the inverse bind matrix), the bone pivots
    if (!joints.empty())
    {
        P3DLod mem;
        mem.resolution = 1.0e15f;
        mem.versionMajor = 28;
        mem.versionMinor = 256;
        for (size_t b = 0; b < joints.size(); ++b)
        {
            const glm::vec3 p = glm::vec3(glm::inverse(ibm[b])[3]);
            mem.points.push_back({ Mirror(p), 0u });
        }
        for (size_t b = 0; b < joints.size(); ++b)
        {
            P3DSelection s;
            s.name = out.importedBones[b].first;
            s.pointBytes.assign(joints.size(), 0);
            s.pointBytes[b] = 1;
            s.pointCount = 1;
            mem.selections.push_back(std::move(s));
        }
        out.lods.push_back(std::move(mem));
    }

    // ---- clips: the joints' deformation G(t) * IBM per frame, in the viewer's axes
    int otherChannels = 0;
    for (size_t ai = 0; ai < d.animations_count && !joints.empty(); ++ai)
    {
        const cgltf_animation& an = d.animations[ai];
        float tEnd = 0.0f;
        for (size_t s = 0; s < an.samplers_count; ++s)
            if (an.samplers[s].input && an.samplers[s].input->count)
            {
                float t = 0;
                cgltf_accessor_read_float(an.samplers[s].input, an.samplers[s].input->count - 1, &t, 1);
                tEnd = std::max(tEnd, t);
            }
        ImportedClip clip;
        clip.name = an.name && *an.name ? an.name : std::format("clip_{}", ai);
        clip.duration = tEnd;
        const int frames = std::clamp(int(std::ceil(tEnd * kFps - 1e-4f)) + 1, 1, kMaxFrames);
        std::vector<NodeTrs> trs;
        std::vector<glm::mat4> g;
        for (int fr = 0; fr < frames; ++fr)
        {
            const float t = frames > 1 ? tEnd * float(fr) / float(frames - 1) : 0.0f;
            trs = rest;
            for (size_t c = 0; c < an.channels_count; ++c)
            {
                const cgltf_animation_channel& ch = an.channels[c];
                if (!ch.target_node || !ch.sampler) continue;
                if (fr == 0 && !boneOf.count(ch.target_node)) ++otherChannels;
                NodeTrs& x = trs[size_t(ch.target_node - d.nodes)];
                if (x.matrix) // a node given by a matrix gets animated: its TRS from the matrix
                {
                    glm::vec3 skew;
                    glm::vec4 persp;
                    glm::decompose(x.m, x.s, x.r, x.t, skew, persp);
                    x.matrix = false;
                }
                SampleChannel(ch, t, x);
            }
            Globals(d, trs, g);
            clip.times.push_back(frames > 1 ? float(fr) / float(frames - 1) : 0.0f);
            for (size_t b = 0; b < joints.size(); ++b) clip.poses.push_back(kMirror * (g[size_t(joints[b] - d.nodes)] * ibm[b]) * kMirror);
        }
        out.importedClips.push_back(std::move(clip));
    }

    out.complete = true;
    log.Info("glTF '{}': {} point(s), {} triangle(s), {} material(s), {} joint(s), {} clip(s){}{}{}", PathToUtf8(path.filename()), out.lods[0].points.size(),
             out.lods[0].faces.size(), mats.size(), joints.size(), out.importedClips.size(),
             skipped ? std::format("; {} non-triangle primitive(s) skipped", skipped) : "",
             noNormals ? std::format("; {} primitive(s) without normals", noNormals) : "",
             otherChannels ? std::format("; {} channel(s) on non-joint nodes ignored", otherChannels) : "");
    if (!mats.empty() && fs::exists(texDir, ec)) log.Debug("glTF textures: {}", PathToUtf8(texDir));
    return true;
}

std::shared_ptr<RtmAnimation> ImportedClipAnimation(const P3DModel& model, const ImportedClip& clip)
{
    auto a = std::make_shared<RtmAnimation>();
    a->path = model.path;
    a->path += L"#" + Utf8ToWide(clip.name);
    a->info.signature = "glTF";
    a->info.frames = uint32_t(clip.times.size());
    for (const auto& [b, p] : model.importedBones) a->info.bones.push_back(b);
    a->times = clip.times;
    a->poses.reserve(clip.poses.size());
    for (const glm::mat4& m : clip.poses) a->poses.push_back(BonePose::FromMatrix(m));
    return a;
}

// ============================================================================================ sample (selftest)

namespace {

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0)
{
    crc = ~crc;
    for (size_t i = 0; i < n; ++i)
    {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// RGBA8 -> PNG with stored (uncompressed) deflate blocks
std::vector<uint8_t> EncodePng(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba)
{
    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < h; ++y)
    {
        raw.push_back(0); // filter none
        raw.insert(raw.end(), rgba.begin() + std::ptrdiff_t(y * w * 4), rgba.begin() + std::ptrdiff_t((y + 1) * w * 4));
    }
    std::vector<uint8_t> z = { 0x78, 0x01 };
    for (size_t o = 0; o < raw.size() || o == 0;)
    {
        const size_t n = std::min<size_t>(65535, raw.size() - o);
        z.push_back(o + n >= raw.size() ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(o), raw.begin() + std::ptrdiff_t(o + n));
        o += n;
        if (n == 0) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    const uint32_t adler = (b << 16) | a;
    for (int k = 3; k >= 0; --k) z.push_back(uint8_t(adler >> (k * 8)));

    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& body) {
        for (int k = 3; k >= 0; --k) png.push_back(uint8_t(body.size() >> (k * 8)));
        std::vector<uint8_t> tb(type, type + 4);
        tb.insert(tb.end(), body.begin(), body.end());
        png.insert(png.end(), tb.begin(), tb.end());
        const uint32_t crc = Crc32(tb.data(), tb.size());
        for (int k = 3; k >= 0; --k) png.push_back(uint8_t(crc >> (k * 8)));
    };
    std::vector<uint8_t> ihdr;
    for (uint32_t v : { w, h })
        for (int k = 3; k >= 0; --k) ihdr.push_back(uint8_t(v >> (k * 8)));
    ihdr.insert(ihdr.end(), { 8, 6, 0, 0, 0 }); // 8 bit RGBA
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return png;
}

std::string Base64(const uint8_t* p, size_t n)
{
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (size_t i = 0; i < n; i += 3)
    {
        const uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? uint32_t(p[i + 2]) : 0);
        s += k[(v >> 18) & 63];
        s += k[(v >> 12) & 63];
        s += i + 1 < n ? k[(v >> 6) & 63] : '=';
        s += i + 2 < n ? k[v & 63] : '=';
    }
    return s;
}

template <class T> void Put(std::vector<uint8_t>& b, const T& v)
{
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}

} // namespace

bool WriteGltfSample(const fs::path& file)
{
    // a 0.2 m square bar from y = 0 to 2: three rings of 4 points (12), sides + caps
    std::vector<uint8_t> buf;
    const float hw = 0.1f;
    const float ring[4][2] = { { -hw, -hw }, { hw, -hw }, { hw, hw }, { -hw, hw } };
    for (int r = 0; r < 3; ++r)
        for (const auto& c : ring) { Put(buf, c[0]); Put(buf, float(r)); Put(buf, c[1]); }
    const size_t posBytes = buf.size();
    for (int r = 0; r < 3; ++r)
        for (const auto& c : ring) { const glm::vec3 n = glm::normalize(glm::vec3(c[0], 0.0f, c[1])); Put(buf, n.x); Put(buf, n.y); Put(buf, n.z); }
    const size_t nrmOff = posBytes, uvOff = buf.size();
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) { Put(buf, float(k) / 3.0f); Put(buf, float(r) / 2.0f); }
    const size_t jntOff = buf.size();
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) { const uint8_t j[4] = { 0, 1, 0, 0 }; buf.insert(buf.end(), j, j + 4); }
    const size_t wgtOff = buf.size();
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k)
        {
            const float w0 = r == 0 ? 1.0f : r == 1 ? 0.5f : 0.0f;
            Put(buf, w0); Put(buf, 1.0f - w0); Put(buf, 0.0f); Put(buf, 0.0f);
        }
    const size_t idxOff = buf.size();
    std::vector<uint16_t> idx;
    for (int r = 0; r < 2; ++r)
        for (int k = 0; k < 4; ++k)
        {
            const uint16_t a = uint16_t(r * 4 + k), b = uint16_t(r * 4 + (k + 1) % 4), c = uint16_t(a + 4), e = uint16_t(b + 4);
            idx.insert(idx.end(), { a, e, b, a, c, e }); // counter-clockwise seen from outside (glTF front faces)
        }
    idx.insert(idx.end(), { 0, 1, 2, 0, 2, 3, 8, 10, 9, 8, 11, 10 }); // bottom (seen from below), top
    for (uint16_t i : idx) Put(buf, i);
    while (buf.size() % 4) buf.push_back(0);
    const size_t ibmOff = buf.size();
    for (float y : { 0.0f, 1.0f }) // inverse bind: translate by -joint position
    {
        const glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(0, -y, 0));
        for (int k = 0; k < 16; ++k) Put(buf, glm::value_ptr(m)[k]);
    }
    const size_t timeOff = buf.size();
    Put(buf, 0.0f); Put(buf, 1.0f);
    const size_t rotOff = buf.size();
    const glm::quat q0(1, 0, 0, 0), q1 = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
    for (const glm::quat& q : { q0, q1 }) { Put(buf, q.x); Put(buf, q.y); Put(buf, q.z); Put(buf, q.w); }
    const size_t end = buf.size();

    const std::vector<uint8_t> colorPng = EncodePng(2, 2, { 200, 60, 40, 255, 230, 200, 60, 255, 230, 200, 60, 255, 200, 60, 40, 255 });
    const std::vector<uint8_t> normalPng = EncodePng(1, 1, { 128, 128, 255, 255 });

    const std::string json = std::format(R"({{
  "asset": {{ "version": "2.0", "generator": "ArmaView selftest" }},
  "scene": 0,
  "scenes": [ {{ "nodes": [ 0, 2 ] }} ],
  "nodes": [
    {{ "name": "bone_base", "children": [ 1 ] }},
    {{ "name": "bone_tip", "translation": [ 0, 1, 0 ] }},
    {{ "name": "bar", "mesh": 0, "skin": 0 }}
  ],
  "skins": [ {{ "name": "sample_rig", "joints": [ 0, 1 ], "inverseBindMatrices": 6 }} ],
  "meshes": [ {{ "name": "bar", "primitives": [ {{
    "attributes": {{ "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "JOINTS_0": 3, "WEIGHTS_0": 4 }},
    "indices": 5, "material": 0 }} ] }} ],
  "materials": [ {{ "name": "sample_paint", "pbrMetallicRoughness": {{ "baseColorTexture": {{ "index": 0 }} }}, "normalTexture": {{ "index": 1 }} }} ],
  "textures": [ {{ "source": 0 }}, {{ "source": 1 }} ],
  "images": [ {{ "uri": "data:image/png;base64,{}" }}, {{ "uri": "data:image/png;base64,{}" }} ],
  "animations": [ {{ "name": "bend",
    "samplers": [ {{ "input": 7, "output": 8, "interpolation": "LINEAR" }} ],
    "channels": [ {{ "sampler": 0, "target": {{ "node": 1, "path": "rotation" }} }} ] }} ],
  "buffers": [ {{ "byteLength": {}, "uri": "data:application/octet-stream;base64,{}" }} ],
  "bufferViews": [
    {{ "buffer": 0, "byteOffset": 0, "byteLength": {} }},
    {{ "buffer": 0, "byteOffset": {}, "byteLength": {} }},
    {{ "buffer": 0, "byteOffset": {}, "byteLength": {} }},
    {{ "buffer": 0, "byteOffset": {}, "byteLength": {} }},
    {{ "buffer": 0, "byteOffset": {}, "byteLength": {} }},
    {{ "buffer": 0, "byteOffset": {}, "byteLength": {} }},
    {{ "buffer": 0, "byteOffset": {}, "byteLength": {} }},
    {{ "buffer": 0, "byteOffset": {}, "byteLength": {} }},
    {{ "buffer": 0, "byteOffset": {}, "byteLength": {} }}
  ],
  "accessors": [
    {{ "bufferView": 0, "componentType": 5126, "count": 12, "type": "VEC3", "min": [ -0.1, 0, -0.1 ], "max": [ 0.1, 2, 0.1 ] }},
    {{ "bufferView": 1, "componentType": 5126, "count": 12, "type": "VEC3" }},
    {{ "bufferView": 2, "componentType": 5126, "count": 12, "type": "VEC2" }},
    {{ "bufferView": 3, "componentType": 5121, "count": 12, "type": "VEC4" }},
    {{ "bufferView": 4, "componentType": 5126, "count": 12, "type": "VEC4" }},
    {{ "bufferView": 5, "componentType": 5123, "count": {}, "type": "SCALAR" }},
    {{ "bufferView": 6, "componentType": 5126, "count": 2, "type": "MAT4" }},
    {{ "bufferView": 7, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [ 0 ], "max": [ 1 ] }},
    {{ "bufferView": 8, "componentType": 5126, "count": 2, "type": "VEC4" }}
  ]
}}
)",
        Base64(colorPng.data(), colorPng.size()), Base64(normalPng.data(), normalPng.size()), end, Base64(buf.data(), end),
        posBytes, nrmOff, uvOff - nrmOff, uvOff, jntOff - uvOff, jntOff, wgtOff - jntOff, wgtOff, idxOff - wgtOff, idxOff, idx.size() * 2,
        ibmOff, timeOff - ibmOff, timeOff, rotOff - timeOff, rotOff, end - rotOff, idx.size());
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream o(file, std::ios::binary | std::ios::trunc);
    o << json;
    return bool(o);
}
