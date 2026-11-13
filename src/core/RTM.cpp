#include "RTM.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>
#include <glm/gtc/matrix_transform.hpp>

#include "CoreGuard.h"

namespace fs = std::filesystem;

// ============================================================================================ header

bool ReadRtmHeader(const fs::path& path, RtmInfo& out, std::string& error)
{
    out = {};
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { error = "cannot open file"; return false; }
    const uint64_t size = static_cast<uint64_t>(f.tellg());
    f.seekg(0);

    char sig[8]{};
    if (!f.read(sig, 8)) { error = "file too short"; return false; }
    out.signature.assign(sig, strnlen(sig, 8));
    if (std::memcmp(sig, "RTM_MDAT", 8) == 0)
    {
        uint32_t zero = 0, count = 0;
        f.read(reinterpret_cast<char*>(&zero), 4);
        f.read(reinterpret_cast<char*>(&count), 4);
        out.mdatEvents = count;
        uint64_t at = 16;
        if (count) // events not decoded yet: find the animation block
        {
            std::string head(size_t(std::min<uint64_t>(size, 1 << 20)), '\0');
            f.seekg(0);
            f.read(head.data(), std::streamsize(head.size()));
            size_t pos = head.find("RTM_0101", 16);
            if (pos == std::string::npos) { error = "RTM_MDAT block without RTM_0101 data"; return false; }
            at = pos;
        }
        f.clear();
        f.seekg(std::streamoff(at));
        if (!f.read(sig, 8)) { error = "file too short after RTM_MDAT"; return false; }
        out.dataOffset = at;
        out.signature = "RTM_MDAT+" + std::string(sig, strnlen(sig, 8));
    }
    const bool v100 = std::memcmp(sig, "RTM_0100", 8) == 0; // OFP: only the Z step (CWR RTMReader.hpp:57)
    if (!v100 && std::memcmp(sig, "RTM_0101", 8) != 0)
    {
        if (std::memcmp(sig, "RTM_", 4) != 0) // not a source RTM at all: a binarized (game) file
        {
            out.binarized = true;
            error = kBinarizedNotSupported;
        }
        else
            error = std::format("unknown signature '{}'", out.signature);
        return false;
    }
    uint32_t bones = 0;
    if (v100) f.read(reinterpret_cast<char*>(&out.move.z), 4);
    else f.read(reinterpret_cast<char*>(&out.move), 12);
    out.headerSize = v100 ? 20 : 28;
    f.read(reinterpret_cast<char*>(&out.frames), 4);
    f.read(reinterpret_cast<char*>(&bones), 4);
    if (!f || bones > 4096) { error = "corrupt header"; return false; }

    const uint64_t expected = out.dataOffset + out.headerSize + 32ull * bones + uint64_t(out.frames) * (4ull + bones * 80ull);
    if (expected != size)
    {
        error = std::format("size {} does not match header ({} frames x {} bones -> {} bytes)", size, out.frames, bones, expected);
        return false;
    }
    out.bones.reserve(bones);
    for (uint32_t i = 0; i < bones; ++i)
    {
        char name[32];
        if (!f.read(name, 32)) { error = "truncated bone names"; return false; }
        out.bones.push_back(AnsiToUtf8(std::string_view(name, strnlen(name, 32))));
    }
    return true;
}

// ============================================================================================ poses

glm::mat4 BonePose::Matrix() const
{
    glm::mat4 m = glm::mat4_cast(rot);
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    m[3] = glm::vec4(pos, 1.0f);
    return m;
}

BonePose BonePose::FromMatrix(const glm::mat4& m)
{
    BonePose p;
    p.pos = glm::vec3(m[3]);
    glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    p.scale = { glm::length(c0), glm::length(c1), glm::length(c2) };
    if (p.scale.x > 1e-8f) c0 /= p.scale.x;
    if (p.scale.y > 1e-8f) c1 /= p.scale.y;
    if (p.scale.z > 1e-8f) c2 /= p.scale.z;
    p.rot = glm::normalize(glm::quat_cast(glm::mat3(c0, c1, c2)));
    return p;
}

BonePose BonePose::Mix(const BonePose& a, const BonePose& b, float t)
{
    BonePose r;
    r.rot = glm::slerp(a.rot, b.rot, t); // takes the short path
    r.pos = glm::mix(a.pos, b.pos, t);
    r.scale = glm::mix(a.scale, b.scale, t);
    return r;
}

// ============================================================================================ animation

std::string RtmAnimation::Name() const { return PathToUtf8(path.filename()); }

