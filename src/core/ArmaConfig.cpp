#include "ArmaConfig.h"
#include "Log.h"
#include "Perf.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>

#include "CoreGuard.h"

namespace fs = std::filesystem;

// ============================================================================================ CfgClass

std::vector<const CfgClass*> CfgClass::Chain() const
{
    std::vector<const CfgClass*> out;
    // own base chain first, then each class's overlay (and its chain); bounded against cycles
    std::vector<const CfgClass*> todo{ this };
    while (!todo.empty() && out.size() < 256)
    {
        const CfgClass* start = todo.front();
        todo.erase(todo.begin());
        int guard = 0;
        for (const CfgClass* c = start; c && guard < 64; c = c->base, ++guard)
        {
            if (std::find(out.begin(), out.end(), c) != out.end()) break;
            out.push_back(c);
            if (c->overlay) todo.push_back(c->overlay);
        }
    }
    return out;
}

const CfgEntry* CfgClass::Find(std::string_view entry, bool inherited) const
{
    if (!inherited)
    {
        for (const auto& e : entries)
            if (IEquals(e.name, entry)) return &e;
        return nullptr;
    }
    // fast path without overlays
    int guard = 0;
    bool overlays = false;
    for (const CfgClass* c = this; c && guard < 64; c = c->base, ++guard)
    {
        for (const auto& e : c->entries)
            if (IEquals(e.name, entry)) return &e;
        overlays |= c->overlay != nullptr;
    }
    if (!overlays) return nullptr;
    for (const CfgClass* c : Chain())
        for (const auto& e : c->entries)
            if (IEquals(e.name, entry)) return &e;
    return nullptr;
}

const CfgClass* CfgClass::FindClass(std::string_view cls, bool inherited) const
{
    if (inherited)
    {
        bool overlays = false;
        int g = 0;
        for (const CfgClass* c = this; c && g < 64; c = c->base, ++g) overlays |= c->overlay != nullptr;
        if (overlays)
        {
            for (const CfgClass* c : Chain())
                for (const auto& k : c->classes)
                    if (IEquals(k->name, cls))
                    {
                        if (k->external) { if (k->base) return k->base; continue; }
                        return k.get();
                    }
            return nullptr;
        }
    }
    int guard = 0;
    for (const CfgClass* c = this; c && guard < 64; c = inherited ? c->base : nullptr, ++guard)
    {
        for (const auto& k : c->classes)
            if (IEquals(k->name, cls))
            {
                if (k->external) // forward declaration: real class lives in the inherited scope
                {
                    if (k->base) return k->base;
                    continue;
                }
                return k.get();
            }
        if (!inherited) break;
    }
    return nullptr;
}

std::string CfgClass::GetString(std::string_view entry, std::string_view def) const
{
    const CfgEntry* e = Find(entry);
    if (!e || e->isArray) return std::string(def);
    return e->value.str;
}

double CfgClass::GetNumber(std::string_view entry, double def) const
{
    const CfgEntry* e = Find(entry);
    if (!e || e->isArray) return def;
    if (e->value.type == CfgValue::Type::Number) return e->value.num;
    double v = def;
    return EvalNumber(e->value.str, v) ? v : def;
}

bool CfgClass::HasNumber(std::string_view entry) const
{
    const CfgEntry* e = Find(entry);
    double v;
    return e && !e->isArray && (e->value.type == CfgValue::Type::Number || EvalNumber(e->value.str, v));
}

namespace {
class ExprParser
{
public:
    explicit ExprParser(std::string_view s) : s_(s) {}
    bool Parse(double& out)
    {
        if (!Expr(out)) return false;
        Ws();
        return p_ == s_.size();
    }

private:
    void Ws() { while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_; }
    bool Expr(double& v)
    {
        if (!Term(v)) return false;
        for (;;)
        {
            Ws();
            if (p_ < s_.size() && (s_[p_] == '+' || s_[p_] == '-'))
            {
                char op = s_[p_++];
                double r;
                if (!Term(r)) return false;
                v = op == '+' ? v + r : v - r;
            }
            else return true;
        }
    }
    bool Term(double& v)
    {
        if (!Unary(v)) return false;
        for (;;)
        {
            Ws();
            if (p_ < s_.size() && (s_[p_] == '*' || s_[p_] == '/'))
            {
                char op = s_[p_++];
                double r;
                if (!Unary(r)) return false;
                if (op == '/' && r == 0.0) return false;
                v = op == '*' ? v * r : v / r;
            }
            else return true;
        }
    }
    bool Unary(double& v)
    {
        Ws();
        if (p_ < s_.size() && (s_[p_] == '-' || s_[p_] == '+'))
        {
            char op = s_[p_++];
            if (!Unary(v)) return false;
            if (op == '-') v = -v;
            return true;
        }
        size_t a = p_;
        while (p_ < s_.size() && std::isalpha(static_cast<unsigned char>(s_[p_]))) ++p_;
        std::string word = ToLower(s_.substr(a, p_ - a));
        if (word == "rad" || word == "deg")
        {
            if (!Unary(v)) return false;
            v = word == "rad" ? v * 3.14159265358979323846 / 180.0 : v * 180.0 / 3.14159265358979323846;
            return true;
        }
        if (word == "pi") { v = 3.14159265358979323846; return true; }
        if (!word.empty()) return false;
        if (p_ < s_.size() && s_[p_] == '(')
        {
            ++p_;
            if (!Expr(v)) return false;
            Ws();
            if (p_ >= s_.size() || s_[p_] != ')') return false;
            ++p_;
            return true;
        }
        const char* begin = s_.data() + p_;
        auto r = std::from_chars(begin, s_.data() + s_.size(), v);
        if (r.ec != std::errc() || r.ptr == begin) return false;
        p_ += size_t(r.ptr - begin);
        return true;
    }
    std::string_view s_;
    size_t p_ = 0;
};
} // namespace

bool EvalNumber(std::string_view expr, double& out)
{
    ExprParser p(expr);
    double v = 0;
    if (!p.Parse(v)) return false;
    out = v;
    return true;
}

std::vector<std::string> CfgClass::GetStrings(std::string_view entry) const
{
    std::vector<std::string> out;
    const CfgEntry* e = Find(entry);
    if (!e) return out;
    auto flatten = [&](auto&& self, const CfgValue& v) -> void {
        if (v.type == CfgValue::Type::Array)
            for (const auto& i : v.items) self(self, i);
        else
            out.push_back(v.str);
    };
    flatten(flatten, e->value);
    return out;
}

// ============================================================================================ ArmaConfig

int ArmaConfig::AddFile(const fs::path& p)
{
    files_.push_back(p);
    return fileBase_ + int(files_.size()) - 1;
}

fs::path ArmaConfig::FileOf(int file) const
{
    int i = file - fileBase_;
    if (i >= 0 && i < int(files_.size())) return files_[i];
    if (fallback_) return fallback_->FileOf(file);
    return {};
}

std::string ArmaConfig::Where(int file, int line) const
{
    fs::path p = FileOf(file);
    std::string f = p.empty() ? "?" : PathToUtf8(p);
    return std::format("{}({})", f, line);
}

void ArmaConfig::Error(int file, int line, const std::string& msg)
{
    ++errors_;
    if (!quiet_) Log::Get().Error("{}: {}", Where(file, line), msg);
}

