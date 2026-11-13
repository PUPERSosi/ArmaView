#include "AnimProject.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

#include "CoreGuard.h"

namespace fs = std::filesystem;

namespace {

std::string Lower(std::string s)
{
    for (auto& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

// ---- minimal JSON (objects, arrays, strings, numbers, booleans)
struct Json
{
    enum class T { Null, Num, Str, Bool, Arr, Obj } t = T::Null;
    double num = 0;
    bool b = false;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json* Get(const std::string& k) const
    {
        for (const auto& [n, v] : obj)
            if (n == k) return &v;
        return nullptr;
    }
    double N(const std::string& k, double d = 0) const { const Json* v = Get(k); return v && v->t == T::Num ? v->num : v && v->t == T::Bool ? double(v->b) : d; }
    std::string S(const std::string& k, const std::string& d = {}) const { const Json* v = Get(k); return v && v->t == T::Str ? v->str : d; }
};

struct JsonParser
{
    const std::string& s;
    size_t p = 0;
    void Ws() { while (p < s.size() && std::isspace(uint8_t(s[p]))) ++p; }
    Json Value()
    {
        Ws();
        Json j;
        if (p >= s.size()) return j;
        const char c = s[p];
        if (c == '{')
        {
            j.t = Json::T::Obj;
            ++p;
            for (;;)
            {
                Ws();
                if (p < s.size() && s[p] == '}') { ++p; break; }
                Json k = Value();
                Ws();
                if (p < s.size() && s[p] == ':') ++p;
                j.obj.push_back({ k.str, Value() });
                Ws();
                if (p < s.size() && s[p] == ',') ++p;
                else if (p < s.size() && s[p] == '}') { ++p; break; }
                else break;
            }
        }
        else if (c == '[')
        {
            j.t = Json::T::Arr;
            ++p;
            for (;;)
            {
                Ws();
                if (p < s.size() && s[p] == ']') { ++p; break; }
                j.arr.push_back(Value());
                Ws();
                if (p < s.size() && s[p] == ',') ++p;
                else if (p < s.size() && s[p] == ']') { ++p; break; }
                else break;
            }
        }
        else if (c == '"')
        {
            j.t = Json::T::Str;
            ++p;
            while (p < s.size() && s[p] != '"')
            {
                if (s[p] == '\\' && p + 1 < s.size()) { ++p; j.str += s[p] == 'n' ? '\n' : s[p]; }
                else j.str += s[p];
                ++p;
            }
            ++p;
        }
        else if (s.compare(p, 4, "true") == 0) { j.t = Json::T::Bool; j.b = true; p += 4; }
        else if (s.compare(p, 5, "false") == 0) { j.t = Json::T::Bool; p += 5; }
        else if (s.compare(p, 4, "null") == 0) p += 4;
        else
        {
            j.t = Json::T::Num;
            size_t e = p;
            while (e < s.size() && (std::isdigit(uint8_t(s[e])) || s[e] == '-' || s[e] == '+' || s[e] == '.' || s[e] == 'e' || s[e] == 'E')) ++e;
            j.num = std::atof(s.substr(p, e - p).c_str());
            p = e;
        }
        return j;
    }
};

std::string Esc(const std::string& s)
{
    std::string o;
    for (char c : s)
    {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

glm::mat4 Rot(const glm::vec3& deg)
{
    return glm::mat4_cast(glm::quat(glm::radians(deg)));
}

// smooth interpolation of offset keys over the phase
LayerKey KeyAt(const std::vector<LayerKey>& keys, float ph)
{
    if (keys.empty()) return {};
    if (keys.size() == 1 || ph <= keys.front().phase) return keys.front();
    for (size_t i = 1; i < keys.size(); ++i)
        if (ph <= keys[i].phase)
        {
            float t = (ph - keys[i - 1].phase) / std::max(keys[i].phase - keys[i - 1].phase, 1e-5f);
            t = t * t * (3 - 2 * t);
            return { ph, glm::mix(keys[i - 1].rotDeg, keys[i].rotDeg, t), glm::mix(keys[i - 1].pos, keys[i].pos, t) };
        }
    return keys.back();
}

float CurveWeight(const CorrectionLayer& l, float ph)
{
    if (l.weightCurve.empty()) return l.weight;
    if (ph <= l.weightCurve.front().first) return l.weight * l.weightCurve.front().second;
    for (size_t i = 1; i < l.weightCurve.size(); ++i)
        if (ph <= l.weightCurve[i].first)
        {
            const float t = (ph - l.weightCurve[i - 1].first) / std::max(l.weightCurve[i].first - l.weightCurve[i - 1].first, 1e-5f);
            return l.weight * (l.weightCurve[i - 1].second + (l.weightCurve[i].second - l.weightCurve[i - 1].second) * t);
        }
    return l.weight * l.weightCurve.back().second;
}

// model-space pose of skeleton bones at a phase; bones the clip lacks follow their parent
void SamplePose(const RtmAnimation& a, const Skeleton& skel, const std::vector<int>& order, float phase, std::vector<glm::mat4>& out)
{
    out.assign(skel.bones.size(), glm::mat4(1.0f));
    std::vector<int> map(skel.bones.size(), -1);
    for (size_t b = 0; b < skel.bones.size(); ++b)
        for (size_t k = 0; k < a.info.bones.size(); ++k)
            if (Lower(a.info.bones[k]) == Lower(skel.bones[b].name)) { map[b] = int(k); break; }
    for (int b : order)
    {
        const int p = skel.bones[size_t(b)].parent;
        out[size_t(b)] = map[size_t(b)] >= 0 ? a.Sample(size_t(map[size_t(b)]), phase).Matrix() : p >= 0 ? out[size_t(p)] : glm::mat4(1.0f);
    }
}

glm::vec3 Joint(const std::vector<glm::mat4>& m, const Skeleton& skel, int b) { return glm::vec3(m[size_t(b)] * glm::vec4(skel.bones[size_t(b)].pivot, 1.0f)); }

void ApplyToSubtree(std::vector<glm::mat4>& m, const Skeleton& skel, int root, const glm::mat4& d, int except = -1)
{
    for (size_t x = 0; x < skel.bones.size(); ++x)
    {
        if (except >= 0 && (int(x) == except || skel.IsAncestor(except, int(x)))) continue;
        if (int(x) == root || skel.IsAncestor(root, int(x))) m[x] = d * m[x];
    }
}

glm::quat FromTo(glm::vec3 a, glm::vec3 b)
{
    if (glm::length(a) < 1e-6f || glm::length(b) < 1e-6f) return glm::quat(1, 0, 0, 0);
    a = glm::normalize(a);
    b = glm::normalize(b);
    const float d = glm::dot(a, b);
    if (d > 0.99999f) return glm::quat(1, 0, 0, 0);
    if (d < -0.99999f) return glm::angleAxis(glm::pi<float>(), glm::normalize(std::abs(a.x) < 0.9f ? glm::cross(a, glm::vec3(1, 0, 0)) : glm::cross(a, glm::vec3(0, 1, 0))));
    const glm::vec3 c = glm::cross(a, b);
    return glm::normalize(glm::quat(1 + d, c.x, c.y, c.z));
}

glm::mat4 About(const glm::vec3& pivot, const glm::quat& q) { return glm::translate(glm::mat4(1.0f), pivot) * glm::mat4_cast(q) * glm::translate(glm::mat4(1.0f), -pivot); }

} // namespace

// ============================================================================================ JSON io

std::string AnimProjectText(const AnimProject& p)
{
    std::ostringstream o;
    o << "{\n  \"armaview\": \"animation project\",\n";
    o << "  \"source\": \"" << Esc(p.source) << "\",\n  \"duration\": " << p.duration << ",\n  \"output\": \"" << Esc(p.output) << "\",\n";
    const ClipOps& c = p.ops;
    o << std::format("  \"clip\": {{ \"trimStart\": {}, \"trimEnd\": {}, \"speed\": {}, \"mirror\": {}, \"seamFrames\": {}, \"alignRoot\": {}, \"footLocks\": [",
                     c.trimStart, c.trimEnd, c.speed, c.mirror ? "true" : "false", c.seamFrames, c.alignRoot ? "true" : "false");
    for (size_t i = 0; i < c.footLocks.size(); ++i)
        o << (i ? ", " : "") << std::format("{{ \"foot\": \"{}\", \"from\": {}, \"to\": {} }}", Esc(c.footLocks[i].foot), c.footLocks[i].from, c.footLocks[i].to);
    o << "] },\n  \"layers\": [\n";
    for (size_t i = 0; i < p.layers.size(); ++i)
    {
        const auto& l = p.layers[i];
        const char* type = l.type == CorrectionLayer::Type::Offset ? "offset" : l.type == CorrectionLayer::Type::Additive ? "additive" : "replace";
        o << std::format("    {{ \"name\": \"{}\", \"type\": \"{}\", \"enabled\": {}, \"weight\": {}, \"rtm\": \"{}\", \"armaMask\": \"{}\", \"falloff\": {},\n",
                         Esc(l.name), type, l.enabled ? "true" : "false", l.weight, Esc(l.rtm), Esc(l.armaMask), l.falloff);
        o << "      \"bones\": [";
        for (size_t k = 0; k < l.bones.size(); ++k) o << (k ? ", " : "") << std::format("[\"{}\", {}]", Esc(l.bones[k].first), l.bones[k].second);
        o << "],\n      \"weightCurve\": [";
        for (size_t k = 0; k < l.weightCurve.size(); ++k) o << (k ? ", " : "") << std::format("[{}, {}]", l.weightCurve[k].first, l.weightCurve[k].second);
        o << "],\n      \"keys\": [";
        for (size_t k = 0; k < l.keys.size(); ++k)
        {
            const auto& kk = l.keys[k];
            o << (k ? ", " : "") << std::format("{{ \"phase\": {}, \"rot\": [{}, {}, {}], \"pos\": [{}, {}, {}] }}", kk.phase, kk.rotDeg.x, kk.rotDeg.y, kk.rotDeg.z,
                                               kk.pos.x, kk.pos.y, kk.pos.z);
        }
        o << "] }" << (i + 1 < p.layers.size() ? "," : "") << "\n";
    }
    o << "  ]\n}\n";
    return o.str();
}

bool SaveAnimProject(const fs::path& file, const AnimProject& p, std::string& error)
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::trunc);
    f << AnimProjectText(p);
    if (!f) { error = "cannot write " + file.string(); return false; }
    return true;
}

bool LoadAnimProject(const fs::path& file, AnimProject& p, std::string& error)
{
    std::ifstream f(file);
    if (!f) { error = "cannot read " + file.string(); return false; }
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    JsonParser jp{ text };
    const Json root = jp.Value();
    if (root.t != Json::T::Obj) { error = "not a JSON object"; return false; }
    p = {};
    p.source = root.S("source");
    p.duration = float(root.N("duration", 1));
    p.output = root.S("output");
    if (const Json* c = root.Get("clip"))
    {
        p.ops.trimStart = float(c->N("trimStart", 0));
        p.ops.trimEnd = float(c->N("trimEnd", 1));
        p.ops.speed = float(c->N("speed", 1));
        p.ops.mirror = c->N("mirror", 0) != 0;
        p.ops.seamFrames = int(c->N("seamFrames", 0));
        p.ops.alignRoot = c->N("alignRoot", 0) != 0;
        if (const Json* fl = c->Get("footLocks"))
            for (const auto& x : fl->arr) p.ops.footLocks.push_back({ x.S("foot", "LeftFoot"), float(x.N("from")), float(x.N("to")) });
    }
    if (const Json* ls = root.Get("layers"))
        for (const auto& x : ls->arr)
        {
            CorrectionLayer l;
            l.name = x.S("name", "layer");
            const std::string t = x.S("type", "offset");
            l.type = t == "additive" ? CorrectionLayer::Type::Additive : t == "replace" ? CorrectionLayer::Type::Replace : CorrectionLayer::Type::Offset;
            l.enabled = x.N("enabled", 1) != 0;
            l.weight = float(x.N("weight", 1));
            l.rtm = x.S("rtm");
            l.armaMask = x.S("armaMask");
            l.falloff = float(x.N("falloff", 0));
            if (const Json* b = x.Get("bones"))
                for (const auto& e : b->arr)
                    if (e.arr.size() >= 2) l.bones.push_back({ e.arr[0].str, float(e.arr[1].num) });
            if (const Json* w = x.Get("weightCurve"))
                for (const auto& e : w->arr)
                    if (e.arr.size() >= 2) l.weightCurve.push_back({ float(e.arr[0].num), float(e.arr[1].num) });
            l.keys.clear();
            if (const Json* k = x.Get("keys"))
                for (const auto& e : k->arr)
                {
                    LayerKey kk;
                    kk.phase = float(e.N("phase"));
                    if (const Json* r = e.Get("rot"); r && r->arr.size() == 3) kk.rotDeg = { float(r->arr[0].num), float(r->arr[1].num), float(r->arr[2].num) };
                    if (const Json* q = e.Get("pos"); q && q->arr.size() == 3) kk.pos = { float(q->arr[0].num), float(q->arr[1].num), float(q->arr[2].num) };
                    l.keys.push_back(kk);
                }
            if (l.keys.empty()) l.keys.push_back({});
            p.layers.push_back(std::move(l));
        }
    return true;
}

// ============================================================================================ masks

std::vector<float> LayerMaskWeights(const CorrectionLayer& l, const Skeleton& skel, const std::map<std::string, BoneMask>& armaMasks)
{
    std::vector<float> w(skel.bones.size(), 0.0f);
    if (!l.armaMask.empty())
    {
        if (auto it = armaMasks.find(Lower(l.armaMask)); it != armaMasks.end())
            for (size_t b = 0; b < skel.bones.size(); ++b)
                if (auto jt = it->second.find(Lower(skel.bones[b].name)); jt != it->second.end()) w[b] = jt->second;
        return w;
    }
    // own bones: weight, children weight x falloff per level (the strongest wins)
    for (const auto& [name, weight] : l.bones)
    {
        const int b = skel.Find(name);
        if (b < 0) continue;
        std::vector<std::pair<int, float>> stack{ { b, weight } };
        while (!stack.empty())
        {
            auto [x, v] = stack.back();
            stack.pop_back();
            w[size_t(x)] = std::max(w[size_t(x)], v);
            if (l.falloff > 0.0f && v * l.falloff > 0.01f)
                for (int c : skel.bones[size_t(x)].children) stack.push_back({ c, v * l.falloff });
        }
    }
    return w;
}

// ============================================================================================ bake

std::shared_ptr<RtmAnimation> BakeAnimProject(const AnimProject& p, const RtmAnimation& base, const Skeleton& skel,
                                              const std::map<std::string, BoneMask>& armaMasks, const RtmLoader& load, std::string& report)
{
    const size_t nb = skel.bones.size();
    if (!nb || base.Frames() < 1) { report = "no skeleton or empty clip"; return nullptr; }
    const auto order = skel.ParentFirstOrder();
    const ClipOps& c = p.ops;
    const float t0 = std::clamp(c.trimStart, 0.0f, 1.0f), t1 = std::clamp(std::max(c.trimEnd, t0 + 1e-3f), 0.0f, 1.0f);
    const size_t frames = std::max<size_t>(2, size_t(std::round(float(base.Frames() - 1) * (t1 - t0))) + 1);
    // L/R pairs for mirroring
    std::vector<int> pair(nb);
    for (size_t b = 0; b < nb; ++b)
    {
        pair[b] = int(b);
        const std::string n = Lower(skel.bones[b].name);
        std::string other;
        if (n.find("left") != std::string::npos) other = n.substr(0, n.find("left")) + "right" + n.substr(n.find("left") + 4);
        else if (n.find("right") != std::string::npos) other = n.substr(0, n.find("right")) + "left" + n.substr(n.find("right") + 5);
        if (!other.empty()) if (int o = skel.Find(other); o >= 0) pair[b] = o;
    }
    const glm::mat4 S = glm::scale(glm::mat4(1.0f), glm::vec3(-1, 1, 1));
    // layer inputs
    struct L { const CorrectionLayer* l; std::vector<float> mask; std::shared_ptr<RtmAnimation> rtm; };
    std::vector<L> layers;
    for (const auto& l : p.layers)
    {
        if (!l.enabled) continue;
        L x{ &l, LayerMaskWeights(l, skel, armaMasks), nullptr };
        if (l.type != CorrectionLayer::Type::Offset)
        {
            x.rtm = load(l.rtm);
            if (!x.rtm) { report += std::format("layer {}: RTM {} not loaded; ", l.name, l.rtm); continue; }
        }
        if (l.type == CorrectionLayer::Type::Offset && l.armaMask.empty() && l.bones.empty()) { report += std::format("layer {}: empty mask; ", l.name); continue; }
        layers.push_back(std::move(x));
    }
    auto out = std::make_shared<RtmAnimation>();
    out->path = base.path;
    out->info.signature = "RTM_0101";
    out->info.move = base.info.move * (t1 - t0);
    if (c.mirror) out->info.move.x = -out->info.move.x;
    for (const auto& b : skel.bones) out->info.bones.push_back(b.name);
    out->info.frames = uint32_t(frames);
    out->poses.resize(frames * nb);
    std::vector<std::vector<glm::mat4>> all(frames);
    const int pelvis = skel.Find("Pelvis");
    for (size_t f = 0; f < frames; ++f)
    {
        const float tn = float(f) / float(frames - 1), ph = t0 + tn * (t1 - t0);
        out->times.push_back(tn);
        std::vector<glm::mat4> m;
        SamplePose(base, skel, order, ph, m);
        if (c.mirror)
        {
            std::vector<glm::mat4> mm(nb);
            for (size_t b = 0; b < nb; ++b) mm[b] = S * m[size_t(pair[b])] * S;
            m = std::move(mm);
        }
        for (const auto& L : layers)
        {
            const float lw = CurveWeight(*L.l, tn);
            if (lw <= 0.0f) continue;
            if (L.l->type == CorrectionLayer::Type::Offset)
            {
                const LayerKey k = KeyAt(L.l->keys, tn);
                for (int b : order)
                {
                    const float w = L.mask[size_t(b)] * lw;
                    if (w <= 0.0f) continue;
                    const glm::vec3 pv = Joint(m, skel, b);
                    const glm::quat rb = BonePose::FromMatrix(m[size_t(b)]).rot;
                    const glm::quat q = rb * glm::quat(glm::radians(k.rotDeg * w)) * glm::inverse(rb);
                    const glm::mat4 d = glm::translate(glm::mat4(1.0f), rb * (k.pos * w)) * About(pv, q);
                    ApplyToSubtree(m, skel, b, d);
                }
            }
            else
            {
                std::vector<glm::mat4> r, r0;
                SamplePose(*L.rtm, skel, order, tn, r);
                if (L.l->type == CorrectionLayer::Type::Additive) SamplePose(*L.rtm, skel, order, 0.0f, r0);
                for (size_t b = 0; b < nb; ++b)
                {
                    const float w = L.mask[b] * lw;
                    if (w <= 0.0f) continue;
                    if (L.l->type == CorrectionLayer::Type::Replace)
                        m[b] = BonePose::Mix(BonePose::FromMatrix(m[b]), BonePose::FromMatrix(r[b]), std::min(w, 1.0f)).Matrix();
                    else
                    {
                        const BonePose d = BonePose::Mix(BonePose{}, BonePose::FromMatrix(r[b] * glm::inverse(r0[b])), std::min(w, 1.0f));
                        m[b] = d.Matrix() * m[b];
                    }
                }
            }
        }
        all[f] = std::move(m);
    }
    // foot IK locks: the foot stays where it was at the start of the interval
    int locks = 0;
    for (const auto& fl : c.footLocks)
    {
        const int foot = skel.Find(fl.foot);
        if (foot < 0) continue;
        const std::string side = Lower(fl.foot).starts_with("right") ? "Right" : "Left";
        const int up = skel.Find(side + "UpLeg"), knee = skel.Find(side + "Leg");
        if (up < 0 || knee < 0) continue;
        const size_t f0 = size_t(std::round(std::clamp(fl.from, 0.0f, 1.0f) * float(frames - 1))), f1 = size_t(std::round(std::clamp(fl.to, 0.0f, 1.0f) * float(frames - 1)));
        const glm::vec3 goal = Joint(all[f0], skel, foot);
        for (size_t f = f0; f <= f1 && f < frames; ++f)
        {
            auto& m = all[f];
            const glm::vec3 H = Joint(m, skel, up), K = Joint(m, skel, knee), F = Joint(m, skel, foot);
            const float a = glm::length(skel.bones[size_t(knee)].pivot - skel.bones[size_t(up)].pivot), b = glm::length(skel.bones[size_t(foot)].pivot - skel.bones[size_t(knee)].pivot);
            const float d = std::clamp(glm::length(goal - H), std::abs(a - b) + 1e-4f, a + b - 1e-4f);
            const glm::vec3 dir = glm::normalize(goal - H);
            glm::vec3 n = glm::cross(F - H, K - H);
            n = glm::length(n) < 1e-6f ? glm::vec3(1, 0, 0) : glm::normalize(n);
            glm::vec3 u = glm::normalize(glm::cross(n, dir));
            if (glm::dot(u, K - H) < 0) u = -u;
            const float cosA = std::clamp((a * a + d * d - b * b) / (2 * a * d), -1.0f, 1.0f);
            const glm::vec3 K2 = H + a * (cosA * dir + std::sqrt(1 - cosA * cosA) * u);
            const glm::quat dUp = FromTo(K - H, K2 - H);
            ApplyToSubtree(m, skel, up, About(H, dUp), foot);
            const glm::vec3 F1 = Joint(m, skel, foot) == F ? K2 + dUp * (F - K) : K2 + dUp * (F - K);
            const glm::quat dKnee = FromTo(F1 - K2, H + dir * d - K2);
            ApplyToSubtree(m, skel, knee, About(K2, dKnee), foot);
            // the foot keeps its rotation, moves onto the chain end
            const glm::vec3 legEnd = Joint(m, skel, knee) + dKnee * (dUp * (F - K));
            ApplyToSubtree(m, skel, foot, glm::translate(glm::mat4(1.0f), legEnd - Joint(m, skel, foot)));
        }
        ++locks;
    }
    // root alignment: the pelvis keeps its frame-0 X / Z (no drift)
    if (c.alignRoot && pelvis >= 0)
    {
        const glm::vec3 p0 = Joint(all[0], skel, pelvis);
        for (auto& m : all)
        {
            const glm::vec3 pf = Joint(m, skel, pelvis);
            const glm::mat4 d = glm::translate(glm::mat4(1.0f), glm::vec3(p0.x - pf.x, 0, p0.z - pf.z));
            for (auto& x : m) x = d * x;
        }
    }
    // loop seam: the last N frames blend into the first pose
    const int seam = std::min<int>(c.seamFrames, int(frames) - 1);
    for (int k = 0; k < seam; ++k)
    {
        const size_t f = frames - size_t(seam) + size_t(k);
        float t = float(k + 1) / float(seam);
        t = t * t * (3 - 2 * t);
        for (size_t b = 0; b < nb; ++b) all[f][b] = BonePose::Mix(BonePose::FromMatrix(all[f][b]), BonePose::FromMatrix(all[0][b]), t).Matrix();
    }
    for (size_t f = 0; f < frames; ++f)
        for (size_t b = 0; b < nb; ++b) out->poses[f * nb + b] = BonePose::FromMatrix(all[f][b]);
    // how much the layers / ops moved the joints against the base (feedback for the user)
    float moved = 0;
    std::string where;
    for (size_t f = 0; f < frames; f += std::max<size_t>(1, frames / 8))
    {
        std::vector<glm::mat4> b0;
        SamplePose(base, skel, order, t0 + float(f) / float(frames - 1) * (t1 - t0), b0);
        for (size_t b = 0; b < nb; ++b)
        {
            const float d = glm::length(Joint(all[f], skel, int(b)) - Joint(b0, skel, int(b)));
            if (d > moved) { moved = d; where = skel.bones[b].name; }
        }
    }
    report += std::format("joints moved up to {:.3f} m vs the base ({}); ", moved, where);
    report += std::format("{} frames (trim {:.2f}..{:.2f}), {} layer(s), {}{} foot lock(s){}{}; cycle {:.2f} s -> config speed {:g}", frames, t0, t1, layers.size(),
                          c.mirror ? "mirrored, " : "", locks, c.alignRoot ? ", root aligned" : "", seam ? std::format(", seam fixed over {} frames", seam) : "",
                          p.duration * (t1 - t0) / std::max(c.speed, 1e-3f), -(p.duration * (t1 - t0) / std::max(c.speed, 1e-3f)));
    return out;
}
