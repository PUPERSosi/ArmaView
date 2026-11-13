#pragma once
// World lighting from CfgWorlds >> <world> >> Weather >> LightingNew: entries by sun angle and overcast with colours
// as {rgb}, EV (radiance = rgb * 2^EV). Sampled at the current sun elevation / overcast and normalised by an auto
// exposure (the game adapts with apertureMin..Max; here a clamped adaptation, marked as approximate in the UI).
#include <glm/glm.hpp>

#include <string>
#include <vector>

class ArmaConfig;
struct LightingResult;

class WorldLighting
{
public:
    static std::vector<std::string> Worlds(const ArmaConfig& cfg); // CfgWorlds classes with a LightingNew table
    bool Load(const ArmaConfig& cfg, const std::string& world);
    bool Loaded() const { return !entries_.empty(); }
    const std::string& Name() const { return name_; }
    float Latitude() const { return latitude_; } // degrees north (config latitude is negative for the north)
    size_t Entries() const { return entries_.size(); }
    // Replaces light / sky / ground colours of r (direction, moon flag stay); sunIntensity / ambientIntensity scale.
    void Apply(LightingResult& r, float overcast, float sunIntensity, float ambientIntensity) const;
    mutable std::string lastInfo; // sampled values for the UI

private:
    struct Entry
    {
        float sunAngle = 0, overcast = 0;
        glm::vec3 diffuse{ 0 }, ambient{ 0 }, ground{ 0 }, sky{ 0 };
    };
    struct Sample { glm::vec3 diffuse{ 0 }, ambient{ 0 }, ground{ 0 }, sky{ 0 }; };
    Sample At(float sunAngle, float overcast) const;
    std::vector<Entry> entries_;
    std::string name_;
    float latitude_ = 0;
};