BonePose RtmAnimation::Sample(size_t bone, float phase) const
{
    const size_t n = times.size(), nb = Bones();
    if (n == 0 || bone >= nb) return {};
    if (n == 1 || phase <= times.front()) return poses[bone];
    if (phase >= times.back()) return poses[(n - 1) * nb + bone];
    size_t hi = std::upper_bound(times.begin(), times.end(), phase) - times.begin();
    size_t lo = hi - 1;
    float span = times[hi] - times[lo];
    float t = span > 1e-9f ? (phase - times[lo]) / span : 0.0f;
    return BonePose::Mix(poses[lo * nb + bone], poses[hi * nb + bone], t);
}

bool LoadRtm(const fs::path& path, RtmAnimation& out, std::string& error, std::vector<std::string>& warnings)
{
    out = {};
    out.path = path;
    if (!ReadRtmHeader(path, out.info, error)) return false;

    std::ifstream f(path, std::ios::binary);
    const size_t nb = out.info.bones.size(), nf = out.info.frames;
    f.seekg(std::streamoff(out.info.dataOffset + out.info.headerSize + 32 * nb));
    out.times.resize(nf);
    out.poses.resize(nf * nb);
    int renamed = 0;
    float maxScaleErr = 0.0f;
    std::string scaleBone;
    for (size_t fr = 0; fr < nf; ++fr)
    {
        f.read(reinterpret_cast<char*>(&out.times[fr]), 4);
        for (size_t b = 0; b < nb; ++b)
        {
            char name[32];
            float m[12];
            f.read(name, 32);
            f.read(reinterpret_cast<char*>(m), 48);
            if (!f) { error = std::format("truncated at frame {}", fr); return false; }
            // Frames normally repeat the header order; map by name if not.
            size_t slot = b;
            std::string nm = AnsiToUtf8(std::string_view(name, strnlen(name, 32)));
            if (!IEquals(nm, out.info.bones[b]))
            {
                ++renamed;
                slot = nb;
                for (size_t k = 0; k < nb; ++k)
                    if (IEquals(nm, out.info.bones[k])) { slot = k; break; }
                if (slot == nb) continue;
            }
            glm::mat4 mat(glm::vec4(m[0], m[1], m[2], 0), glm::vec4(m[3], m[4], m[5], 0), glm::vec4(m[6], m[7], m[8], 0),
                          glm::vec4(m[9], m[10], m[11], 1));
            BonePose p = BonePose::FromMatrix(mat);
            float se = std::max({ std::abs(p.scale.x - 1), std::abs(p.scale.y - 1), std::abs(p.scale.z - 1) });
            if (se > maxScaleErr) { maxScaleErr = se; scaleBone = out.info.bones[slot]; }
            out.poses[fr * nb + slot] = p;
        }
    }
    if (renamed) warnings.push_back(std::format("{} frame entries out of header bone order (mapped by name)", renamed));
    for (size_t i = 1; i < nf; ++i)
        if (out.times[i] < out.times[i - 1])
        {
            warnings.push_back(std::format("frame times not increasing at frame {} ({:.4f} < {:.4f})", i, out.times[i], out.times[i - 1]));
            break;
        }
    if (nf && (out.times.front() > 0.001f || out.times.back() < 0.999f))
        warnings.push_back(std::format("frame times span {:.3f}..{:.3f} (expected 0..1)", out.times.front(), out.times.back()));
    if (maxScaleErr > 0.01f) warnings.push_back(std::format("bone matrices contain scale (max deviation {:.3f} on '{}')", maxScaleErr, scaleBone));
    return true;
}

std::vector<SeamJump> CheckLoopSeam(const RtmAnimation& a, float maxDistance, float maxAngleDeg)
{
    std::vector<SeamJump> out;
    const size_t nf = a.Frames(), nb = a.Bones();
    if (nf < 2) return out;
    for (size_t b = 0; b < nb; ++b)
    {
        const BonePose& p0 = a.poses[b];
        const BonePose& p1 = a.poses[(nf - 1) * nb + b];
        float d = glm::length(p1.pos - p0.pos);
        float ang = glm::degrees(2.0f * std::acos(std::clamp(std::abs(glm::dot(p0.rot, p1.rot)), 0.0f, 1.0f)));
        if (d > maxDistance || ang > maxAngleDeg) out.push_back({ a.info.bones[b], d, ang });
    }
    std::sort(out.begin(), out.end(), [](const SeamJump& x, const SeamJump& y) { return x.distance + x.angle * 0.01f > y.distance + y.angle * 0.01f; });
    return out;
}

float DurationFromMovesSpeed(float speed)
{
    if (speed >= 1e6f) return 0.0f; // e.g. class Crew: speed=1e+010 - a static pose
    if (speed > 1e-6f) return 1.0f / speed;
    if (speed < -1e-6f) return -speed;
    return 1.0f;
}