void ArmaConfig::Warn(int file, int line, const std::string& msg)
{
    if (!quiet_) Log::Get().Warn("{}: {}", Where(file, line), msg);
}

namespace {
DriveTargetFn g_driveTarget = nullptr;
}

void SetDriveTargetResolver(DriveTargetFn fn) { g_driveTarget = fn; }

fs::path ArmaProjectRoot(const fs::path& anyPath)
{
    std::error_code ec;
    fs::path abs = fs::absolute(anyPath, ec);
    std::wstring lower = abs.native();
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (lower.starts_with(L"p:\\")) return L"P:\\";
    if (std::wstring t = g_driveTarget ? g_driveTarget(L'P') : std::wstring(); !t.empty()) // SUBST shows as "\??\C:\Work"
    {
        if (t.starts_with(L"\\??\\")) t = t.substr(4);
        std::wstring tl = t;
        std::transform(tl.begin(), tl.end(), tl.begin(), ::towlower);
        if (!tl.empty() && tl.back() != L'\\') tl += L'\\';
        if (lower.starts_with(tl)) return fs::path(t) / "";
    }
    return abs.root_path();
}

namespace {

std::string MountKey(std::string_view prefix)
{
    std::string p = ToLower(prefix);
    while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    while (!p.empty() && (p.front() == '\\' || p.front() == '/')) p.erase(p.begin());
    return p;
}

bool UnderPrefix(const std::string& lower, const std::string& prefix)
{
    return lower.size() > prefix.size() && lower.compare(0, prefix.size(), prefix) == 0 &&
           (lower[prefix.size()] == '\\' || lower[prefix.size()] == '/');
}

} // namespace

ArmaPathResolver::ArmaPathResolver(const IFileSystem& fs) : fs_(fs) {}

void ArmaPathResolver::AddMount(std::string_view prefix, const fs::path& folder)
{
    std::unique_lock lock(mutex_);
    const std::string p = MountKey(prefix);
    for (auto& [k, v] : mounts_)
        if (k == p) { v = folder; return; }
    mounts_.emplace_back(p, folder);
}

void ArmaPathResolver::AddFallbackMount(std::string_view prefix, const fs::path& folder)
{
    std::unique_lock lock(mutex_);
    const std::string p = MountKey(prefix);
    for (auto& [k, v] : fallback_)
        if (k == p) { v = folder; return; }
    fallback_.emplace_back(p, folder);
}

std::string ArmaPathResolver::PathOf(const fs::path& file, const fs::path& projectRoot) const
{
    std::shared_lock lock(mutex_);
    std::error_code ec;
    auto under = [&](const fs::path& dir, std::string& out) {
        const fs::path rel = fs::relative(file, dir, ec);
        const std::string r = PathToUtf8(rel);
        if (ec || rel.empty() || r.starts_with("..")) return false;
        out = r;
        return true;
    };
    std::string r;
    for (const auto& [prefix, dir] : mounts_)
        if (under(dir, r)) return prefix + "\\" + r;
    for (const auto& [prefix, dir] : fallback_) // a mod not at its prefix on P:: named as its config names it
        if (under(dir, r)) return prefix + "\\" + r;
    return PathToUtf8(fs::relative(file, projectRoot, ec));
}

fs::path ArmaPathResolver::Resolve(std::string_view armaPath, const fs::path& projectRoot) const
{
    std::string p(armaPath);
    while (!p.empty() && (p.front() == '\\' || p.front() == '/')) p.erase(p.begin());
    if (p.size() > 1 && p[1] == ':') return ArmaStringPath(p);
    const std::string lower = ToLower(p);
    std::shared_lock lock(mutex_);
    for (const auto& [prefix, dir] : mounts_)
        if (UnderPrefix(lower, prefix)) return dir / ArmaStringPath(p.substr(prefix.size() + 1));
    fs::path onRoot = projectRoot / ArmaStringPath(p);
    for (const auto& [prefix, dir] : fallback_)
        if (UnderPrefix(lower, prefix))
        {
            if (fs_.Stamp(onRoot).exists) break;
            return dir / ArmaStringPath(p.substr(prefix.size() + 1));
        }
    return onRoot;
}

ArmaPathResolver& DefaultArmaPaths()
{
    static ArmaPathResolver resolver(DefaultFileSystem());
    return resolver;
}

void AddArmaMount(const std::string& prefix, const fs::path& folder) { DefaultArmaPaths().AddMount(prefix, folder); }
void AddArmaFallbackMount(const std::string& prefix, const fs::path& folder) { DefaultArmaPaths().AddFallbackMount(prefix, folder); }
std::string ArmaPathOf(const fs::path& file, const fs::path& projectRoot) { return DefaultArmaPaths().PathOf(file, projectRoot); }

namespace {
std::mutex g_prefixMutex;
std::vector<std::pair<fs::path, std::string>> g_prefixOverrides;

std::string TrimPrefix(std::string p)
{
    while (!p.empty() && (p.back() == '\r' || p.back() == '\n' || p.back() == ' ' || p.back() == '\\' || p.back() == '/')) p.pop_back();
    while (!p.empty() && (p.front() == '\\' || p.front() == '/' || p.front() == ' ')) p.erase(p.begin());
    return p;
}

// every string value of the config's own classes (not the game's it is overlaid on)
void CollectStrings(const CfgClass& c, std::vector<std::string>& out, size_t limit)
{
    std::vector<const CfgValue*> stack;
    for (const auto& e : c.entries)
    {
        stack.push_back(&e.value);
        while (!stack.empty() && out.size() < limit)
        {
            const CfgValue* v = stack.back();
            stack.pop_back();
            if (v->type == CfgValue::Type::String) { if (!v->str.empty()) out.push_back(v->str); }
            else if (v->type == CfgValue::Type::Array)
                for (const auto& i : v->items) stack.push_back(&i);
        }
    }
    for (const auto& k : c.classes)
        if (out.size() < limit && !k->external) CollectStrings(*k, out, limit);
}
} // namespace

void SetModPrefixOverrides(std::vector<std::pair<fs::path, std::string>> folderToPrefix)
{
    std::lock_guard lock(g_prefixMutex);
    g_prefixOverrides = std::move(folderToPrefix);
}

