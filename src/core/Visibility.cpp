#include "Visibility.h"
#include "P3D.h"

#include <algorithm>
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

std::vector<std::string> SplitTabs(const std::string& line)
{
    std::vector<std::string> out;
    size_t a = 0;
    for (size_t b; (b = line.find('\t', a)) != std::string::npos; a = b + 1) out.push_back(line.substr(a, b - a));
    out.push_back(line.substr(a));
    return out;
}

std::string ModelHeader(const fs::path& model)
{
    auto u = model.u8string();
    return "[model]\t" + Lower(std::string(u.begin(), u.end()));
}

std::string JoinFaces(const auto& faces)
{
    std::string s;
    for (uint32_t f : faces) s += std::format("{} ", f);
    if (!s.empty()) s.pop_back();
    return s;
}

std::vector<uint32_t> ParseFaces(const std::string& s)
{
    std::vector<uint32_t> v;
    std::istringstream is(s);
    for (uint32_t f; is >> f;) v.push_back(f);
    return v;
}

} // namespace

std::string ModelVisibility::Key(float resolution) { return std::format("{:g}", resolution); }

void ModelVisibility::Load(Settings& settings, const fs::path& model)
{
    settings_ = &settings;
    model_ = model;
    lods_.clear();
    overlays.clear();
    std::istringstream f(settings.Block("models"));
    const std::string header = ModelHeader(model);
    bool mine = false;
    for (std::string line; std::getline(f, line);)
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("[model]")) { mine = Lower(line) == header; continue; }
        if (!mine || line.empty()) continue;
        auto t = SplitTabs(line);
        if (t[0] == "overlay" && t.size() >= 2) { overlays.insert(t[1]); continue; }
        if (t[0] != "lod" || t.size() < 3) continue;
        LodVisibility& v = lods_[t[1]];
        const std::string& what = t[2];
        if (what == "hide" && t.size() >= 4) v.hidden.insert(Lower(t[3]));
        else if (what == "hidefaces" && t.size() >= 4) for (uint32_t x : ParseFaces(t[3])) v.hiddenFaces.insert(x);
        else if (what == "isolate" && t.size() >= 4) v.isolate = t[3];
        else if (what == "isolatefaces" && t.size() >= 4) v.isolateFaces = ParseFaces(t[3]);
        else if (what == "set" && t.size() >= 5) v.sets[t[3]] = ParseFaces(t[4]);
        else if (what == "meshhidden") v.meshHidden = true;
    }
}

void ModelVisibility::Save() const
{
    if (!settings_) return;
    // keep the other models' blocks
    std::vector<std::string> keep;
    const std::string header = ModelHeader(model_);
    {
        std::istringstream f(settings_->Block("models"));
        bool mine = false;
        for (std::string line; std::getline(f, line);)
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("[model]")) mine = Lower(line) == header;
            if (!mine && !line.empty()) keep.push_back(line);
        }
    }
    std::ostringstream o;
    for (const auto& l : keep) o << l << '\n';
    bool any = !overlays.empty();
    for (const auto& [k, v] : lods_) any |= !v.Empty();
    if (any)
    {
        o << header << '\n';
        for (const auto& r : overlays) o << "overlay\t" << r << '\n';
        for (const auto& [k, v] : lods_)
        {
            for (const auto& h : v.hidden) o << "lod\t" << k << "\thide\t" << h << '\n';
            if (!v.hiddenFaces.empty()) o << "lod\t" << k << "\thidefaces\t" << JoinFaces(v.hiddenFaces) << '\n';
            if (!v.isolate.empty()) o << "lod\t" << k << "\tisolate\t" << v.isolate << '\n';
            if (!v.isolateFaces.empty()) o << "lod\t" << k << "\tisolatefaces\t" << JoinFaces(v.isolateFaces) << '\n';
            for (const auto& [n, faces] : v.sets) o << "lod\t" << k << "\tset\t" << n << '\t' << JoinFaces(faces) << '\n';
            if (v.meshHidden) o << "lod\t" << k << "\tmeshhidden\n";
        }
    }
    settings_->SetBlock("models", o.str());
    settings_->Save();
}

LodVisibility& ModelVisibility::Lod(float resolution) { return lods_[Key(resolution)]; }

const LodVisibility* ModelVisibility::Find(float resolution) const
{
    auto it = lods_.find(Key(resolution));
    return it == lods_.end() ? nullptr : &it->second;
}

bool ModelVisibility::FaceInNamed(const P3DLod& lod, const LodVisibility& v, const std::string& name, uint32_t face)
{
    if (auto it = v.sets.find(name); it != v.sets.end())
        return std::find(it->second.begin(), it->second.end(), face) != it->second.end();
    const std::string ln = Lower(name);
    for (const auto& s : lod.selections)
        if (Lower(s.name) == ln) return face < s.faceBytes.size() && s.faceBytes[face];
    return false;
}

std::vector<uint8_t> ModelVisibility::HiddenFaces(const P3DLod& lod, float resolution) const
{
    std::vector<uint8_t> hid(lod.faces.size(), 0);
    const LodVisibility* v = Find(resolution);
    if (!v) return hid;
    if (v->meshHidden)
    {
        std::fill(hid.begin(), hid.end(), uint8_t(1));
        return hid;
    }
    for (uint32_t f : v->hiddenFaces)
        if (f < hid.size()) hid[f] = 1;
    for (const auto& s : lod.selections)
        if (v->hidden.count(Lower(s.name)))
            for (size_t f = 0; f < s.faceBytes.size() && f < hid.size(); ++f)
                if (s.faceBytes[f]) hid[f] = 1;
    for (const auto& [n, faces] : v->sets)
        if (v->hidden.count(Lower(n)))
            for (uint32_t f : faces)
                if (f < hid.size()) hid[f] = 1;
    if (!v->isolate.empty())
    {
        std::vector<uint8_t> keep(hid.size(), 0);
        if (v->isolate == "#faces")
        {
            for (uint32_t f : v->isolateFaces)
                if (f < keep.size()) keep[f] = 1;
        }
        else if (auto it = v->sets.find(v->isolate); it != v->sets.end())
        {
            for (uint32_t f : it->second)
                if (f < keep.size()) keep[f] = 1;
        }
        else
        {
            const std::string ln = Lower(v->isolate);
            for (const auto& s : lod.selections)
                if (Lower(s.name) == ln)
                    for (size_t f = 0; f < s.faceBytes.size() && f < keep.size(); ++f)
                        if (s.faceBytes[f]) keep[f] = 1;
        }
        for (size_t f = 0; f < hid.size(); ++f)
            if (!keep[f]) hid[f] = 1;
    }
    return hid;
}

bool ModelVisibility::AnyHidden(float resolution) const
{
    const LodVisibility* v = Find(resolution);
    return v && (!v->hidden.empty() || !v->hiddenFaces.empty() || !v->isolate.empty() || v->meshHidden);
}

void ModelVisibility::ShowAll(float resolution)
{
    LodVisibility& v = Lod(resolution);
    v.hidden.clear();
    v.hiddenFaces.clear();
    v.isolate.clear();
    v.isolateFaces.clear();
    v.meshHidden = false;
}