std::vector<RtmBoneCheck> CheckRtmFolder(const fs::path& dir, const Skeleton& skel)
{
    std::vector<RtmBoneCheck> out;
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        std::wstring ext = e.path().extension().native();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (e.is_regular_file() && ext == L".rtm") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files)
    {
        RtmBoneCheck c;
        c.file = f;
        if (!ReadRtmHeader(f, c.info, c.error))
        {
            out.push_back(std::move(c));
            continue;
        }
        std::set<std::string> rtm;
        for (const auto& b : c.info.bones) rtm.insert(ToLower(b));
        for (const auto& b : skel.bones)
            if (!rtm.count(ToLower(b.name))) c.missing.push_back(b.name);
        for (const auto& b : c.info.bones)
            if (skel.Find(b) < 0) c.extra.push_back(b);
        out.push_back(std::move(c));
    }
    return out;
}

// ============================================================================================ stack

void AnimStack::Bind(const Skeleton& skel)
{
    for (auto& l : layers)
    {
        l.boneMap.assign(skel.bones.size(), -1);
        l.matched = 0;
        if (!l.anim) continue;
        for (size_t b = 0; b < skel.bones.size(); ++b)
            for (size_t k = 0; k < l.anim->Bones(); ++k)
                if (IEquals(skel.bones[b].name, l.anim->info.bones[k]))
                {
                    l.boneMap[b] = int(k);
                    ++l.matched;
                    break;
                }
    }
    // parent-first order (DFS from roots)
    order_.clear();
    std::vector<int> stack;
    for (int i = int(skel.bones.size()) - 1; i >= 0; --i)
        if (skel.bones[i].parent < 0) stack.push_back(i);
    while (!stack.empty())
    {
        int b = stack.back();
        stack.pop_back();
        order_.push_back(b);
        for (auto it = skel.bones[b].children.rbegin(); it != skel.bones[b].children.rend(); ++it) stack.push_back(*it);
    }
    if (order_.size() != skel.bones.size()) // cycles: fall back to declaration order
    {
        order_.resize(skel.bones.size());
        for (int i = 0; i < int(order_.size()); ++i) order_[i] = i;
    }
}

bool AnimStack::Active() const
{
    for (const auto& l : layers)
        if (l.enabled && l.anim && l.weight > 0.0f) return true;
    return false;
}

float AnimStack::Phase(const AnimLayer& l, double time) const
{
    double cycles = l.duration > 1e-6f ? time * l.speed / l.duration : 0.0;
    double p = cycles + l.offset;
    if (l.pingPong)
    {
        double m = p - 2.0 * std::floor(p / 2.0); // 0..2
        if (!l.loop && p >= 1.0) m = std::min(p, 2.0);
        return float(m <= 1.0 ? m : 2.0 - m);
    }
    if (l.loop) return float(p - std::floor(p));
    return float(std::clamp(p, 0.0, 1.0));
}

std::vector<bool> AnimStack::Covered(size_t bones) const
{
    std::vector<bool> out(bones, false);
    for (const auto& l : layers)
    {
        if (!l.enabled || !l.anim || l.weight <= 0.0f) continue;
        for (size_t b = 0; b < bones && b < l.boneMap.size(); ++b)
            if (l.boneMap[b] >= 0) out[b] = true;
    }
    return out;
}

std::vector<bool> AnimStack::Overridden(size_t bones) const
{
    std::vector<bool> out(bones, false);
    for (const auto& l : layers)
    {
        if (!l.enabled || !l.anim || l.mode != LayerMode::Override || l.weight <= 0.0f) continue;
        for (size_t b = 0; b < bones && b < l.boneMap.size(); ++b)
            if (l.boneMap[b] >= 0 && (l.boneWeight.empty() || (b < l.boneWeight.size() && l.boneWeight[b] > 0.0f))) out[b] = true;
    }
    return out;
}