std::string InferModPrefix(const fs::path& modFolder, const ArmaConfig* parsed, std::string* why)
{
    auto say = [&](const std::string& w) { if (why) *why = w; };
    std::error_code ec;
    if (std::ifstream pf(modFolder / "$PBOPREFIX$"); pf)
    {
        std::string line;
        std::getline(pf, line);
        if (std::string p = TrimPrefix(line); !p.empty()) { say("$PBOPREFIX$"); return p; }
    }
    {
        std::lock_guard lock(g_prefixMutex);
        for (const auto& [folder, prefix] : g_prefixOverrides)
            if (fs::equivalent(folder, modFolder, ec) && !TrimPrefix(prefix).empty()) { say("set in the settings"); return TrimPrefix(prefix); }
    }
    if (parsed)
    {
        // the config's own file paths: "\a\b\rest" whose longest tail "rest" exists under the folder votes for "a\b"
        std::vector<std::string> strings;
        CollectStrings(parsed->Root(), strings, 4000);
        std::map<std::string, std::pair<int, std::string>> votes; // lower prefix -> (votes, as written)
        int checked = 0;
        for (const auto& s : strings)
        {
            if (s.find('\\') == std::string::npos || s.find('.') == std::string::npos || s.size() > 260) continue;
            std::vector<std::string> parts;
            size_t a = 0;
            const std::string t = TrimPrefix(s);
            for (size_t i = 0; i <= t.size(); ++i)
                if (i == t.size() || t[i] == '\\' || t[i] == '/')
                {
                    if (i > a) parts.push_back(t.substr(a, i - a));
                    a = i + 1;
                }
            if (parts.size() < 2 || ++checked > 600) continue;
            for (size_t i = 1; i < parts.size(); ++i)
            {
                fs::path tail;
                for (size_t k = i; k < parts.size(); ++k) tail /= fs::path(Utf8ToWide(parts[k]));
                if (!fs::exists(modFolder / tail, ec)) continue;
                std::string head;
                for (size_t k = 0; k < i; ++k) head += (k ? "\\" : "") + parts[k];
                auto& v = votes[ToLower(head)];
                ++v.first;
                v.second = head;
                break;
            }
        }
        const std::pair<int, std::string>* best = nullptr;
        for (const auto& [k, v] : votes)
            if (!best || v.first > best->first) best = &v;
        if (best)
        {
            say(std::format("{} of the config's paths agree", best->first));
            return best->second;
        }
    }
    say("the folder name");
    return PathToUtf8(modFolder.filename());
}

std::string MountInferredModPrefix(const fs::path& modFolder, const ArmaConfig* parsed, const fs::path& projectRoot)
{
    std::string why;
    const std::string prefix = InferModPrefix(modFolder, parsed, &why);
    if (prefix.empty()) return {};
    std::error_code ec;
    const fs::path root = projectRoot.empty() ? fs::path("P:\\") : projectRoot;
    const fs::path onP = root / fs::path(Utf8ToWide(prefix));
    if (!fs::exists(onP, ec) && !fs::equivalent(modFolder, ResolveArmaPath(prefix, projectRoot), ec))
    {
        AddArmaFallbackMount(prefix, modFolder);
        Log::Get().Info("Mod prefix \\{} ({}) -> {} (not at {}): the mod's own paths that are not on P: resolve there", prefix, why,
                        PathToUtf8(modFolder), PathToUtf8(onP));
    }
    return prefix;
}
fs::path ResolveArmaPath(std::string_view armaPath, const fs::path& projectRoot) { return DefaultArmaPaths().Resolve(armaPath, projectRoot); }

namespace {

struct ParseError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct OutLine
{
    std::string text;
    int file, line;
};

bool IsIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool IsIdentChar(char c) { return IsIdentStart(c) || (c >= '0' && c <= '9'); }

std::string Trim(std::string_view s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

// ============================================================================================ preprocessor

class Preprocessor
{
public:
    Preprocessor(ArmaConfig& cfg, fs::path root) : cfg_(cfg), root_(std::move(root)) {}

    void Run(const fs::path& path, std::vector<OutLine>& out, int depth, int fromFile, int fromLine)
    {
        if (depth > 32)
        {
            cfg_.Error(fromFile, fromLine, "#include nested too deeply (cycle?)");
            return;
        }
        std::ifstream f(path, std::ios::binary);
        if (!f)
        {
            cfg_.Error(fromFile, fromLine, std::format("cannot open '{}'", PathToUtf8(path)));
            return;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        std::string raw = ss.str();
        if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0) raw.erase(0, 3); // UTF-8 BOM (e.g. BI's rtd.hpp)
        std::string text = StripComments(raw);
        int fileId = cfg_.AddFile(path);

        std::vector<std::string> lines;
        {
            std::string cur;
            for (char c : text)
            {
                if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
                else if (c != '\r') cur += c;
            }
            lines.push_back(std::move(cur));
        }

        for (size_t i = 0; i < lines.size(); ++i)
        {
            int lineNo = int(i) + 1;
            std::string line = lines[i];
            std::string t = Trim(line);
            if (!t.empty() && t[0] == '#')
            {
                // directive; join "\" continuations
                while (!t.empty() && t.back() == '\\' && i + 1 < lines.size())
                {
                    t.pop_back();
                    t += ' ' + Trim(lines[++i]);
                }
                Directive(t.substr(1), path, fileId, lineNo, out, depth);
                continue;
            }
            if (!Active()) { out.push_back({ "", fileId, lineNo }); continue; }
            std::set<std::string> guard;
            out.push_back({ Expand(line, guard, 0, fileId, lineNo), fileId, lineNo });
        }
        if (depth == 0 && !cond_.empty()) cfg_.Error(fileId, int(lines.size()), "missing #endif");
    }

private:
    struct Macro
    {
        bool function = false;
        std::vector<std::string> params;
        std::string body;
    };

    bool Active() const
    {
        for (const auto& c : cond_) if (!c.active) return false;
        return true;
    }

    static std::string StripComments(const std::string& s)
    {
        std::string r;
        r.reserve(s.size());
        bool inStr = false;
        for (size_t i = 0; i < s.size(); ++i)
        {
            char c = s[i];
            if (inStr)
            {
                r += c;
                if (c == '"') inStr = false;
                else if (c == '\n') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; r += c; continue; }
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '/')
            {
                while (i < s.size() && s[i] != '\n') ++i;
                if (i < s.size()) r += '\n';
                continue;
            }
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/'))
                {
                    if (s[i] == '\n') r += '\n';
                    ++i;
                }
                ++i;
                continue;
            }
            r += c;
        }
        return r;
    }

    void Directive(const std::string& d, const fs::path& path, int file, int line, std::vector<OutLine>& out, int depth)
    {
        size_t p = 0;
        while (p < d.size() && std::isspace(static_cast<unsigned char>(d[p]))) ++p;
        size_t q = p;
        while (q < d.size() && IsIdentChar(d[q])) ++q;
        std::string word = d.substr(p, q - p);
        std::string rest = Trim(std::string_view(d).substr(q));
        out.push_back({ "", file, line }); // keep line numbering

        if (word == "ifdef" || word == "ifndef")
        {
            bool def = macros_.count(rest) > 0;
            cond_.push_back({ word == "ifdef" ? def : !def, false });
            return;
        }
        if (word == "if")
        {
            cfg_.Warn(file, line, "#if expressions are not supported; block treated as false");
            cond_.push_back({ false, false });
            return;
        }
        if (word == "else")
        {
            if (cond_.empty()) cfg_.Error(file, line, "#else without #ifdef");
            else if (cond_.back().hadElse) cfg_.Error(file, line, "second #else");
            else { cond_.back().active = !cond_.back().active; cond_.back().hadElse = true; }
            return;
        }
        if (word == "endif")
        {
            if (cond_.empty()) cfg_.Error(file, line, "#endif without #ifdef");
            else cond_.pop_back();
            return;
        }
        if (!Active()) return;

        if (word == "include")
        {
            if (rest.size() < 2 || !((rest.front() == '"' && rest.back() == '"') || (rest.front() == '<' && rest.back() == '>')))
            {
                cfg_.Error(file, line, "#include expects \"file\"");
                return;
            }
            std::string name = rest.substr(1, rest.size() - 2);
            fs::path u8name = ArmaStringPath(name);
            fs::path target;
            if (!name.empty() && (name[0] == '\\' || name[0] == '/')) target = ResolveArmaPath(name, root_);
            else
            {
                target = path.parent_path() / u8name;
                if (!fs::exists(target)) target = ResolveArmaPath(name, root_);
            }
            Run(target, out, depth + 1, file, line);
            return;
        }
        if (word == "define")
        {
            size_t a = 0;
            while (a < rest.size() && IsIdentChar(rest[a])) ++a;
            std::string name = rest.substr(0, a);
            if (name.empty()) { cfg_.Error(file, line, "#define without a name"); return; }
            Macro m;
            if (a < rest.size() && rest[a] == '(')
            {
                m.function = true;
                size_t close = rest.find(')', a);
                if (close == std::string::npos) { cfg_.Error(file, line, "#define: missing ')'"); return; }
                std::stringstream ps(rest.substr(a + 1, close - a - 1));
                std::string param;
                while (std::getline(ps, param, ','))
                    if (auto t = Trim(param); !t.empty()) m.params.push_back(t);
                m.body = Trim(std::string_view(rest).substr(close + 1));
            }
            else
                m.body = Trim(std::string_view(rest).substr(a));
            macros_[name] = std::move(m);
            return;
        }
        if (word == "undef") { macros_.erase(rest); return; }
        cfg_.Warn(file, line, std::format("unsupported directive #{} ignored", word));
    }

