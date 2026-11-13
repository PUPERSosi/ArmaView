#include "ShaderSurvey.h"
#include "Log.h"
#include "Text.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <map>

#include "CoreGuard.h"

namespace fs = std::filesystem;

namespace {
const char* ShaderSupport(const std::string& id, std::string& note)
{
    const std::string s = ToLower(id);
    static const char* const kDone[] = { "normal", "normalmap", "normalmapdiffuse", "normalmapspecularmap", "normalmapspecularDIMap", "normalmapdetailspecularmap",
                                         "normalmapdetailspecularDIMap", "normalmapmacroas", "normalmapmacroasspecularmap", "normalmapmacroasspecularDIMap",
                                         "normalmapdetailmacroasspecularmap", "normalmapdetailmacroasspecularDIMap", "normalmapdiffusemacroas", "super",
                                         "detail", "detailmacroas", "white", "whitealpha" };
    for (const char* d : kDone)
        if (s == ToLower(d)) { note = "stages by suffix: _co _nohq _smdi _as _mc _dt + fresnel / env"; return "implemented"; }
    if (s.starts_with("(none")) { note = "no PixelShaderID: the engine default (Normal)"; return "implemented"; }
    if (s == "normaldxta") { note = "as Normal, alpha from the texture"; return "approximated"; }
    if (s == "multi") { note = "4 layers by the mask (_co / _nohq / _dtsmdi), macro, _adshq as ambient; no parallax"; return "approximated"; }
    if (s == "skin") { note = "Super + wrap scatter (emmisive = scatter colour / amount, no emission), rim translucency (forcedDiffuse)"; return "approximated"; }
    if (s == "superhair" || s == "hair" || s == "superext" || s.starts_with("super"))
    {
        note = "as Super (no anisotropic hair)";
        return "approximated";
    }
    if (s == "glass" || s == "alphashadow" || s == "alphanoshadow" || s.starts_with("normalmapthrough") || s.starts_with("normalmapspecularthrough"))
    {
        note = "alpha from the texture (_ca), no refraction / transmission";
        return "approximated";
    }
    note = "falls back to the Super approximation (terrain, water, sprites, trees, refraction ... not modelled)";
    return "not implemented";
}
} // namespace

void ShaderSurvey::Start(const std::filesystem::path& projectRoot)
{
    if (job_.valid()) return;
    progress = 0;
    job_ = std::async(std::launch::async, [this, projectRoot] {
        std::map<std::string, ShaderUse> out;
        std::error_code ec;
        const fs::path root = projectRoot.empty() ? fs::path("P:\\") : projectRoot;
        auto valueAfter = [](const std::string& d, size_t p) {
            // text: PixelShaderID = "Multi";   rapified: PixelShaderID\0Multi\0
            size_t q = p + 13;
            if (q < d.size() && d[q] == '\0')
            {
                size_t e = d.find('\0', q + 1);
                return e == std::string::npos ? std::string() : d.substr(q + 1, e - q - 1);
            }
            size_t a = d.find('"', q), b = a == std::string::npos ? a : d.find('"', a + 1);
            if (a == std::string::npos || b == std::string::npos || a - q > 8) return std::string();
            return d.substr(a + 1, b - a - 1);
        };
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (!it->is_regular_file(ec) || ToLower(PathToUtf8(it->path().extension())) != ".rvmat") continue;
            ++progress;
            std::ifstream f(it->path(), std::ios::binary);
            std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            size_t p = 0;
            std::string id;
            for (; (p = d.find("PixelShaderID", p)) != std::string::npos; p += 13)
                if (!(id = valueAfter(d, p)).empty()) break;
            if (id.empty()) id = "(none: default Normal)";
            const std::string rel = PathToUtf8(fs::relative(it->path(), root, ec));
            const bool a3 = ToLower(rel).starts_with("a3\\");
            ShaderUse& u = out[ToLower(id)];
            if (u.name.empty()) u.name = id;
            (a3 ? u.a3 : u.mods)++;
            if (a3 ? u.exampleA3.empty() : u.exampleMod.empty()) (a3 ? u.exampleA3 : u.exampleMod) = rel;
        }
        std::vector<ShaderUse> v;
        for (auto& [k, u] : out)
        {
            u.status = ShaderSupport(u.name, u.note);
            v.push_back(u);
        }
        std::sort(v.begin(), v.end(), [](const ShaderUse& a, const ShaderUse& b) { return a.a3 + a.mods > b.a3 + b.mods; });
        return v;
    });
}

bool ShaderSurvey::Poll()
{
    if (job_.valid() && job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        results = job_.get();
        std::string table = "PixelShaderID | status | a3 | mods | note | example\n";
        for (const auto& u : results) table += std::format("{} | {} | {} | {} | {} | {}\n", u.name, u.status, u.a3, u.mods, u.note, u.exampleMod.empty() ? u.exampleA3 : u.exampleMod);
        text = table;
        Log::Get().Info("RVMAT survey: {} shader IDs over {} files", results.size(), progress.load());
        return true;
    }
    return false;
}
