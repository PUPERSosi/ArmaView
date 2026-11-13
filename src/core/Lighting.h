#pragma once
// Stage 6 lighting: sun (or moon) from time of day / latitude / season, sky + ground ambient, overcast, presets.
// World frame for the sun: +X east, +Y up, +Z north, then rotated around Y by `rotation` (turns the sun around the model).
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

enum class LightPreset { Noon, Sunset, Night, Overcast };

struct LightingResult
{
    glm::vec3 lightDir{ 0, -1, 0 }; // direction the light travels
    glm::vec3 lightColor{ 1.0f };   // linear
    glm::vec3 skyColor{ 0.3f };     // ambient from above (linear)
    glm::vec3 groundColor{ 0.15f }; // ambient from below (linear)
    glm::vec3 background{ 0.2f };   // gamma-space clear colour following the sky
    float exposure = 1.0f;
    float sunElevation = 0.0f, sunAzimuth = 0.0f; // degrees; azimuth 0 = north, 90 = east
    bool moon = false;
};

struct Lighting
{
    bool headlight = false;
    float timeOfDay = 10.0f;    // hours 0..24
    float latitude = 40.0f;     // degrees
    float declination = 0.0f;   // degrees: -23.4 (winter) .. +23.4 (summer)
    float rotation = 0.0f;      // degrees
    float overcast = 0.0f;      // 0 clear .. 1 fully overcast
    float sunIntensity = 1.0f, ambientIntensity = 1.0f, exposure = 1.0f;

    void Apply(LightPreset p)
    {
        headlight = false;
        overcast = 0.0f;
        switch (p)
        {
        case LightPreset::Noon: timeOfDay = 12.0f; break;
        case LightPreset::Sunset: timeOfDay = 17.6f; break;
        case LightPreset::Night: timeOfDay = 0.0f; break;
        case LightPreset::Overcast: timeOfDay = 12.0f; overcast = 0.9f; break;
        }
    }

    LightingResult Evaluate(const glm::vec3& camForward, const glm::vec3& camUp, const glm::vec3& camRight) const
    {
        auto smooth = [](float a, float b, float x) { float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3 - 2 * t); };
        const float d2r = 3.14159265f / 180.0f;
        LightingResult r;
        r.exposure = exposure;
        if (headlight)
        {
            r.lightDir = glm::normalize(camForward * 0.8f - camUp * 0.35f + camRight * 0.25f);
            r.lightColor = glm::vec3(1.0f) * sunIntensity;
            r.skyColor = glm::vec3(0.32f, 0.34f, 0.38f) * ambientIntensity;
            r.groundColor = glm::vec3(0.18f, 0.17f, 0.16f) * ambientIntensity;
            r.background = glm::vec3(0.20f, 0.22f, 0.25f);
            return r;
        }
        // Solar position (hour angle H, declination, latitude) in east/north/up.
        float H = (timeOfDay - 12.0f) * 15.0f * d2r, lat = latitude * d2r, dec = declination * d2r;
        float east = -std::cos(dec) * std::sin(H);
        float north = std::sin(dec) * std::cos(lat) - std::cos(dec) * std::cos(H) * std::sin(lat);
        float up = std::sin(dec) * std::sin(lat) + std::cos(dec) * std::cos(H) * std::cos(lat);
        r.sunElevation = std::asin(std::clamp(up, -1.0f, 1.0f)) / d2r;
        r.sunAzimuth = std::fmod(std::atan2(east, north) / d2r + 360.0f, 360.0f);
        float rot = rotation * d2r;
        glm::vec3 sun(east * std::cos(rot) + north * std::sin(rot), up, -east * std::sin(rot) + north * std::cos(rot));
        sun = glm::normalize(sun);

        const float e = r.sunElevation;
        const float day = smooth(-4.0f, 8.0f, e);       // sun contribution
        const float golden = 1.0f - smooth(4.0f, 30.0f, e);
        const float skyDay = smooth(-10.0f, 15.0f, e);
        glm::vec3 sunCol = glm::mix(glm::vec3(1.0f, 0.96f, 0.90f), glm::vec3(1.0f, 0.55f, 0.30f), golden) * day * 1.1f;
        glm::vec3 sky = glm::mix(glm::vec3(0.020f, 0.028f, 0.055f), glm::vec3(0.30f, 0.40f, 0.55f), skyDay);
        sky = glm::mix(sky, glm::vec3(0.42f, 0.32f, 0.30f) * skyDay, golden * skyDay * 0.6f);
        glm::vec3 ground = glm::mix(glm::vec3(0.010f, 0.012f, 0.018f), glm::vec3(0.20f, 0.18f, 0.15f), skyDay);
        glm::vec3 dir = sun;
        if (e < -4.0f) // night: a dim bluish moon opposite the sun
        {
            r.moon = true;
            dir = glm::normalize(glm::vec3(-sun.x, std::max(0.35f, -sun.y), -sun.z));
            sunCol = glm::vec3(0.16f, 0.20f, 0.30f) * smooth(-4.0f, -12.0f, e);
        }
        // Overcast: sun mostly gone, flat bright-grey sky
        glm::vec3 grey = glm::vec3(0.50f, 0.52f, 0.55f) * (0.08f + 0.92f * skyDay);
        sunCol *= 1.0f - 0.85f * overcast;
        sky = glm::mix(sky, grey, overcast);
        ground = glm::mix(ground, grey * 0.45f, overcast);

        r.lightDir = -dir;
        r.lightColor = sunCol * sunIntensity;
        r.skyColor = sky * ambientIntensity;
        r.groundColor = ground * ambientIntensity;
        glm::vec3 bg = glm::min(sky * 1.3f, glm::vec3(1.0f));
        r.background = glm::vec3(std::pow(bg.r, 1 / 2.2f), std::pow(bg.g, 1 / 2.2f), std::pow(bg.b, 1 / 2.2f));
        return r;
    }
};