void AnimStack::Evaluate(const Skeleton& skel, double time, std::vector<glm::mat4>& out) const
{
    const size_t nb = skel.bones.size();
    out.assign(nb, glm::mat4(1.0f));
    std::vector<float> phase(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) phase[i] = Phase(layers[i], time);
    const bool engine = missing == MissingBones::Engine;

    // Pose of layer i for bone b; false = no data. Engine mode gives Blend layers identity for such bones; an Override
    // layer (gesture, handAnim) never overrides a bone it has no data for: the bone keeps its pose relative to the
    // parent (identity there put fingers back to the bind pose in model space while the hand had moved).
    auto layerPose = [&](size_t i, int b, BonePose& p) {
        const AnimLayer& l = layers[i];
        if (b < int(l.boneMap.size()) && l.boneMap[b] >= 0)
        {
            p = l.anim->Sample(size_t(l.boneMap[b]), phase[i]);
            if (subtractStep) p.pos -= l.anim->info.move * phase[i];
            return true;
        }
        if (!engine || l.mode == LayerMode::Override) return false;
        p = BonePose{}; // identity, like the engine's per-phase matrix default
        return true;
    };
    auto active = [&](const AnimLayer& l, LayerMode m) { return l.enabled && l.anim && l.mode == m && l.weight > 0.0f; };
    // parent-relative overrides: base pose before the overrides and each override layer's own model pose per bone
    std::vector<glm::mat4> baseM(nb, glm::mat4(1.0f)), layM;
    if (overrideRelative) layM.assign(nb * layers.size(), glm::mat4(1.0f));

    for (int b : order_)
    {
        // Blend layers: weighted average
        float wsum = 0.0f;
        glm::quat q(0, 0, 0, 0), ref(1, 0, 0, 0);
        glm::vec3 pos(0.0f), scale(0.0f);
        bool first = true;
        for (size_t i = 0; i < layers.size(); ++i)
        {
            if (!active(layers[i], LayerMode::Blend)) continue;
            BonePose p;
            if (!layerPose(i, b, p)) continue;
            float w = layers[i].weight;
            if (first) { ref = p.rot; first = false; }
            if (glm::dot(ref, p.rot) < 0.0f) p.rot = -p.rot; // same hemisphere
            q += p.rot * w;
            pos += p.pos * w;
            scale += p.scale * w;
            wsum += w;
        }
        BonePose pose;
        int parent = skel.bones[b].parent;
        const bool baseHas = wsum > 0.0f;
        if (baseHas)
        {
            pose.rot = glm::normalize(q);
            pose.pos = pos / wsum;
            pose.scale = scale / wsum;
        }
        else if (!engine && parent >= 0)
            // not animated: follow the parent rigidly - its BASE pose (the overrides of the parent reach it through
            // out[parent] below, in both modes; taking out[parent] here applied them twice: fingers / face bones of
            // skeletons with more bones than the RTM drifted away while a gesture played)
            pose = BonePose::FromMatrix(baseM[parent]);

        const glm::mat4 B = pose.Matrix();
        baseM[b] = B;
        if (overrideRelative)
        {
            // Override layers in parent space: the bone's transform relative to its parent (base: base pose, layer:
            // the layer's own pose of the parent) is mixed, then put under the parent as it came out (overrides included).
            // Deformation matrices: the bind pose is identity, so no pivots are needed for the local transforms.
            const glm::mat4 baseLocal = parent >= 0 ? glm::inverse(baseM[parent]) * B : B;
            glm::mat4 local = baseLocal;
            for (size_t i = 0; i < layers.size(); ++i)
            {
                if (!active(layers[i], LayerMode::Override)) continue;
                BonePose p;
                const bool has = layerPose(i, b, p);
                glm::mat4& lm = layM[i * nb + size_t(b)];
                lm = has ? p.Matrix() : parent >= 0 ? layM[i * nb + size_t(parent)] * baseLocal : B;
                if (!has) continue;
                const auto& bw = layers[i].boneWeight;
                const float w = std::min(layers[i].weight, 1.0f) * (bw.empty() ? 1.0f : b < int(bw.size()) ? bw[b] : 0.0f);
                if (w <= 0.0f) continue;
                const glm::mat4 layerLocal = parent >= 0 ? glm::inverse(layM[i * nb + size_t(parent)]) * lm : lm;
                local = BonePose::Mix(BonePose::FromMatrix(local), BonePose::FromMatrix(layerLocal), w).Matrix();
            }
            out[b] = parent >= 0 ? out[parent] * local : local;
            continue;
        }
        // Override layers: mix towards the layer pose (model space). A bone no override layer moves (no data, or mask
        // weight 0) keeps its base pose relative to the parent, so it goes with a parent the overrides moved.
        bool overridden = false;
        for (size_t i = 0; i < layers.size(); ++i)
        {
            if (!active(layers[i], LayerMode::Override)) continue;
            BonePose p;
            if (!layerPose(i, b, p)) continue;
            const auto& bw = layers[i].boneWeight;
            const float w = std::min(layers[i].weight, 1.0f) * (bw.empty() ? 1.0f : b < int(bw.size()) ? bw[b] : 0.0f);
            if (w > 0.0f) { pose = BonePose::Mix(pose, p, w); overridden = true; }
        }
        out[b] = overridden || parent < 0 ? pose.Matrix() : out[parent] * glm::inverse(baseM[parent]) * B;
    }
}
