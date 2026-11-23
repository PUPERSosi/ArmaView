// RVMAT: parsing (text and rapified), saving changed values back into the text, config classes that re-texture a model.
#include "RvMaterial.h"
#include "Log.h"
#include "Perf.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>

#include "CoreGuard.h"

namespace fs = std::filesystem;

namespace {

std::vector<float> Numbers(const CfgClass& c, std::string_view entry)
{
    std::vector<float> v;
    const CfgEntry* e = c.Find(entry);
    if (!e) return v;
    for (const auto& i : e->value.items)
    {
        double d = 0;
        if (i.type == CfgValue::Type::Number) d = i.num;
        else EvalNumber(i.str, d);
        v.push_back(float(d));
    }
    return v;
}

glm::vec4 Color(const CfgClass& c, std::string_view entry, glm::vec4 def)
{
    auto v = Numbers(c, entry);
    for (size_t i = 0; i < v.size() && i < 4; ++i) def[int(i)] = v[i];
    return def;
}

} // namespace

bool LoadRvMaterial(const fs::path& path, const fs::path& root, RvMaterial& out)
{
    out = {};
    out.path = path;
    ArmaConfig cfg;
    out.rapified = IsRapified(path);
    {
        std::error_code ec;
        out.fileTime = fs::last_write_time(path, ec).time_since_epoch().count();
    }
    if (!cfg.Load(path, root))
    {
        out.error = "cannot open";
        return false;
    }
    const CfgClass& r = cfg.Root();
    out.includes = cfg.Files().size() > 1;
    // source lines of the entries written in this file (for saving edits in place)
    if (!out.rapified)
    {
        std::error_code ec;
        std::function<void(const CfgClass&, const std::string&)> lines = [&](const CfgClass& c, const std::string& prefix) {
            for (const auto& e : c.entries)
                if (fs::equivalent(cfg.FileOf(e.file), path, ec)) out.entryLines[prefix + ToLower(e.name)] = e.line;
            for (const auto& k : c.classes)
            {
                if (k->external || !fs::equivalent(cfg.FileOf(k->file), path, ec)) continue;
                std::string p = prefix + ToLower(k->name);
                out.classLines[p] = k->line;
                lines(*k, p + ".");
            }
        };
        lines(r, "");
    }
    out.ambient = Color(r, "ambient", glm::vec4(1));
    out.diffuse = Color(r, "diffuse", glm::vec4(1));
    out.forcedDiffuse = Color(r, "forcedDiffuse", glm::vec4(0));
    out.emissive = Color(r, "emmisive", glm::vec4(0)); // sic, Arma spelling
    out.specular = Color(r, "specular", glm::vec4(0));
    out.specularPower = float(r.GetNumber("specularPower", 1.0));
    out.pixelShader = r.GetString("PixelShaderID", "Normal");
    out.vertexShader = r.GetString("VertexShaderID", "Basic");
    const bool super = ToLower(out.pixelShader).starts_with("super");

    for (const auto& k : r.classes)
    {
        std::string ln = ToLower(k->name);
        if (!ln.starts_with("stage") || ln.size() <= 5 || !std::isdigit(static_cast<unsigned char>(ln[5]))) continue;
        RvStage st;
        st.index = std::atoi(ln.c_str() + 5);
        st.texture = k->GetString("texture");
        // UV: either in the stage or in class TexGen<texGen>
        const CfgClass* uvc = k.get();
        if (std::string tg = k->GetString("texGen"); !tg.empty())
            if (const CfgClass* g = r.FindClass("TexGen" + tg)) uvc = g;
        st.uvClass = ToLower(uvc->name);
        st.uvSourceName = uvc->GetString("uvSource", "tex");
        std::string src = ToLower(st.uvSourceName);
        st.uv.source = src == "tex1" ? 1 : 0;
        if (src != "tex" && src != "tex1" && src != "none")
            Log::Get().Debug("{}: Stage{} uvSource \"{}\" approximated as tex", PathToUtf8(path.filename()), st.index, st.uvSourceName);
        if (const CfgClass* t = uvc->FindClass("uvTransform"))
        {
            auto aside = Numbers(*t, "aside"), up = Numbers(*t, "up"), pos = Numbers(*t, "pos");
            st.asideRaw = aside;
            st.upRaw = up;
            st.dirRaw = Numbers(*t, "dir");
            st.posRaw = pos;
            if (aside.size() >= 2) st.uv.aside = { aside[0], aside[1] };
            if (up.size() >= 2) st.uv.up = { up[0], up[1] };
            if (pos.size() >= 2) st.uv.pos = { pos[0], pos[1] };
        }
        st.role = src == "none" ? TexRole::Count : RoleFromTextureName(st.texture); // uvSource none: env map / fresnel
        {
            const std::string lt = ToLower(st.texture);
            if (size_t f = lt.find("fresnel("); f != std::string::npos && lt.starts_with("#(ai"))
            {
                float n = 1.5f, kk = 0.0f;
                if (sscanf_s(lt.c_str() + f + 8, "%f,%f", &n, &kk) >= 1)
                {
                    out.hasFresnel = true;
                    out.fresnelN = n;
                    out.fresnelK = kk;
                }
            }
            std::string stem = PathToUtf8(ArmaStringPath(lt).stem());
            if (IEquals(k->GetString("useWorldEnvMap"), "true") || stem.starts_with("env_") || stem.find("_env") != std::string::npos)
                out.hasEnvMap = true;
        }
        if (st.role == TexRole::Count && super && src != "none")
        {
            static const TexRole bySuperIndex[] = { TexRole::Diffuse, TexRole::Normal, TexRole::Detail, TexRole::Macro, TexRole::AmbientShadow, TexRole::Specular };
            if (st.index >= 1 && st.index <= 5) st.role = bySuperIndex[st.index];
        }
        out.stages.push_back(std::move(st));
    }
    std::sort(out.stages.begin(), out.stages.end(), [](auto& a, auto& b) { return a.index < b.index; });
    out.errors = cfg.Errors();
    out.loaded = true;
    return true;
}

