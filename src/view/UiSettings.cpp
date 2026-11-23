#include "UiSettings.h"
#include "Lang.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>

void UiSettings::Load(const Settings& settings)
{
    for (const auto& [k, v] : settings.Pairs("ui"))
    {
        RecordSettings& r = record_;
        if (k == "UiScale")
        {
            const float f = float(std::atof(v.c_str()));
            if (f == 0.0f || (f >= 0.75f && f <= 3.0f))
            {
                shell_.uiScale = f;
                shell_.uiScaleDirty = true;
            }
        }
        else if (k == "Lang") SetUiLang(v == "en" ? UiLang::En : UiLang::Ru);
        else if (k == "Started") startedSeen = v == "1";
        else if (k == "StartScreen") startScreen = v != "0";
        else if (k == "RecFfmpeg") strncpy_s(r.ffmpeg, v.c_str(), _TRUNCATE);
        else if (k == "RecDir") strncpy_s(r.outDir, v.c_str(), _TRUNCATE);
        else if (k == "RecSize") sscanf_s(v.c_str(), "%dx%d", &r.width, &r.height);
        else if (k == "RecFps") r.fps = std::atoi(v.c_str());
        else if (k == "MoveStart")
            if (const size_t tab = v.find('\t'); tab != std::string::npos) moves_.startPick[v.substr(0, tab)] = v.substr(tab + 1);
    }
    for (const auto& [k, v] : settings.Pairs("icons"))
        if (k == "ImageToPaa" && imageToPaa) *imageToPaa = v;
        else if (k.starts_with("Class:") && iconClasses) (*iconClasses)[k.substr(6)] = v;
    PointOverlaySettings ps = points_.Settings();
    ps.FromPairs(settings.Pairs("points")); // missing keys keep the defaults
    points_.SetSettings(ps);
}

void UiSettings::Store(Settings& settings) const
{
    const RecordSettings& r = record_;
    std::vector<std::pair<std::string, std::string>> p = {
        { "UiScale", std::format("{:.2f}", shell_.uiScale) },
        { "Lang", GetUiLang() == UiLang::En ? "en" : "ru" },
        { "Started", startedSeen ? "1" : "0" },
        { "StartScreen", startScreen ? "1" : "0" },
        { "RecFfmpeg", r.ffmpeg },
        { "RecDir", r.outDir },
        { "RecSize", std::format("{}x{}", r.width, r.height) },
        { "RecFps", std::to_string(r.fps) },
    };
    for (const auto& [model, state] : moves_.startPick)
        if (!state.empty()) p.emplace_back("MoveStart", model + "\t" + state);
    settings.SetPairs(p, "ui");
    settings.SetPairs(points_.Settings().ToPairs(), "points");
    std::vector<std::pair<std::string, std::string>> icons;
    if (imageToPaa) icons.emplace_back("ImageToPaa", *imageToPaa);
    if (iconClasses)
        for (const auto& [cls, v] : *iconClasses) icons.emplace_back("Class:" + cls, v);
    settings.SetPairs(icons, "icons");
}
