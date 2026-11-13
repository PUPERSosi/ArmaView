#include "WorldLight.h"
#include "ArmaConfig.h"
#include "Lighting.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

#include "CoreGuard.h"

namespace {

// {{r,g,b}, ev} -> rgb * 2^ev; {r,g,b} -> rgb
glm::vec3 Radiance(const CfgClass& c, const char* entry)
{
    const CfgEntry* e = c.Find(entry);
    if (!e || e->value.type != CfgValue::Type::Array) return glm::vec3(0);
    const auto& it = e->value.items;
    auto rgb = [](const CfgValue& v) {
        glm::vec3 out(0);
        for (int i = 0; i < 3 && i < int(v.items.size()); ++i) out[i] = float(v.items[i].num);
        return out;
    };
    if (!it.empty() && it[0].type == CfgValue::Type::Array) return rgb(it[0]) * std::exp2(it.size() > 1 ? float(it[1].num) : 0.0f);
    return rgb(e->value);
}

float Lum(const glm::vec3& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

} // namespace

std::vector<std::string> WorldLighting::Worlds(const ArmaConfig& cfg)
{
    std::vector<std::string> out;
    const CfgClass* worlds = cfg.Root().FindClass("CfgWorlds");
    if (!worlds) return out;
    for (const CfgClass* w : MergedChildClasses(*worlds))
    {
        const CfgClass* weather = w->FindClass("Weather");
        const CfgClass* ln = weather ? weather->FindClass("LightingNew") : nullptr;
        if (ln && !MergedChildClasses(*ln).empty() && !w->GetString("description").empty()) out.push_back(w->name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool WorldLighting::Load(const ArmaConfig& cfg, const std::string& world)
{
    entries_.clear();
    name_ = world;
    const CfgClass* worlds = cfg.Root().FindClass("CfgWorlds");
    const CfgClass* w = worlds ? worlds->FindClass(world) : nullptr;
    const CfgClass* weather = w ? w->FindClass("Weather") : nullptr;
    const CfgClass* ln = weather ? weather->FindClass("LightingNew") : nullptr;
    if (!ln) return false;
    latitude_ = -float(w->GetNumber("latitude", -40.0)); // Arma: negative = north
    for (const CfgClass* k : MergedChildClasses(*ln))
    {
        Entry e;
        e.sunAngle = float(k->GetNumber("sunAngle", 0));
        e.overcast = float(k->GetNumber("overcast", 0));
        e.diffuse = Radiance(*k, "diffuse");
        e.ambient = Radiance(*k, "ambient");
        e.ground = Radiance(*k, "groundReflection");
        e.sky = Radiance(*k, "sky");
        entries_.push_back(e);
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.overcast != b.overcast ? a.overcast < b.overcast : a.sunAngle < b.sunAngle; });
    return !entries_.empty();
}

WorldLighting::Sample WorldLighting::At(float sunAngle, float overcast) const
{
    // groups by overcast, each interpolated by sun angle, then between the two groups around the overcast
    std::map<float, std::vector<const Entry*>> groups;
    for (const auto& e : entries_) groups[e.overcast].push_back(&e);
    auto inGroup = [&](const std::vector<const Entry*>& g) {
        Sample s;
        if (g.empty()) return s;
        const Entry* a = g.front();
        const Entry* b = g.front();
        for (const Entry* e : g)
        {
            if (e->sunAngle <= sunAngle) a = e;
            if (e->sunAngle >= sunAngle) { b = e; break; }
            b = e;
        }
        const float t = b->sunAngle > a->sunAngle ? std::clamp((sunAngle - a->sunAngle) / (b->sunAngle - a->sunAngle), 0.0f, 1.0f) : 0.0f;
        s.diffuse = glm::mix(a->diffuse, b->diffuse, t);
        s.ambient = glm::mix(a->ambient, b->ambient, t);
        s.ground = glm::mix(a->ground, b->ground, t);
        s.sky = glm::mix(a->sky, b->sky, t);
        return s;
    };
    if (groups.empty()) return {};
    auto hi = groups.lower_bound(overcast);
    if (hi == groups.end()) return inGroup(std::prev(hi)->second);
    if (hi == groups.begin() || hi->first == overcast) return inGroup(hi->second);
    auto lo = std::prev(hi);
    const float t = (overcast - lo->first) / (hi->first - lo->first);
    const Sample a = inGroup(lo->second), b = inGroup(hi->second);
    return { glm::mix(a.diffuse, b.diffuse, t), glm::mix(a.ambient, b.ambient, t), glm::mix(a.ground, b.ground, t), glm::mix(a.sky, b.sky, t) };
}

void WorldLighting::Apply(LightingResult& r, float overcast, float sunIntensity, float ambientIntensity) const
{
    if (entries_.empty()) return;
    const Sample s = At(r.sunElevation, overcast);
    // exposure adapts to the sky light (ambient) like the eye: ambient -> 0.45 in the viewer's units, sun and sky by
    // the same factor; the adaptation is clamped (6 EV over noon) so nights stay dark (the game's aperture range)
    const Sample noon = At(60.0f, overcast);
    auto adapt = [](const Sample& x, float elev) { return Lum(x.ambient) + 0.35f * Lum(x.diffuse) * std::max(std::sin(glm::radians(elev)), 0.0f); };
    const float kNoon = 0.9f / std::max(adapt(noon, 60.0f), 1e-6f);
    const float k = std::min(0.9f / std::max(adapt(s, r.sunElevation), 1e-9f), kNoon * 64.0f);
    r.lightColor = s.diffuse * k * sunIntensity;
    r.skyColor = s.ambient * k * ambientIntensity;
    r.groundColor = s.ground * k * ambientIntensity;
    glm::vec3 bg = glm::min(s.sky * k * 0.9f, glm::vec3(1.0f));
    r.background = glm::vec3(std::pow(bg.r, 1 / 2.2f), std::pow(bg.g, 1 / 2.2f), std::pow(bg.b, 1 / 2.2f));
    lastInfo = std::format("{} at sun {:.1f} deg, overcast {:.2f}: diffuse EV {:.1f}, ambient EV {:.1f}, exposure x{:.3g}", name_, r.sunElevation, overcast,
                           std::log2(std::max(Lum(s.diffuse), 1e-9f)), std::log2(std::max(Lum(s.ambient), 1e-9f)), k);
}