// ============================================================================================ RVMAT save

namespace {

std::string Num(float v)
{
    std::string s = std::format("{:.6g}", v);
    return s == "-0" ? "0" : s;
}

std::string ColorText(const glm::vec4& c) { return std::format("{{{},{},{},{}}}", Num(c.r), Num(c.g), Num(c.b), Num(c.a)); }

std::string ArrayText(std::vector<float> raw, float a, float b)
{
    if (raw.size() < 3) raw.resize(3, 0.0f);
    raw[0] = a;
    raw[1] = b;
    std::string s = "{";
    for (size_t i = 0; i < raw.size(); ++i) s += (i ? "," : "") + Num(raw[i]);
    return s + "}";
}

// Replaces the value of `name` (= value; / []= {...};) starting at line `line` (1-based). False if not found there.
bool PatchValue(std::vector<std::string>& lines, int line, const std::string& name, const std::string& value)
{
    if (line < 1 || line > int(lines.size())) return false;
    std::string& l = lines[size_t(line - 1)];
    std::string low = ToLower(l);
    size_t at = low.find(ToLower(name));
    while (at != std::string::npos)
    {
        size_t p = at + name.size();
        bool boundary = (at == 0 || !(isalnum((unsigned char)low[at - 1]) || low[at - 1] == '_'));
        size_t q = p;
        while (q < l.size() && (l[q] == ' ' || l[q] == '\t')) ++q;
        if (boundary && q + 1 < l.size() && l[q] == '[' && l[q + 1] == ']') { q += 2; while (q < l.size() && (l[q] == ' ' || l[q] == '\t')) ++q; }
        if (boundary && q < l.size() && l[q] == '=')
        {
            size_t v = q + 1;
            while (v < l.size() && (l[v] == ' ' || l[v] == '\t')) ++v;
            // value ends at the ';' outside quotes / braces (single-line values only)
            int depth = 0;
            bool str = false;
            size_t e = v;
            for (; e < l.size(); ++e)
            {
                char ch = l[e];
                if (ch == '"') str = !str;
                else if (!str && ch == '{') ++depth;
                else if (!str && ch == '}') --depth;
                else if (!str && depth == 0 && ch == ';') break;
            }
            if (e >= l.size()) return false; // multi-line value: not patched in place
            l = l.substr(0, v) + value + l.substr(e);
            return true;
        }
        at = low.find(ToLower(name), at + 1);
    }
    return false;
}

// Inserts "name=value;" before the closing brace of the class that starts at classLine (0 = top level: at the end
// of the top-level entries, i.e. before the first class).
void InsertEntry(std::vector<std::string>& lines, int classLine, const std::string& entry, const std::string& eol)
{
    if (classLine <= 0)
    {
        size_t i = 0;
        for (; i < lines.size(); ++i)
            if (ToLower(lines[i]).find("class ") != std::string::npos) break;
        lines.insert(lines.begin() + i, entry + eol);
        return;
    }
    int depth = 0;
    bool opened = false;
    for (size_t i = size_t(classLine - 1); i < lines.size(); ++i)
    {
        for (char ch : lines[i])
        {
            if (ch == '{') { ++depth; opened = true; }
            else if (ch == '}' && --depth == 0 && opened)
            {
                std::string indent;
                for (char c2 : lines[i]) { if (c2 == ' ' || c2 == '\t') indent += c2; else break; }
                lines.insert(lines.begin() + i, indent + "\t" + entry + eol);
                return;
            }
        }
    }
    lines.push_back(entry + eol);
}

} // namespace