    std::string Expand(const std::string& s, std::set<std::string>& active, int depth, int file, int line)
    {
        if (depth > 32) { cfg_.Error(file, line, "macro expansion too deep"); return s; }
        std::string r;
        size_t i = 0;
        while (i < s.size())
        {
            char c = s[i];
            if (c == '"')
            {
                size_t j = s.find('"', i + 1);
                j = j == std::string::npos ? s.size() : j + 1;
                r.append(s, i, j - i);
                i = j;
                continue;
            }
            if (!IsIdentStart(c) || (i > 0 && IsIdentChar(s[i - 1])))
            {
                r += c;
                ++i;
                continue;
            }
            size_t j = i;
            while (j < s.size() && IsIdentChar(s[j])) ++j;
            std::string id = s.substr(i, j - i);
            auto it = macros_.find(id);
            if (it == macros_.end() || active.count(id))
            {
                r += id;
                i = j;
                continue;
            }
            const Macro& m = it->second;
            std::string body;
            if (m.function)
            {
                size_t k = j;
                while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
                if (k >= s.size() || s[k] != '(') { r += id; i = j; continue; }
                std::vector<std::string> args;
                std::string cur;
                int nest = 0;
                bool inStr = false;
                size_t e = k + 1;
                for (; e < s.size(); ++e)
                {
                    char ch = s[e];
                    if (inStr) { cur += ch; if (ch == '"') inStr = false; continue; }
                    if (ch == '"') { inStr = true; cur += ch; continue; }
                    if (ch == '(' || ch == '{' || ch == '[') ++nest;
                    if (ch == ')' && nest == 0) break;
                    if (ch == ')' || ch == '}' || ch == ']') --nest;
                    if (ch == ',' && nest == 0) { args.push_back(Trim(cur)); cur.clear(); continue; }
                    cur += ch;
                }
                if (e >= s.size()) { cfg_.Error(file, line, std::format("macro {}: missing ')'", id)); r += id; i = j; continue; }
                args.push_back(Trim(cur));
                if (m.params.empty() && args.size() == 1 && args[0].empty()) args.clear();
                if (args.size() != m.params.size())
                    cfg_.Error(file, line, std::format("macro {} expects {} argument(s), got {}", id, m.params.size(), args.size()));
                body = Substitute(m, args);
                i = e + 1;
            }
            else
            {
                body = m.body;
                i = j;
            }
            active.insert(id);
            r += Expand(body, active, depth + 1, file, line);
            active.erase(id);
        }
        return r;
    }

    static std::string Substitute(const Macro& m, const std::vector<std::string>& args)
    {
        auto argFor = [&](const std::string& id) -> const std::string* {
            for (size_t p = 0; p < m.params.size(); ++p)
                if (m.params[p] == id) return p < args.size() ? &args[p] : nullptr;
            return nullptr;
        };
        std::string r;
        const std::string& b = m.body;
        size_t i = 0;
        while (i < b.size())
        {
            if (b[i] == '#' && i + 1 < b.size() && b[i + 1] == '#')
            {
                while (!r.empty() && (r.back() == ' ' || r.back() == '\t')) r.pop_back();
                i += 2;
                while (i < b.size() && (b[i] == ' ' || b[i] == '\t')) ++i;
                continue;
            }
            if (b[i] == '#' && i + 1 < b.size() && IsIdentStart(b[i + 1]))
            {
                size_t j = i + 1;
                while (j < b.size() && IsIdentChar(b[j])) ++j;
                if (const std::string* a = argFor(b.substr(i + 1, j - i - 1)))
                {
                    r += '"' + *a + '"';
                    i = j;
                    continue;
                }
            }
            if (IsIdentStart(b[i]) && (i == 0 || !IsIdentChar(b[i - 1])))
            {
                size_t j = i;
                while (j < b.size() && IsIdentChar(b[j])) ++j;
                std::string id = b.substr(i, j - i);
                const std::string* a = argFor(id);
                r += a ? *a : id;
                i = j;
                continue;
            }
            r += b[i++];
        }
        return r;
    }

    struct Cond { bool active; bool hadElse; };
    ArmaConfig& cfg_;
    fs::path root_;
    std::map<std::string, Macro> macros_;
    std::vector<Cond> cond_;
};

// ============================================================================================ parser

class Parser
{
public:
    Parser(ArmaConfig& cfg, const std::vector<OutLine>& lines) : cfg_(cfg), lines_(lines)
    {
        for (const auto& l : lines)
        {
            starts_.push_back(s_.size());
            s_ += l.text;
            s_ += '\n';
        }
    }

    void Parse(CfgClass& root) { Body(root, true); }

private:
    void Body(CfgClass& cls, bool top)
    {
        for (;;)
        {
            SkipWs();
            if (p_ >= s_.size())
            {
                if (!top) Error(p_, std::format("missing '}}' for class {}", cls.name));
                return;
            }
            if (s_[p_] == '}')
            {
                if (!top) return;
                Error(p_, "unexpected '}'");
                ++p_;
                continue;
            }
            if (s_[p_] == ';') { ++p_; continue; }
            size_t stmt = p_;
            try
            {
                Statement(cls);
            }
            catch (const ParseError& e)
            {
                Error(stmt, e.what());
                if (cfg_.Errors() > 50) { p_ = s_.size(); return; }
                // recover: skip to the next ';' (not inside a string)
                bool inStr = false;
                while (p_ < s_.size() && (inStr || s_[p_] != ';'))
                {
                    if (s_[p_] == '"') inStr = !inStr;
                    if (!inStr && s_[p_] == '}') break;
                    ++p_;
                }
                if (p_ < s_.size() && s_[p_] == ';') ++p_;
            }
        }
    }

