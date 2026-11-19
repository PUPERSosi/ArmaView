#include "ModCheck.h"
#include "ArmaConfig.h"
#include "Log.h"
#include "RvMaterial.h"
#include "Moves.h"
#include "P3D.h"
#include "RTM.h"
#include "Skeleton.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include "CoreGuard.h"

namespace fs = std::filesystem;

namespace {

std::string Lower(std::string s)
{
    for (auto& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

// "P:\mod\config.cpp(123): text" (cfg.Where) -> file, line, rest
bool SplitWhere(const std::string& text, fs::path& file, int& line, std::string& rest)
{
    static const std::regex re(R"(^(.+?\.(?:cpp|hpp|cfg|h|rvmat|inc|sqm))\((\d+)\)[: ]*(.*)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(text, m, re)) return false;
    file = fs::path(Utf8ToWide(m[1].str()));
    line = std::stoi(m[2].str());
    rest = m[3].str();
    return true;
}

struct Checker
{
    fs::path root;
    std::shared_ptr<const ArmaConfig> game;
    std::vector<ModIssue> out;
    std::map<std::wstring, std::shared_ptr<ArmaConfig>> modelCfgs; // model.cfg already read
    std::set<std::string> animSeen;                                // model.cfg animation problems already listed

    void Add(ModIssue::Sev sev, const char* kind, const fs::path& file, int line, std::string text)
    {
        ModIssue i;
        i.sev = sev;
        i.kind = kind;
        i.file = file;
        i.line = line;
        i.text = std::move(text);
        out.push_back(std::move(i));
    }
    // log lines of the parsers while checking `file`: warnings / errors become issues (with their own file:line if given)
    void TakeLog(std::vector<LogEntry>& captured, const char* kind, const fs::path& file)
    {
        for (auto& e : captured)
        {
            if (e.level != LogLevel::Warn && e.level != LogLevel::Error) continue;
            fs::path f = file;
            int line = 0;
            std::string rest = e.text;
            SplitWhere(e.text, f, line, rest);
            Add(e.level == LogLevel::Error ? ModIssue::Sev::Error : ModIssue::Sev::Warning, kind, f, line, rest);
        }
        captured.clear();
    }
    bool Exists(const std::string& armaPath, const char* defaultExt, fs::path* resolved = nullptr)
    {
        fs::path p = ResolveArmaPath(armaPath, root);
        if (!p.has_extension() && defaultExt) p += defaultExt;
        if (resolved) *resolved = p;
        std::error_code ec;
        return fs::exists(p, ec);
    }
    static bool Procedural(const std::string& s) { return s.empty() || s[0] == '#'; }

    std::shared_ptr<ArmaConfig> ModelCfg(const fs::path& p3d)
    {
        std::error_code ec;
        for (fs::path dir = p3d.parent_path(); !dir.empty(); dir = dir.parent_path())
        {
            const fs::path cfg = dir / "model.cfg";
            if (fs::exists(cfg, ec))
            {
                auto& slot = modelCfgs[cfg.native()];
                if (!slot)
                {
                    std::vector<LogEntry> cap;
                    Log::CaptureScope scope(cap);
                    slot = std::make_shared<ArmaConfig>();
                    slot->Load(cfg, root);
                    TakeLog(cap, "model.cfg", cfg);
                }
                return slot;
            }
            if (dir == dir.root_path() || fs::equivalent(dir, root, ec)) break;
        }
        return nullptr;
    }

    void P3d(const fs::path& file)
    {
        {
            char sig[4] = {};
            std::ifstream(file, std::ios::binary).read(sig, 4);
            if (std::string_view(sig, 4) != "MLOD") { Add(ModIssue::Sev::Warning, "p3d", file, 0, kBinarizedNotSupported); return; }
        }
        std::vector<LogEntry> cap;
        P3DModel m;
        bool ok;
        {
            Log::CaptureScope scope(cap);
            ok = LoadP3D(file, m);
        }
        TakeLog(cap, "p3d", file);
        if (!ok || m.lods.empty()) { Add(ModIssue::Sev::Error, "p3d", file, 0, "cannot be read"); return; }
        if (!m.complete) Add(ModIssue::Sev::Error, "p3d", file, 0, "truncated: not every LOD of the header was read");
        // textures and materials of the faces (first LOD and every resolution LOD)
        std::map<std::string, int> tex, mat;
        for (const auto& l : m.lods)
            for (const auto& f : l.faces)
            {
                if (!Procedural(f.texture)) ++tex[Lower(f.texture)];
                if (!f.material.empty()) ++mat[Lower(f.material)];
            }
        for (const auto& [t, n] : tex)
        {
            const bool other = t.ends_with(".tga") || t.ends_with(".png"); // binarize turns these into .paa
            if (!Exists(t, nullptr) && !(other && Exists(t.substr(0, t.size() - 4) + ".paa", nullptr)))
                Add(ModIssue::Sev::Error, "p3d", file, 0, std::format("texture not found: {} ({} faces)", t, n));
        }
        for (const auto& [t, n] : mat)
            if (!Exists(t, nullptr)) Add(ModIssue::Sev::Error, "p3d", file, 0, std::format("material not found: {} ({} faces)", t, n));
        // proxies that point nowhere
        std::set<std::string> proxies;
        for (const auto& s : m.lods.front().selections)
            if (s.name.starts_with("proxy:"))
            {
                std::string p = s.name.substr(6);
                if (size_t dot = p.find_last_of('.'); dot != std::string::npos && dot + 1 < p.size() && std::isdigit(uint8_t(p[dot + 1]))) p.resize(dot);
                proxies.insert(Lower(p));
            }
        for (const auto& p : proxies)
            if (!Exists(p, ".p3d")) Add(ModIssue::Sev::Warning, "p3d", file, 0, std::format("proxy model not found: {}", p));
        // the rig: skeleton of its model.cfg class, selection weights, model.cfg animations and their axes
        auto cfg = ModelCfg(file);
        if (!cfg) return;
        Skeleton sk;
        ModelCfgInfo info;
        std::vector<LogEntry> cap2;
        {
            Log::CaptureScope scope(cap2);
            if (!BuildSkeleton(*cfg, PathToUtf8(file.stem()), sk, info)) return;
        }
        TakeLog(cap2, "model.cfg", file);
        const P3DLod& l0 = m.lods.front();
        const ValidationReport v = Validate(sk, l0, info.sections);
        auto count = [&](size_t n, const char* what, ModIssue::Sev sev) {
            if (n) Add(sev, "p3d", file, 0, std::format("LOD {}: {} {}", LodResolutionName(l0.resolution), n, what));
        };
        count(v.weightedNotInSkeleton.size(), "weighted selection(s) not in the skeleton", ModIssue::Sev::Warning);
        count(v.tooManyBones.size(), "point(s) with more than 4 bones", ModIssue::Sev::Warning);
        count(v.badWeightSum.size(), "point(s) whose weights do not sum to 1", ModIssue::Sev::Warning);
        count(v.noBone.size(), "point(s) without a bone", ModIssue::Sev::Warning);
        std::vector<CfgAnimation> anims;
        std::vector<CfgSource> sources;
        {
            Log::CaptureScope scope(cap2);
            BuildCfgAnimations(*cfg, info, sk, anims, sources);
            ResolveAnimationAxes(anims, m, l0, false);
        }
        TakeLog(cap2, "model.cfg", file);
        for (const auto& a : anims)
        {
            const fs::path where = cfg->FileOf(a.file);
            if (!animSeen.insert(std::format("{}|{}|{}|{}", PathToUtf8(where), a.line, a.problem, a.axisProblem)).second) continue;
            if (!a.problem.empty()) Add(a.problemIsWarning ? ModIssue::Sev::Warning : ModIssue::Sev::Error, "model.cfg", where, a.line, std::format("{} (first in {}): {}", a.name, PathToUtf8(file.filename()), a.problem));
            else if (!a.axisProblem.empty()) Add(ModIssue::Sev::Error, "model.cfg", where, a.line, std::format("{} (first in {}): {}", a.name, PathToUtf8(file.filename()), a.axisProblem));
        }
    }

    void ConfigCpp(const fs::path& file)
    {
        std::vector<LogEntry> cap;
        ArmaConfig cfg;
        cfg.SetFallback(game);
        {
            Log::CaptureScope scope(cap);
            cfg.Load(file, root, false);
        }
        TakeLog(cap, "config.cpp", file);
        if (cfg.Errors()) Add(ModIssue::Sev::Error, "config.cpp", file, 0, std::format("{} parse error(s)", cfg.Errors()));
        if (game && cfg.MissingBases()) Add(ModIssue::Sev::Warning, "config.cpp", file, 0, std::format("{} base class(es) found neither here nor in the game configs", cfg.MissingBases()));
        // file references of the classes this config defines
        static const char* const kRoots[] = { "CfgVehicles", "CfgWeapons", "CfgMagazines", "CfgAmmo", "CfgNonAIVehicles" };
        static const char* const kModel[] = { "model", "modelSpecial", "modelOptics" };
        static const char* const kTex[] = { "picture", "icon", "UiPicture", "editorPreview" };
        for (const char* rc : kRoots)
        {
            const CfgClass* r = nullptr;
            for (const auto& k : cfg.Root().classes)
                if (_stricmp(k->name.c_str(), rc) == 0) r = k.get();
            if (!r) continue;
            for (const auto& own : r->classes) // classes written in this config (not the game's merged in)
            {
                const CfgClass* c = own.get();
                if (c->external || cfg.FileOf(c->file).empty()) continue;
                auto check = [&](const char* entry, const char* ext, bool array) {
                    const CfgEntry* e = c->Find(entry, false);
                    if (!e) return;
                    std::vector<std::string> vals;
                    if (array) { for (const auto& it : e->value.items) if (it.type == CfgValue::Type::String) vals.push_back(it.str); }
                    else if (e->value.type == CfgValue::Type::String) vals.push_back(e->value.str);
                    for (const auto& v : vals)
                    {
                        if (Procedural(v) || v.starts_with("\\a3\\") || v.starts_with("a3\\") || v == "-") continue; // the game's own files are packed
                        if (!Exists(v, ext))
                            Add(ModIssue::Sev::Error, "config.cpp", cfg.FileOf(e->file), e->line, std::format("{} >> {}: file not found: {}", c->name, entry, v));
                    }
                };
                for (const char* k : kModel) check(k, ".p3d", false);
                for (const char* k : kTex) check(k, ".paa", false);
                check("hiddenSelectionsTextures", nullptr, true);
                check("hiddenSelectionsMaterials", nullptr, true);
            }
        }
        // move configs: missing RTMs and links to states that do not exist
        for (const auto& name : ListMovesClasses(cfg))
        {
            const CfgClass* mc = nullptr;
            for (const auto& k : cfg.Root().classes)
                if (_stricmp(k->name.c_str(), name.c_str()) == 0 && !k->external) mc = k.get();
            if (!mc || cfg.FileOf(mc->file).empty()) continue;
            MovesGraph g;
            {
                Log::CaptureScope scope(cap);
                if (!g.Build(cfg, name, root)) continue;
            }
            TakeLog(cap, "config.cpp", file);
            for (const auto& s : g.states)
            {
                const fs::path where = cfg.FileOf(s.cfgFile);
                if (where.empty()) continue; // inherited from the game
                if (!s.abstract && !s.fileExists && !s.file.empty())
                    Add(ModIssue::Sev::Error, "config.cpp", where, s.cfgLine, std::format("{} >> {}: RTM not found: {}", name, s.name, s.file));
                for (const auto& b : s.badLinks) Add(ModIssue::Sev::Error, "config.cpp", where, s.cfgLine, std::format("{} >> {}: {} is not a state", name, s.name, b));
            }
            for (const auto& b : g.badActions)
                if (b.find("inherited / shared") == std::string::npos) // the game's shared action lists: not the mod's
                    Add(ModIssue::Sev::Warning, "config.cpp", file, 0, std::format("{}: {}", name, b));
        }
    }

    void Rvmat(const fs::path& file)
    {
        std::vector<LogEntry> cap;
        RvMaterial m;
        bool ok;
        {
            Log::CaptureScope scope(cap);
            ok = LoadRvMaterial(file, root, m);
        }
        TakeLog(cap, "rvmat", file);
        if (!ok) { Add(ModIssue::Sev::Error, "rvmat", file, 0, "cannot be read"); return; }
        for (const auto& s : m.stages)
            if (!Procedural(s.texture) && !Exists(s.texture, nullptr))
            {
                auto it = m.entryLines.find(std::format("stage{}.texture", s.index));
                Add(ModIssue::Sev::Error, "rvmat", file, it != m.entryLines.end() ? it->second : 0, std::format("Stage{}: texture not found: {}", s.index, s.texture));
            }
    }

    void Rtm(const fs::path& file)
    {
        RtmAnimation a;
        std::string err;
        std::vector<std::string> warn;
        std::vector<LogEntry> cap;
        bool ok;
        {
            Log::CaptureScope scope(cap);
            ok = LoadRtm(file, a, err, warn);
        }
        TakeLog(cap, "rtm", file);
        if (!ok) { Add(ModIssue::Sev::Error, "rtm", file, 0, err.empty() ? std::string("cannot be read") : err); return; }
        for (const auto& w : warn) Add(ModIssue::Sev::Warning, "rtm", file, 0, w);
        if (a.Frames() == 0) Add(ModIssue::Sev::Error, "rtm", file, 0, "no frames");
    }
};

std::string NowText()
{
    const auto t = std::chrono::system_clock::now();
    return std::format("{:%Y-%m-%d %H:%M:%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::current_zone()->to_local(t)));
}

std::string Html(const std::string& s)
{
    std::string o;
    for (char c : s)
        switch (c)
        {
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '&': o += "&amp;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    return o;
}

} // namespace

std::string ModIssue::Key() const
{
    // numbers change with every edit (line numbers, counts): the identity is file + kind + the text without digits
    std::string t;
    for (char c : text)
        if (!std::isdigit(uint8_t(c))) t += c;
    return Lower(PathToUtf8(file)) + "|" + kind + "|" + t;
}

int ModReport::Count(ModIssue::Sev s) const
{
    return int(std::count_if(issues.begin(), issues.end(), [&](const ModIssue& i) { return i.sev == s; }));
}

fs::path ModCheckDir()
{
#pragma warning(suppress : 4996) // _wgetenv: a read-only lookup
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    std::error_code ec;
    return (local ? fs::path(local) : fs::temp_directory_path(ec)) / "ArmaView" / "modcheck";
}

ModReport RunModCheck(const fs::path& folder, const fs::path& projectRoot, std::shared_ptr<const ArmaConfig> game, ModCheckProgress& progress)
{
    const auto t0 = std::chrono::steady_clock::now();
    ModReport r;
    r.folder = folder;
    r.when = NowText();
    Checker c;
    c.root = projectRoot.empty() ? ArmaProjectRoot(folder) : projectRoot;
    c.game = std::move(game);
    std::vector<std::pair<fs::path, std::string>> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = Lower(PathToUtf8(it->path().extension())), name = Lower(PathToUtf8(it->path().filename()));
        if (ext == ".p3d") files.emplace_back(it->path(), "p3d");
        else if (name == "config.cpp") files.emplace_back(it->path(), "config.cpp");
        else if (name == "model.cfg") files.emplace_back(it->path(), "model.cfg");
        else if (ext == ".rvmat") files.emplace_back(it->path(), "rvmat");
        else if (ext == ".rtm") files.emplace_back(it->path(), "rtm");
    }
    progress.total = int(files.size());
    // model.cfg first: their own parse problems once, then reused by the P3Ds
    std::stable_sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return (a.second == "model.cfg") > (b.second == "model.cfg"); });
    for (const auto& [f, kind] : files)
    {
        if (progress.cancel) { r.cancelled = true; break; }
        ++r.files[kind];
        try
        {
            if (kind == "p3d") c.P3d(f);
            else if (kind == "config.cpp") c.ConfigCpp(f);
            else if (kind == "model.cfg") c.ModelCfg(f.parent_path() / "_.p3d"); // its own parse problems
            else if (kind == "rvmat") c.Rvmat(f);
            else if (kind == "rtm") c.Rtm(f);
        }
        catch (const std::exception& e)
        {
            c.Add(ModIssue::Sev::Error, kind.c_str(), f, 0, std::string("check failed: ") + e.what());
        }
        ++progress.done;
    }
    // one issue per (file, line, text)
    std::set<std::string> seen;
    for (auto& i : c.out)
        if (seen.insert(std::format("{}|{}|{}", PathToUtf8(i.file), i.line, i.text)).second) r.issues.push_back(std::move(i));
    std::stable_sort(r.issues.begin(), r.issues.end(), [](const ModIssue& a, const ModIssue& b) {
        return a.file != b.file ? a.file < b.file : a.sev != b.sev ? a.sev < b.sev : a.line < b.line;
    });
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

void FinishModReport(ModReport& r)
{
    std::error_code ec;
    const fs::path dir = ModCheckDir();
    fs::create_directories(dir, ec);
    // previous run of this folder: issues by key
    const std::string id = std::format("{:016x}", std::hash<std::string>{}(Lower(PathToUtf8(fs::absolute(r.folder, ec)))));
    const fs::path last = dir / (id + ".last.tsv");
    std::map<std::string, ModIssue> before;
    if (std::ifstream in(last); in)
    {
        std::string line;
        if (std::getline(in, line)) r.previousWhen = line;
        while (std::getline(in, line))
        {
            std::vector<std::string> f;
            std::stringstream ss(line);
            for (std::string x; std::getline(ss, x, '\t');) f.push_back(x);
            if (f.size() < 5) continue;
            ModIssue i;
            i.sev = f[0] == "E" ? ModIssue::Sev::Error : ModIssue::Sev::Warning;
            i.kind = f[1];
            i.file = fs::path(Utf8ToWide(f[2]));
            i.line = std::atoi(f[3].c_str());
            i.text = f[4];
            before.emplace(i.Key(), i);
        }
        r.hadPrevious = true;
    }
    std::set<std::string> now;
    for (auto& i : r.issues)
    {
        now.insert(i.Key());
        i.isNew = r.hadPrevious && !before.count(i.Key());
    }
    for (const auto& [k, i] : before)
        if (!now.count(k)) r.fixed.push_back(i);
    if (!r.cancelled)
        if (std::ofstream out(last, std::ios::trunc); out)
        {
            out << r.when << '\n';
            for (const auto& i : r.issues)
                out << (i.sev == ModIssue::Sev::Error ? "E" : "W") << '\t' << i.kind << '\t' << PathToUtf8(i.file) << '\t' << i.line << '\t' << i.text << '\n';
        }
    // HTML: one table, filters by text / severity / kind / new, grouped by file
    std::string stem = PathToUtf8(r.folder.filename());
    for (auto& ch : stem)
        if (!std::isalnum(uint8_t(ch)) && ch != '_' && ch != '-') ch = '_';
    std::string stamp = r.when;
    std::replace(stamp.begin(), stamp.end(), ':', '-');
    std::replace(stamp.begin(), stamp.end(), ' ', '_');
    r.html = dir / std::format("{}_{}.html", stem, stamp);
    std::ofstream h(r.html, std::ios::trunc);
    if (!h) return;
    h << "<!doctype html><html><head><meta charset=\"utf-8\"><title>Mod check " << Html(stem) << "</title><style>"
         "body{font:14px Segoe UI,sans-serif;margin:16px;background:#1d1f23;color:#ddd}table{border-collapse:collapse;width:100%}"
         "td,th{padding:3px 8px;border-bottom:1px solid #333;text-align:left;vertical-align:top}th{position:sticky;top:0;background:#2a2d33}"
         ".E{color:#ff7070}.W{color:#ffcc55}.new{background:#3b3420}.file{color:#9cc8ff;font-weight:600;padding-top:10px}"
         "input,select,label{margin-right:12px}a{color:#9cc8ff}.fixed{color:#7fd17f}</style></head><body>";
    h << "<h2>Mod check: " << Html(PathToUtf8(r.folder)) << "</h2><p>" << Html(r.when) << " &middot; " << std::format("{:.1f}", r.seconds) << " s &middot; ";
    for (const auto& [k, n] : r.files) h << Html(k) << " " << n << " &middot; ";
    h << "<b class=E>" << r.Count(ModIssue::Sev::Error) << " error(s)</b>, <b class=W>" << r.Count(ModIssue::Sev::Warning) << " warning(s)</b>";
    if (r.hadPrevious)
        h << " &middot; against " << Html(r.previousWhen) << ": " << std::count_if(r.issues.begin(), r.issues.end(), [](const ModIssue& i) { return i.isNew; })
          << " new, <span class=fixed>" << r.fixed.size() << " fixed</span>";
    if (r.cancelled) h << " &middot; <b class=E>cancelled</b>";
    h << "</p><p><input id=q placeholder=\"filter\" oninput=f()><label><input type=checkbox id=e checked onchange=f()>errors</label>"
         "<label><input type=checkbox id=w checked onchange=f()>warnings</label><label><input type=checkbox id=n onchange=f()>only new</label>"
         "<select id=k onchange=f()><option value=\"\">all kinds</option><option>p3d</option><option>model.cfg</option><option>config.cpp</option>"
         "<option>rvmat</option><option>rtm</option></select></p><table id=t><tr><th></th><th>kind</th><th>line</th><th>problem</th></tr>";
    fs::path cur;
    for (const auto& i : r.issues)
    {
        if (i.file != cur)
        {
            cur = i.file;
            h << "<tr class=fh><td colspan=4 class=file><a href=\"file:///" << Html(PathToUtf8(i.file)) << "\">" << Html(PathToUtf8(i.file)) << "</a></td></tr>";
        }
        const char* s = i.sev == ModIssue::Sev::Error ? "E" : "W";
        h << "<tr class=\"r " << s << (i.isNew ? " new" : "") << "\" data-k=\"" << Html(i.kind) << "\"><td class=" << s << ">" << (i.sev == ModIssue::Sev::Error ? "error" : "warning")
          << (i.isNew ? " (new)" : "") << "</td><td>" << Html(i.kind) << "</td><td>" << (i.line ? std::to_string(i.line) : "") << "</td><td>" << Html(i.text) << "</td></tr>";
    }
    h << "</table>";
    if (!r.fixed.empty())
    {
        h << "<h3 class=fixed>Fixed since " << Html(r.previousWhen) << "</h3><table>";
        for (const auto& i : r.fixed)
            h << "<tr><td>" << Html(PathToUtf8(i.file)) << (i.line ? "(" + std::to_string(i.line) + ")" : "") << "</td><td>" << Html(i.text) << "</td></tr>";
        h << "</table>";
    }
    h << "<script>function f(){const q=document.getElementById('q').value.toLowerCase(),e=document.getElementById('e').checked,w=document.getElementById('w').checked,"
         "n=document.getElementById('n').checked,k=document.getElementById('k').value;let fh=null,any=false;"
         "for(const r of document.querySelectorAll('#t tr')){if(r.classList.contains('fh')){if(fh)fh.style.display=any?'':'none';fh=r;any=false;continue;}"
         "if(!r.classList.contains('r'))continue;const ok=(r.classList.contains('E')?e:w)&&(!n||r.classList.contains('new'))&&(!k||r.dataset.k===k)&&"
         "(!q||r.textContent.toLowerCase().includes(q)||(fh&&fh.textContent.toLowerCase().includes(q)));r.style.display=ok?'':'none';any=any||ok;}"
         "if(fh)fh.style.display=any?'':'none';}</script></body></html>";
}

void MountModPrefix(const fs::path& dir, const fs::path& root)
{
    // the same inference as the viewer's model (InferModPrefix): the mod's config.cpp read quietly for its own paths
    std::error_code ec;
    ArmaConfig cfg;
    const bool hasCfg = fs::exists(dir / "config.cpp", ec);
    if (hasCfg)
    {
        LogCapture quiet; // its problems are the Mod check's report, not the log's
        cfg.Load(dir / "config.cpp", root, false);
    }
    MountInferredModPrefix(dir, hasCfg ? &cfg : nullptr, root);
}