bool SaveRvMaterial(const RvMaterial& o, const RvMaterial& e, std::string& error, int& changed)
{
    changed = 0;
    if (o.rapified) { error = "binary (rapified) RVMAT - read only"; return false; }
    std::ifstream in(o.path, std::ios::binary);
    if (!in) { error = "cannot read " + PathToUtf8(o.path); return false; }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines;
    for (size_t a = 0; a <= text.size();)
    {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) { if (a < text.size()) lines.push_back(text.substr(a)); break; }
        lines.push_back(text.substr(a, b + 1 - a));
        a = b + 1;
    }
    // strip the EOL for editing, put it back on write
    std::vector<std::string> ends(lines.size());
    for (size_t i = 0; i < lines.size(); ++i)
    {
        std::string& l = lines[i];
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) { ends[i].insert(ends[i].begin(), l.back()); l.pop_back(); }
    }
    std::vector<std::string> problems;
    // Collect changes first (line numbers refer to the original file), patch bottom-up, then insert missing ones.
    struct Change { std::string key, name, value; int classLine; bool block = false; };
    std::vector<Change> changes;
    auto put = [&](const std::string& key, const std::string& name, const std::string& value, const std::string& cls) {
        int cl = cls.empty() ? 0 : (o.classLines.count(cls) ? o.classLines.at(cls) : -1);
        changes.push_back({ key, name, value, cl });
    };
    auto col = [&](const char* n, const glm::vec4& a, const glm::vec4& b) {
        if (a != b) put(n, std::string(n) + "[]", ColorText(b), "");
    };
    col("ambient", o.ambient, e.ambient);
    col("diffuse", o.diffuse, e.diffuse);
    col("forceddiffuse", o.forcedDiffuse, e.forcedDiffuse);
    col("emmisive", o.emissive, e.emissive);
    col("specular", o.specular, e.specular);
    if (o.specularPower != e.specularPower) put("specularpower", "specularPower", Num(e.specularPower), "");
    if (o.pixelShader != e.pixelShader) put("pixelshaderid", "PixelShaderID", "\"" + e.pixelShader + "\"", "");
    if (o.vertexShader != e.vertexShader) put("vertexshaderid", "VertexShaderID", "\"" + e.vertexShader + "\"", "");
    for (size_t i = 0; i < e.stages.size() && i < o.stages.size(); ++i)
    {
        const RvStage& a = o.stages[i];
        const RvStage& b = e.stages[i];
        const std::string st = std::format("stage{}", a.index);
        if (a.texture != b.texture) put(st + ".texture", "texture", "\"" + b.texture + "\"", st);
        if (a.uvSourceName != b.uvSourceName) put(a.uvClass + ".uvsource", "uvSource", "\"" + b.uvSourceName + "\"", a.uvClass);
        const std::string uvt = a.uvClass + ".uvtransform";
        const bool uvChanged = a.uv.aside != b.uv.aside || a.uv.up != b.uv.up || a.uv.pos != b.uv.pos;
        if (uvChanged && !o.classLines.count(uvt) && o.classLines.count(a.uvClass))
        {
            // the stage has no class uvTransform yet: add the whole block
            std::string blk = "class uvTransform" + eol + "\t{" + eol;
            blk += "\t\taside[]=" + ArrayText(a.asideRaw, b.uv.aside.x, b.uv.aside.y) + ";" + eol;
            blk += "\t\tup[]=" + ArrayText(a.upRaw, b.uv.up.x, b.uv.up.y) + ";" + eol;
            blk += "\t\tdir[]=" + ArrayText(a.dirRaw, a.dirRaw.size() > 0 ? a.dirRaw[0] : 0.0f, a.dirRaw.size() > 1 ? a.dirRaw[1] : 0.0f) + ";" + eol;
            blk += "\t\tpos[]=" + ArrayText(a.posRaw, b.uv.pos.x, b.uv.pos.y) + ";" + eol + "\t};";
            changes.push_back({ uvt, "", blk, o.classLines.at(a.uvClass), true });
        }
        else
        {
            if (a.uv.aside != b.uv.aside) put(uvt + ".aside", "aside[]", ArrayText(a.asideRaw, b.uv.aside.x, b.uv.aside.y), uvt);
            if (a.uv.up != b.uv.up) put(uvt + ".up", "up[]", ArrayText(a.upRaw, b.uv.up.x, b.uv.up.y), uvt);
            if (a.uv.pos != b.uv.pos) put(uvt + ".pos", "pos[]", ArrayText(a.posRaw, b.uv.pos.x, b.uv.pos.y), uvt);
        }
    }
    std::vector<const Change*> missing;
    std::vector<const Change*> ordered;
    for (const auto& ch : changes) ordered.push_back(&ch);
    for (const Change* ch : ordered)
    {
        if (ch->block) { missing.push_back(ch); continue; }
        auto it = o.entryLines.find(ch->key);
        std::string plain = ch->name;
        if (plain.ends_with("[]")) plain.resize(plain.size() - 2);
        if (it != o.entryLines.end())
        {
            if (!PatchValue(lines, it->second, plain, ch->value)) problems.push_back(std::format("{} (line {}): value not on one line - not changed", ch->key, it->second));
            else ++changed;
        }
        else if (ch->classLine < 0) problems.push_back(std::format("{}: class not in this file - not added", ch->key));
        else missing.push_back(ch);
    }
    // insert from the bottom so earlier class lines stay valid
    std::sort(missing.begin(), missing.end(), [](const Change* x, const Change* y) { return x->classLine > y->classLine; });
    for (const Change* ch : missing)
    {
        // fresh lines: ends[] gets an entry for the inserted line
        size_t before = lines.size();
        std::vector<std::string> tmp = lines;
        InsertEntry(tmp, ch->classLine, ch->block ? ch->value : ch->name + "=" + ch->value + ";", "");
        // find inserted index (first difference)
        size_t idx = 0;
        while (idx < before && tmp[idx] == lines[idx]) ++idx;
        lines = std::move(tmp);
        ends.insert(ends.begin() + idx, eol);
        ++changed;
    }
    if (!problems.empty())
    {
        error.clear();
        for (const auto& p : problems) error += p + "; ";
    }
    if (changed == 0) return problems.empty();
    // backup, then write
    std::error_code ec;
    fs::path bak = o.path;
    bak += ".bak";
    fs::copy_file(o.path, bak, fs::copy_options::overwrite_existing, ec);
    if (ec) { error = "cannot write the backup " + PathToUtf8(bak) + ": " + ec.message(); return false; }
    std::ofstream out(o.path, std::ios::binary | std::ios::trunc);
    for (size_t i = 0; i < lines.size(); ++i) out << lines[i] << ends[i];
    if (!out) { error = "cannot write " + PathToUtf8(o.path); return false; }
    return problems.empty();
}