    void Statement(CfgClass& cls)
    {
        size_t at = p_;
        std::string word = Ident("name or 'class'");
        if (word == "class")
        {
            std::string name = Ident("class name");
            std::string base;
            SkipWs();
            if (Accept(':')) base = Ident("base class name");
            CfgClass* k = nullptr;
            for (auto& c : cls.classes)
                if (IEquals(c->name, name)) { k = c.get(); break; }
            SkipWs();
            if (Accept('{'))
            {
                if (!k)
                {
                    cls.classes.push_back(std::make_unique<CfgClass>());
                    k = cls.classes.back().get();
                }
                k->name = name;
                k->external = false;
                if (!base.empty()) k->baseName = base;
                k->parent = &cls;
                Loc(at, k->file, k->line);
                Body(*k, false);
                Expect('}');
                SkipWs();
                if (!Accept(';')) Warn(p_, std::format("missing ';' after class {}", name));
            }
            else
            {
                Expect(';');
                if (!base.empty()) throw ParseError(std::format("class {} : {} has no body", name, base));
                if (!k)
                {
                    cls.classes.push_back(std::make_unique<CfgClass>());
                    k = cls.classes.back().get();
                    k->name = name;
                    k->external = true;
                    k->parent = &cls;
                    Loc(at, k->file, k->line);
                }
            }
            return;
        }
        if (word == "enum") // enum { A, B = 2, ... }; - constants for expressions; BI headers #define them as well
        {
            SkipWs();
            Expect('{');
            while (p_ < s_.size() && s_[p_] != '}') ++p_;
            Expect('}');
            SkipWs();
            Accept(';');
            return;
        }
        if (word == "delete")
        {
            std::string name = Ident("class name");
            Expect(';');
            std::erase_if(cls.classes, [&](const auto& c) { return IEquals(c->name, name); });
            return;
        }

        CfgEntry e;
        e.name = word;
        Loc(at, e.file, e.line);
        SkipWs();
        bool append = false;
        if (Accept('['))
        {
            SkipWs();
            Expect(']');
            e.isArray = true;
            SkipWs();
            if (Accept('+')) append = true;
            Expect('=');
            e.value = Array();
        }
        else
        {
            Expect('=');
            e.value = Scalar(";");
        }
        SkipWs();
        Expect(';');

        for (auto& old : cls.entries)
            if (IEquals(old.name, e.name))
            {
                if (append && old.isArray)
                    for (auto& v : e.value.items) old.value.items.push_back(std::move(v));
                else
                    old = std::move(e);
                return;
            }
        if (append) // += on an inherited array: copy the inherited values first
            if (const CfgEntry* inh = cls.base ? cls.base->Find(e.name) : nullptr)
            {
                CfgEntry merged = *inh;
                merged.file = e.file;
                merged.line = e.line;
                for (auto& v : e.value.items) merged.value.items.push_back(std::move(v));
                e = std::move(merged);
            }
        cls.entries.push_back(std::move(e));
    }

    CfgValue Array()
    {
        SkipWs();
        Expect('{');
        CfgValue v;
        v.type = CfgValue::Type::Array;
        for (;;)
        {
            SkipWs();
            if (Accept('}')) return v;
            if (p_ < s_.size() && s_[p_] == '{')
                v.items.push_back(Array());
            else
                v.items.push_back(Scalar(",}"));
            SkipWs();
            if (Accept(',')) continue;
            if (Accept('}')) return v;
            throw ParseError(std::format("expected ',' or '}}' in array, got '{}'", Peek()));
        }
    }

    CfgValue Scalar(std::string_view terminators)
    {
        SkipWs();
        CfgValue v;
        if (p_ < s_.size() && s_[p_] == '"')
        {
            ++p_;
            for (;;)
            {
                if (p_ >= s_.size() || s_[p_] == '\n') throw ParseError("unterminated string");
                if (s_[p_] == '"')
                {
                    if (p_ + 1 < s_.size() && s_[p_ + 1] == '"') { v.str += '"'; p_ += 2; continue; }
                    ++p_;
                    break;
                }
                v.str += s_[p_++];
            }
            return v;
        }
        size_t a = p_;
        while (p_ < s_.size() && terminators.find(s_[p_]) == std::string_view::npos && s_[p_] != '\n') ++p_;
        v.str = Trim(std::string_view(s_).substr(a, p_ - a));
        if (v.str.empty()) throw ParseError("missing value");
        double d;
        auto r = std::from_chars(v.str.data(), v.str.data() + v.str.size(), d);
        if (r.ec == std::errc() && r.ptr == v.str.data() + v.str.size())
        {
            v.type = CfgValue::Type::Number;
            v.num = d;
        }
        return v;
    }

    std::string Ident(const char* what)
    {
        SkipWs();
        size_t a = p_;
        while (p_ < s_.size() && IsIdentChar(s_[p_])) ++p_;
        if (a == p_) throw ParseError(std::format("expected {}, got '{}'", what, Peek()));
        return s_.substr(a, p_ - a);
    }

    void SkipWs()
    {
        while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_;
    }
    bool Accept(char c)
    {
        if (p_ < s_.size() && s_[p_] == c) { ++p_; return true; }
        return false;
    }
    void Expect(char c)
    {
        SkipWs();
        if (!Accept(c)) throw ParseError(std::format("expected '{}', got '{}'", c, Peek()));
    }
    std::string Peek() const
    {
        if (p_ >= s_.size()) return "end of file";
        std::string t = s_.substr(p_, 16);
        if (auto nl = t.find('\n'); nl != std::string::npos) t.resize(nl);
        return t.empty() ? "end of line" : t;
    }
    void Loc(size_t pos, int& file, int& line) const
    {
        size_t idx = std::upper_bound(starts_.begin(), starts_.end(), pos) - starts_.begin();
        idx = idx ? idx - 1 : 0;
        if (idx < lines_.size()) { file = lines_[idx].file; line = lines_[idx].line; }
    }
    void Error(size_t pos, const std::string& msg)
    {
        int f = 0, l = 0;
        Loc(pos, f, l);
        cfg_.Error(f, l, msg);
    }
    void Warn(size_t pos, const std::string& msg)
    {
        int f = 0, l = 0;
        Loc(pos, f, l);
        cfg_.Warn(f, l, msg);
    }

    ArmaConfig& cfg_;
    const std::vector<OutLine>& lines_;
    std::string s_;
    std::vector<size_t> starts_;
    size_t p_ = 0;
};

} // namespace

