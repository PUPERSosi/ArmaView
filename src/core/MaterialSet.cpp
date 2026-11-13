#include "MaterialSet.h"
#include <algorithm>
#include <thread>
#include <atomic>
#include "ArmaConfig.h"
#include "Log.h"

#include <format>

#include "CoreGuard.h"

namespace fs = std::filesystem;

std::string MaterialSet::ToLowerName(std::string s) { return ToLower(s); }

const RvMaterial* MaterialSet::Get(const std::string& name)
{
    if (name.empty()) return nullptr;
    const std::string key = Key(name);
    if (auto it = loaded.find(key); it != loaded.end()) return &it->second;
    RvMaterial m;
    fs::path p = ResolveArmaPath(name, root_);
    std::error_code ec;
    if (!fs::exists(p, ec))
    {
        m.path = p;
        m.error = "not found";
        Log::Get().Warn("RVMAT '{}' not found ({})", name, PathToUtf8(p));
    }
    else if (LoadRvMaterial(p, root_, m))
        Log::Get().Debug("RVMAT '{}': {} / {}, {} stages", name, m.pixelShader, m.vertexShader, m.stages.size());
    return &loaded.emplace(key, std::move(m)).first->second;
}

const RvMaterial* MaterialSet::Current(const std::string& name, const RvMaterial* original) const
{
    auto it = edits.find(Key(name));
    return it != edits.end() ? &it->second : original;
}

bool MaterialSet::Save(const std::string& name, const RvMaterial& original)
{
    const std::string key = Key(name);
    auto it = edits.find(key);
    if (it == edits.end()) return false;
    std::string err;
    int n = 0;
    const bool ok = SaveRvMaterial(original, it->second, err, n);
    saveMsg = ok ? std::format("saved {} value(s) to {} (backup {}.bak)", n, PathToUtf8(original.path.filename()), PathToUtf8(original.path.filename()))
                 : std::format("save: {}", err);
    if (ok) Log::Get().Info("RVMAT {}: {}", name, saveMsg);
    else Log::Get().Error("RVMAT {}: {}", name, saveMsg);
    if (n <= 0) return false;
    edits.erase(key);
    loaded.erase(key); // re-read from disk
    return true;
}

int MaterialSet::DropChanged()
{
    int n = 0;
    std::error_code ec;
    for (auto it = loaded.begin(); it != loaded.end();)
    {
        const bool keep = it->second.loaded && fs::exists(it->second.path, ec) &&
                          fs::last_write_time(it->second.path, ec).time_since_epoch().count() == it->second.fileTime;
        if (keep) ++it;
        else { it = loaded.erase(it); ++n; }
    }
    return n;
}

int MaterialSet::DropFile(const fs::path& file)
{
    int n = 0;
    std::error_code ec;
    for (auto it = loaded.begin(); it != loaded.end();)
        if (fs::equivalent(it->second.path, file, ec)) { it = loaded.erase(it); ++n; }
        else ++it;
    return n;
}

const std::vector<const char*>& MaterialSet::PixelShaders()
{
    static const std::vector<const char*> v = { "Normal", "NormalMap", "NormalMapDiffuse", "NormalMapMacroAS", "NormalMapSpecularDIMap", "NormalMapDetailSpecularDIMap",
                                                "NormalMapMacroASSpecularDIMap", "Super", "Multi", "Glass", "Skin", "Tree", "TreeAdv", "Grass", "AlphaShadow", "Detail" };
    return v;
}

const std::vector<const char*>& MaterialSet::VertexShaders()
{
    static const std::vector<const char*> v = { "Basic", "NormalMap", "NormalMapAS", "NormalMapDiffuse", "NormalMapSpecularDIMap", "Super", "Multi", "Glass", "Skin", "Tree", "Grass" };
    return v;
}

const std::vector<const char*>& MaterialSet::UvSources()
{
    static const std::vector<const char*> v = { "tex", "tex1", "none", "pos", "norm", "worldPos", "worldNorm", "texWaterAnim" };
    return v;
}

void MaterialSet::UpdateRole(RvStage& st)
{
    if (const TexRole r = RoleFromTextureName(st.texture); r != TexRole::Count) st.role = r;
}

void MaterialSet::Prefetch(const std::vector<std::string>& names)
{
    std::vector<std::string> todo;
    for (const auto& n : names)
        if (!n.empty() && !loaded.count(Key(n)) && std::find(todo.begin(), todo.end(), n) == todo.end()) todo.push_back(n);
    if (todo.size() < 2) return;
    struct R { RvMaterial m; bool found = false, ok = false; };
    std::vector<R> res(todo.size());
    std::atomic<size_t> next{ 0 };
    const unsigned n = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), unsigned(todo.size())));
    {
        std::vector<std::jthread> pool;
        for (unsigned w = 0; w < n; ++w)
            pool.emplace_back([&] {
                for (size_t i; (i = next.fetch_add(1)) < todo.size();)
                {
                    const fs::path p = ResolveArmaPath(todo[i], root_);
                    std::error_code ec;
                    res[i].found = fs::exists(p, ec);
                    res[i].m.path = p;
                    if (res[i].found) res[i].ok = LoadRvMaterial(p, root_, res[i].m);
                }
            });
    }
    for (size_t i = 0; i < todo.size(); ++i)
    {
        R& r = res[i];
        if (!r.found)
        {
            r.m.error = "not found";
            Log::Get().Warn("RVMAT '{}' not found ({})", todo[i], PathToUtf8(r.m.path));
        }
        else if (r.ok)
            Log::Get().Debug("RVMAT '{}': {} / {}, {} stages", todo[i], r.m.pixelShader, r.m.vertexShader, r.m.stages.size());
        loaded.emplace(Key(todo[i]), std::move(r.m));
    }
}