// ============================================================================================ config skins

std::string NormalizeArmaPath(std::string_view p)
{
    std::string s = ToLower(p);
    std::replace(s.begin(), s.end(), '/', '\\');
    while (!s.empty() && s.front() == '\\') s.erase(s.begin());
    size_t slash = s.rfind('\\');
    size_t dot = s.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) s += ".p3d";
    return s;
}

std::vector<ConfigSkin> FindConfigSkins(const ArmaConfig& cfg, const std::string& modelPath)
{
    std::vector<ConfigSkin> out;
    const std::string want = NormalizeArmaPath(modelPath);
    for (const auto& root : cfg.Root().classes)
    {
        if (root->external || !ToLower(root->name).starts_with("cfg")) continue;
        for (const auto& k : root->classes)
        {
            if (k->external) continue;
            std::string model = k->GetString("model");
            if (model.empty() || NormalizeArmaPath(model) != want) continue;
            ConfigSkin s;
            s.cfgRoot = root->name;
            s.className = k->name;
            s.selections = k->GetStrings("hiddenSelections");
            s.textures = k->GetStrings("hiddenSelectionsTextures");
            s.materials = k->GetStrings("hiddenSelectionsMaterials");
            s.where = cfg.Where(k->file, k->line);
            s.file = cfg.FileOf(k->file);
            s.line = k->line;
            // "" entries count: all textures "" is a re-texture that hides those selections
            const bool arrays = k->Find("hiddenSelectionsTextures") || k->Find("hiddenSelectionsMaterials");
            if (arrays && !s.selections.empty()) out.push_back(std::move(s));
        }
    }
    return out;
}

bool SkinFace(const std::vector<std::string>& textures, const std::vector<std::string>& materials, int i, std::string& texture, std::string& material)
{
    if (i < 0) return true;
    if (i < int(textures.size()))
    {
        if (textures[size_t(i)].empty()) return false; // "" hides the selection
        texture = textures[size_t(i)];
    }
    if (i < int(materials.size()) && !materials[size_t(i)].empty()) material = materials[size_t(i)];
    return true;
}

const CfgClass* FindVehicleClass(const ArmaConfig& cfg, const std::string& modelPath)
{
    const std::string want = NormalizeArmaPath(modelPath);
    const CfgClass* found = nullptr;
    for (const auto& root : cfg.Root().classes)
    {
        if (!IEquals(root->name, "CfgVehicles")) continue;
        for (const auto& k : root->classes)
        {
            if (k->external) continue;
            std::string model = k->GetString("model");
            if (model.empty() || NormalizeArmaPath(model) != want) continue;
            bool own = k->Find("model", false) != nullptr;
            if (!found || (own && !found->Find("model", false))) found = k.get();
        }
    }
    return found;
}