// Resolve "class A : B" (B looked up in the enclosing scopes, own and inherited) and external declarations.
// counterpart: the class at the same path in the fallback (base) configs, if any.
void ResolveBasesFor(ArmaConfig& cfg, CfgClass& cls, const CfgClass* counterpart)
{
    for (auto& kp : cls.classes)
    {
        CfgClass& k = *kp;
        const CfgClass* cp = counterpart ? counterpart->FindClass(k.name, false) : nullptr;
        if (cp && cp->external && cp->base) cp = cp->base;
        k.overlay = cp && !k.external ? cp : nullptr; // the game's own class of the same name: searched after the bases
        if (k.external)
        {
            k.base = nullptr;
            if (cls.base)
                if (const CfgClass* t = cls.base->FindClass(k.name)) k.base = const_cast<CfgClass*>(t);
            if (!k.base && cls.overlay)
                if (const CfgClass* t = cls.overlay->FindClass(k.name)) k.base = const_cast<CfgClass*>(t);
            if (!k.base && cp) k.base = const_cast<CfgClass*>(cp);
        }
        else if (cp && k.baseName.empty())
        {
            // the class reopens a class of the base configs without naming a base: the game class carries on
            k.base = nullptr;
        }
        else if (!k.baseName.empty())
        {
            const CfgClass* found = nullptr;
            for (const CfgClass* scope = &cls; scope && !found; scope = scope->parent)
            {
                for (const auto& c : scope->classes)
                    if (c.get() != &k && IEquals(c->name, k.baseName))
                    {
                        found = c->external ? c->base : c.get();
                        break;
                    }
                if (!found && scope->base) found = scope->base->FindClass(k.baseName);
                if (!found && scope->overlay) found = scope->overlay->FindClass(k.baseName);
            }
            if (!found)
            {
                if (cfg.reportMissingBases_) cfg.Error(k.file, k.line, std::format("class {}: base class {} not found", k.name, k.baseName));
                else ++cfg.missingBases_;
            }
            else if (found == &k) cfg.Error(k.file, k.line, std::format("class {} inherits from itself", k.name));
            else k.base = const_cast<CfgClass*>(found);
        }
        ResolveBasesFor(cfg, k, cp);
    }
}

bool ArmaConfig::Load(const fs::path& path, const fs::path& projectRoot, bool reportMissingBases)
{
    root_ = CfgClass{};
    files_.clear();
    errors_ = 0;
    missingBases_ = 0;
    reportMissingBases_ = reportMissingBases;
    auto t0 = Perf::Clock::now();
    auto note = [&](const char* what) {
        if (!fallback_) return; // only the addon config over the game's is interesting here
        auto t1 = Perf::Clock::now();
        Perf::Get().Note(what, std::chrono::duration<double, std::milli>(t1 - t0).count());
        t0 = t1;
    };
    if (IsRapified(path))
    {
        if (!LoadRap(*this, path, root_)) return false;
    }
    else
    {
        std::vector<OutLine> lines;
        Preprocessor pp(*this, projectRoot);
        pp.Run(path, lines, 0, -1, 0);
        if (files_.empty()) return false;
        Parser parser(*this, lines);
        parser.Parse(root_);
    }
    note("config.cpp: preprocess + parse");
    if (fallback_) // overlay on the game configs: classes the addon reopens point at the game's (CfgClass::overlay)
    {
        root_.base = const_cast<CfgClass*>(&fallback_->root_);
        root_.overlay = nullptr;
    }
    ResolveBasesFor(*this, root_, fallback_ ? &fallback_->root_ : nullptr);
    note("config.cpp: resolve bases");
    return true;
}

std::unique_ptr<CfgClass> CloneClass(const CfgClass& src, CfgClass* parent)
{
    auto c = std::make_unique<CfgClass>();
    c->name = src.name;
    c->baseName = src.baseName;
    c->external = src.external;
    c->file = src.file;
    c->line = src.line;
    c->parent = parent;
    c->entries = src.entries;
    c->classes.reserve(src.classes.size());
    for (const auto& k : src.classes) c->classes.push_back(CloneClass(*k, c.get()));
    return c;
}

void MergeClass(CfgClass& dst, CfgClass&& src)
{
    if (!src.external)
    {
        dst.external = false;
        if (!src.baseName.empty()) dst.baseName = src.baseName;
        dst.file = src.file;
        dst.line = src.line;
    }
    for (auto& e : src.entries)
    {
        auto it = std::find_if(dst.entries.begin(), dst.entries.end(), [&](const CfgEntry& o) { return IEquals(o.name, e.name); });
        if (it != dst.entries.end()) *it = std::move(e);
        else dst.entries.push_back(std::move(e));
    }
    for (auto& k : src.classes)
    {
        auto it = std::find_if(dst.classes.begin(), dst.classes.end(), [&](const auto& o) { return IEquals(o->name, k->name); });
        if (it != dst.classes.end()) MergeClass(**it, std::move(*k));
        else
        {
            k->parent = &dst;
            dst.classes.push_back(std::move(k));
        }
    }
}

bool ArmaConfig::LoadMerged(const std::vector<fs::path>& files, const fs::path& projectRoot, const std::vector<std::string>& keepRoots)
{
    root_ = CfgClass{};
    files_.clear();
    errors_ = 0;
    missingBases_ = 0;
    reportMissingBases_ = false;
    quiet_ = true;
    fileBase_ = kGameFileBase;
    for (const auto& f : files)
    {
        if (IsRapified(f)) LoadRap(*this, f, root_);
        else
        {
            std::vector<OutLine> lines;
            Preprocessor pp(*this, projectRoot);
            pp.Run(f, lines, 0, -1, 0);
            Parser parser(*this, lines);
            parser.Parse(root_);
        }
        if (!keepRoots.empty())
            std::erase_if(root_.classes, [&](const auto& k) {
            {
                // "name" exact, "prefix*" any root class starting with prefix (lower case)
                const std::string n = ToLower(k->name);
                return std::none_of(keepRoots.begin(), keepRoots.end(), [&](const std::string& r) {
                    return r.ends_with('*') ? n.starts_with(std::string_view(r).substr(0, r.size() - 1)) : n == r;
                });
            }
            });
    }
    ResolveBasesFor(*this, root_, nullptr);
    return !files_.empty();
}

std::vector<const CfgClass*> MergedChildClasses(const CfgClass& cls)
{
    std::vector<const CfgClass*> out;
    std::set<std::string> seen;
    int guard = 0;
    (void)guard;
    for (const CfgClass* c : cls.Chain())
        for (const auto& k : c->classes)
        {
            if (!seen.insert(ToLower(k->name)).second) continue;
            if (k->external) { if (k->base) out.push_back(k->base); }
            else out.push_back(k.get());
        }
    return out;
}

// ============================================================================================ rapified configs

bool IsRapified(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    char sig[4] = {};
    return f.read(sig, 4) && std::memcmp(sig, "\0raP", 4) == 0;
}

namespace {

// Arma rapified layout (own reading, checked against P:\bin\config.bin):
//   "\0raP" u32 0 u32 8 u32 enumOffset, root class body at offset 16.
//   class body: asciiz baseName, varint entry count, entries:
//     0 class : asciiz name, u32 body offset          3 extern : asciiz name
//     1 value : u8 subtype, asciiz name, value        4 delete : asciiz name
//     2 array : asciiz name, array                    5 array with flags (+=) : u32 flags, asciiz name, array
//   value / element subtypes: 0 string, 1 float, 2 int32, 3 nested array (elements), 4 variable name, 6 int64.
//   varint: 7 bits per byte, low group first, high bit = more bytes follow.
// CWR ParamFileParse.cpp:922 is the older OFP variant (string table, version field) - a different layout.
class RapReader
{
public:
    RapReader(std::vector<char>&& data, int file) : d_(std::move(data)), file_(file) {}

    void Body(size_t at, CfgClass& cls, int depth)
    {
        if (depth > 64) throw std::runtime_error("classes nested too deep");
        size_t p = at;
        std::string base = Str(p);
        if (!base.empty()) cls.baseName = base;
        const uint32_t n = VarInt(p);
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint8_t type = U8(p);
            switch (type)
            {
            case 0:
            {
                std::string name = Str(p);
                uint32_t off = U32(p);
                if (off >= d_.size()) throw std::runtime_error("class body offset outside the file");
                CfgClass& k = Child(cls, name);
                k.external = false;
                Body(off, k, depth + 1);
                break;
            }
            case 1:
            {
                uint8_t sub = U8(p);
                CfgEntry e;
                e.name = Str(p);
                e.value = Value(sub, p);
                Put(cls, std::move(e), false);
                break;
            }
            case 2:
            case 5:
            {
                bool append = type == 5 && (U32(p) & 1) != 0;
                CfgEntry e;
                e.name = Str(p);
                e.isArray = true;
                e.value = Array(p, 0);
                Put(cls, std::move(e), append);
                break;
            }
            case 3:
            {
                std::string name = Str(p);
                bool exists = false;
                for (auto& k : cls.classes) exists |= IEquals(k->name, name);
                if (!exists) Child(cls, name).external = true;
                break;
            }
            case 4:
            {
                std::string name = Str(p);
                std::erase_if(cls.classes, [&](const auto& k) { return IEquals(k->name, name); });
                break;
            }
            default: throw std::runtime_error(std::format("unknown entry type {} at offset {}", type, p - 1));
            }
        }
    }

private:
    CfgClass& Child(CfgClass& cls, const std::string& name)
    {
        for (auto& k : cls.classes)
            if (IEquals(k->name, name)) return *k;
        cls.classes.push_back(std::make_unique<CfgClass>());
        CfgClass& k = *cls.classes.back();
        k.name = name;
        k.parent = &cls;
        k.file = file_;
        return k;
    }
    void Put(CfgClass& cls, CfgEntry&& e, bool append)
    {
        e.file = file_;
        for (auto& old : cls.entries)
            if (IEquals(old.name, e.name))
            {
                if (append && old.isArray)
                    for (auto& v : e.value.items) old.value.items.push_back(std::move(v));
                else old = std::move(e);
                return;
            }
        cls.entries.push_back(std::move(e));
    }
    CfgValue Value(uint8_t sub, size_t& p)
    {
        CfgValue v;
        switch (sub)
        {
        case 0:
        case 4: v.type = CfgValue::Type::String; v.str = Str(p); break;
        case 1:
        {
            float f;
            std::memcpy(&f, Need(p, 4), 4);
            p += 4;
            v.type = CfgValue::Type::Number;
            v.num = f;
            v.str = std::format("{:g}", f);
            break;
        }
        case 2: v.type = CfgValue::Type::Number; v.num = double(int32_t(U32(p))); v.str = std::format("{}", int64_t(v.num)); break;
        case 6:
        {
            int64_t x;
            std::memcpy(&x, Need(p, 8), 8);
            p += 8;
            v.type = CfgValue::Type::Number;
            v.num = double(x);
            v.str = std::format("{}", x);
            break;
        }
        default: throw std::runtime_error(std::format("unknown value type {} at offset {}", sub, p));
        }
        return v;
    }
    CfgValue Array(size_t& p, int depth)
    {
        if (depth > 32) throw std::runtime_error("arrays nested too deep");
        CfgValue v;
        v.type = CfgValue::Type::Array;
        const uint32_t n = VarInt(p);
        if (n > d_.size()) throw std::runtime_error("array size outside the file");
        v.items.reserve(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            uint8_t t = U8(p);
            v.items.push_back(t == 3 ? Array(p, depth + 1) : Value(t, p));
        }
        return v;
    }
    const char* Need(size_t p, size_t n) const
    {
        if (p + n > d_.size()) throw std::runtime_error("unexpected end of file");
        return d_.data() + p;
    }
    uint8_t U8(size_t& p) { uint8_t b = uint8_t(*Need(p, 1)); ++p; return b; }
    uint32_t U32(size_t& p) { uint32_t x; std::memcpy(&x, Need(p, 4), 4); p += 4; return x; }
    uint32_t VarInt(size_t& p)
    {
        uint32_t v = 0;
        for (int shift = 0; shift < 35; shift += 7)
        {
            uint8_t b = U8(p);
            v |= uint32_t(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        throw std::runtime_error("bad compressed integer");
    }
    std::string Str(size_t& p)
    {
        size_t e = p;
        while (e < d_.size() && d_[e]) ++e;
        if (e >= d_.size()) throw std::runtime_error("unterminated string");
        std::string s(d_.data() + p, e - p);
        p = e + 1;
        return s;
    }

    std::vector<char> d_;
    int file_;
};

// CfgPatches of a derapified config.cpp: patch class names and requiredAddons (cheap text scan of the file start).
void ScanPatches(const fs::path& file, std::vector<std::string>& names, std::vector<std::string>& required)
{
    std::ifstream f(file, std::ios::binary);
    std::string s(64 * 1024, '\0');
    f.read(s.data(), std::streamsize(s.size()));
    s.resize(size_t(f.gcount()));
    size_t at = s.find("class CfgPatches");
    if (at == std::string::npos) return;
    size_t p = s.find('{', at);
    if (p == std::string::npos) return;
    int depth = 0;
    bool inReq = false;
    for (size_t i = p; i < s.size(); ++i)
    {
        char c = s[i];
        if (c == '{') { ++depth; continue; }
        if (c == '}')
        {
            if (--depth == 0) break;
            inReq = false;
            continue;
        }
        if (depth == 1 && s.compare(i, 6, "class ") == 0)
        {
            size_t b = i + 6, e = b;
            while (e < s.size() && (isalnum((unsigned char)s[e]) || s[e] == '_')) ++e;
            names.push_back(ToLower(s.substr(b, e - b)));
            i = e;
        }
        else if (depth == 2 && s.compare(i, 14, "requiredAddons") == 0) inReq = true;
        else if (c == '"' && inReq && depth == 3)
        {
            size_t e = s.find('"', i + 1);
            if (e == std::string::npos) break;
            required.push_back(ToLower(s.substr(i + 1, e - i - 1)));
            i = e;
        }
    }
}

} // namespace

bool LoadRap(ArmaConfig& cfg, const fs::path& path, CfgClass& root)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const int file = cfg.AddFile(path);
    if (data.size() < 16 || std::memcmp(data.data(), "\0raP", 4) != 0)
    {
        cfg.Error(file, 0, "not a rapified config");
        return false;
    }
    try
    {
        RapReader r(std::move(data), file);
        r.Body(16, root, 0);
    }
    catch (const std::exception& e)
    {
        cfg.Error(file, 0, std::string("rapified config: ") + e.what());
        return false;
    }
    return true;
}

std::vector<fs::path> GameConfigFiles(const fs::path& projectRoot)
{
    std::vector<fs::path> out;
    std::error_code ec;
    const fs::path a3 = projectRoot / "a3";
    if (!fs::is_directory(a3, ec)) return out;
    if (fs::path core = projectRoot / "bin" / "config.bin"; fs::exists(core, ec)) out.push_back(core);

    struct Addon
    {
        fs::path file;
        std::vector<std::string> names, required;
    };
    std::vector<Addon> addons;
    for (auto it = fs::recursive_directory_iterator(a3, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (!it->is_regular_file(ec) || !IEquals(PathToUtf8(it->path().filename()), "config.cpp")) continue;
        Addon a;
        a.file = it->path();
        ScanPatches(a.file, a.names, a.required);
        addons.push_back(std::move(a));
    }
    std::sort(addons.begin(), addons.end(), [](const Addon& a, const Addon& b) { return a.file < b.file; });
    // requiredAddons first (unknown names ignored, cycles broken in path order)
    std::map<std::string, size_t> byName;
    for (size_t i = 0; i < addons.size(); ++i)
        for (const auto& n : addons[i].names) byName.emplace(n, i);
    std::vector<int> state(addons.size(), 0);
    std::function<void(size_t)> visit = [&](size_t i) {
        if (state[i]) return;
        state[i] = 1;
        for (const auto& r : addons[i].required)
            if (auto it = byName.find(r); it != byName.end() && state[it->second] == 0) visit(it->second);
        state[i] = 2;
        out.push_back(addons[i].file);
    };
    for (size_t i = 0; i < addons.size(); ++i) visit(i);
    return out;
}

// ============================================================================================ binary cache

namespace {

constexpr uint32_t kCacheMagic = 0x43435641; // "AVCC"
constexpr uint32_t kCacheVersion = 6; // 2: gesture / move configs and CfgWorlds kept, 3: CfgWeapons, 4: weapon slot root classes, 5: CfgMagazines, 6: CfgFaces / CfgHeads / CfgGlasses (Unit)

struct CacheWriter
{
    std::string out;
    void U32(uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }
    void F64(double v) { out.append(reinterpret_cast<const char*>(&v), 8); }
    void Str(const std::string& s) { U32(uint32_t(s.size())); out += s; }
    void Value(const CfgValue& v)
    {
        out += char(v.type);
        Str(v.str);
        F64(v.num);
        U32(uint32_t(v.items.size()));
        for (const auto& i : v.items) Value(i);
    }
    void Class(const CfgClass& c)
    {
        Str(c.name);
        Str(c.baseName);
        out += char(c.external);
        U32(uint32_t(c.file));
        U32(uint32_t(c.line));
        U32(uint32_t(c.entries.size()));
        for (const auto& e : c.entries)
        {
            Str(e.name);
            out += char(e.isArray);
            U32(uint32_t(e.file));
            U32(uint32_t(e.line));
            Value(e.value);
        }
        U32(uint32_t(c.classes.size()));
        for (const auto& k : c.classes) Class(*k);
    }
};

struct CacheReader
{
    const std::string& d;
    size_t p = 0;
    void Need(size_t n) { if (p + n > d.size()) throw std::runtime_error("cache truncated"); }
    uint32_t U32() { Need(4); uint32_t v; std::memcpy(&v, &d[p], 4); p += 4; return v; }
    double F64() { Need(8); double v; std::memcpy(&v, &d[p], 8); p += 8; return v; }
    char Ch() { Need(1); return d[p++]; }
    std::string Str() { uint32_t n = U32(); Need(n); std::string s = d.substr(p, n); p += n; return s; }
    void Value(CfgValue& v)
    {
        v.type = CfgValue::Type(Ch());
        v.str = Str();
        v.num = F64();
        v.items.resize(U32());
        for (auto& i : v.items) Value(i);
    }
    void Class(CfgClass& c, CfgClass* parent)
    {
        c.parent = parent;
        c.name = Str();
        c.baseName = Str();
        c.external = Ch() != 0;
        c.file = int(U32());
        c.line = int(U32());
        c.entries.resize(U32());
        for (auto& e : c.entries)
        {
            e.name = Str();
            e.isArray = Ch() != 0;
            e.file = int(U32());
            e.line = int(U32());
            Value(e.value);
        }
        uint32_t n = U32();
        c.classes.reserve(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            c.classes.push_back(std::make_unique<CfgClass>());
            Class(*c.classes.back(), &c);
        }
    }
};

} // namespace

bool ArmaConfig::SaveCache(const fs::path& file) const
{
    const uint64_t key = ConfigFilesStamp(files_);
    CacheWriter w;
    w.U32(kCacheMagic);
    w.U32(kCacheVersion);
    w.out.append(reinterpret_cast<const char*>(&key), 8);
    w.U32(uint32_t(fileBase_));
    w.U32(uint32_t(files_.size()));
    for (const auto& f : files_) w.Str(PathToUtf8(f));
    w.U32(uint32_t(errors_));
    w.U32(uint32_t(missingBases_));
    w.Class(root_);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream o(file, std::ios::binary | std::ios::trunc);
    o.write(w.out.data(), std::streamsize(w.out.size()));
    return bool(o);
}

bool ArmaConfig::LoadCache(const fs::path& file)
{
    std::ifstream f(file, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::string d(size_t(f.tellg()), '\0');
    f.seekg(0);
    f.read(d.data(), std::streamsize(d.size()));
    try
    {
        CacheReader r{ d };
        if (r.U32() != kCacheMagic || r.U32() != kCacheVersion) return false;
        uint64_t k;
        r.Need(8);
        std::memcpy(&k, &d[r.p], 8);
        r.p += 8;
        root_ = CfgClass{};
        files_.clear();
        fileBase_ = int(r.U32());
        for (uint32_t i = 0, n = r.U32(); i < n; ++i)
        {
            std::string s = r.Str();
            files_.push_back(ArmaStringPath(s));
        }
        if (ConfigFilesStamp(files_) != k) { files_.clear(); return false; } // a source changed
        errors_ = int(r.U32());
        missingBases_ = int(r.U32());
        auto t0 = Perf::Clock::now();
        r.Class(root_, nullptr);
        auto t1 = Perf::Clock::now();
        reportMissingBases_ = false;
        quiet_ = true;
        ResolveBasesFor(*this, root_, nullptr);
        Log::Get().Info("Game configs cache: read {:.0f} MB, tree {:.2f} s, bases {:.2f} s", d.size() / 1048576.0,
                        std::chrono::duration<double>(t1 - t0).count(), std::chrono::duration<double>(Perf::Clock::now() - t1).count());
        return true;
    }
    catch (const std::exception&)
    {
        root_ = CfgClass{};
        files_.clear();
        return false;
    }
}

uint64_t ConfigFilesStamp(const std::vector<fs::path>& files)
{
    uint64_t h = 1469598103934665603ull ^ kCacheVersion;
    auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    std::error_code ec;
    std::set<std::wstring> dirs;
    for (const auto& f : files)
    {
        const auto sz = uint64_t(fs::file_size(f, ec));
        const auto t = int64_t(fs::last_write_time(f, ec).time_since_epoch().count());
        mix(f.native().data(), f.native().size() * sizeof(wchar_t));
        mix(&sz, 8);
        mix(&t, 8);
        for (fs::path d = f.parent_path(); d.has_relative_path() && dirs.size() < 100000; d = d.parent_path())
            if (!dirs.insert(d.native()).second) break;
    }
    for (const auto& d : dirs)
    {
        const auto t = int64_t(fs::last_write_time(d, ec).time_since_epoch().count());
        mix(d.data(), d.size() * sizeof(wchar_t));
        mix(&t, 8);
    }
    return h;
}
