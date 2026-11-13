#include "MovesEdit.h"
#include "RTM.h"
#include "Skeleton.h"
#include "Text.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>

#include <glm/gtc/quaternion.hpp>

#include "CoreGuard.h"

namespace fs = std::filesystem;

namespace {

std::string Lower(std::string s)
{
    for (auto& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

bool IdChar(char c) { return std::isalnum(uint8_t(c)) || c == '_'; }

// Class blocks of a config text (comments, strings and preprocessor lines skipped).
struct Block { std::string name; size_t header = 0, open = 0, close = 0; int parent = -1; };
std::vector<Block> ScanBlocks(const std::string& t)
{
    std::vector<Block> out;
    std::vector<int> stack;      // open class blocks
    std::vector<char> braces;    // 'c' class brace, 'v' value brace
    bool pendingClass = false;
    Block pending;
    size_t i = 0;
    auto lineStart = [&](size_t p) { size_t s = t.rfind('\n', p == 0 ? 0 : p - 1); return s == std::string::npos ? 0 : s + 1; };
    while (i < t.size())
    {
        const char c = t[i];
        if (c == '/' && i + 1 < t.size() && t[i + 1] == '/') { i = t.find('\n', i); if (i == std::string::npos) break; continue; }
        if (c == '/' && i + 1 < t.size() && t[i + 1] == '*') { i = t.find("*/", i + 2); if (i == std::string::npos) break; i += 2; continue; }
        if (c == '"') { ++i; while (i < t.size() && !(t[i] == '"' && (i + 1 >= t.size() || t[i + 1] != '"'))) i += t[i] == '"' ? 2 : 1; ++i; continue; }
        if (c == '#' && lineStart(i) == i - (i - lineStart(i)) && t.find_first_not_of(" \t", lineStart(i)) == i) { i = t.find('\n', i); if (i == std::string::npos) break; continue; }
        if (std::isalpha(uint8_t(c)) || c == '_')
        {
            size_t j = i;
            while (j < t.size() && IdChar(t[j])) ++j;
            if (t.compare(i, j - i, "class") == 0 && (i == 0 || !IdChar(t[i - 1])))
            {
                size_t k = j;
                while (k < t.size() && std::isspace(uint8_t(t[k]))) ++k;
                size_t e = k;
                while (e < t.size() && IdChar(t[e])) ++e;
                pending = {};
                pending.name = t.substr(k, e - k);
                pending.header = lineStart(i);
                pendingClass = true;
                i = e;
                continue;
            }
            i = j;
            continue;
        }
        if (c == '{')
        {
            if (pendingClass)
            {
                pending.open = i;
                pending.parent = stack.empty() ? -1 : stack.back();
                out.push_back(pending);
                stack.push_back(int(out.size()) - 1);
                braces.push_back('c');
                pendingClass = false;
            }
            else
                braces.push_back('v');
        }
        else if (c == '}')
        {
            if (!braces.empty())
            {
                if (braces.back() == 'c' && !stack.empty()) { out[size_t(stack.back())].close = i; stack.pop_back(); }
                braces.pop_back();
            }
        }
        else if (c == ';' && braces.empty() == false && braces.back() == 'c')
            pendingClass = false; // "class X;" external declaration
        else if (c == ';')
            pendingClass = false;
        ++i;
    }
    return out;
}

std::vector<std::string> SplitPath(const std::string& p)
{
    std::vector<std::string> out;
    size_t a = 0;
    for (size_t b; (b = p.find('/', a)) != std::string::npos; a = b + 1) out.push_back(p.substr(a, b - a));
    out.push_back(p.substr(a));
    return out;
}

std::vector<std::string> SplitLines(const std::string& t)
{
    std::vector<std::string> out;
    size_t a = 0;
    for (size_t b; (b = t.find('\n', a)) != std::string::npos; a = b + 1)
    {
        std::string l = t.substr(a, b - a);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
    }
    if (a < t.size()) out.push_back(t.substr(a));
    return out;
}

} // namespace

// ============================================================================================ ConfigDoc

bool ConfigDoc::Load(const fs::path& file, std::string& error)
{
    std::ifstream f(file, std::ios::binary);
    if (!f) { error = "cannot read " + file.string(); return false; }
    text_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (text_.size() >= 4 && text_.compare(0, 4, std::string("\0raP", 4)) == 0) { error = "binary (rapified) config - read only"; text_.clear(); return false; }
    saved_ = text_;
    eol_ = text_.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    file_ = file;
    undo_.clear();
    redo_.clear();
    if (onLoad) onLoad();
    return true;
}

bool ConfigDoc::FindClass(const std::string& path, Range& r) const
{
    const auto blocks = ScanBlocks(text_);
    int parent = -1;
    for (const auto& part : SplitPath(path))
    {
        int found = -1;
        for (int b = 0; b < int(blocks.size()); ++b)
            if (blocks[size_t(b)].parent == parent && Lower(blocks[size_t(b)].name) == Lower(part)) found = b; // last definition wins
        if (found < 0) return false;
        parent = found;
    }
    if (parent < 0) return false;
    r = { blocks[size_t(parent)].open, blocks[size_t(parent)].close, blocks[size_t(parent)].header };
    return r.close > r.open;
}

bool ConfigDoc::HasClass(const std::string& classPath) const
{
    Range r;
    return FindClass(classPath, r);
}

bool ConfigDoc::FindEntry(const Range& r, const std::string& name, size_t& begin, size_t& end) const
{
    // statements directly in the class body: skip nested class blocks, value braces, strings, comments
    int depth = 0;
    const std::string want = Lower(name);
    for (size_t i = r.open + 1; i < r.close;)
    {
        const char c = text_[i];
        if (c == '/' && text_[i + 1] == '/') { i = text_.find('\n', i); continue; }
        if (c == '/' && text_[i + 1] == '*') { i = text_.find("*/", i) + 2; continue; }
        if (c == '"') { ++i; while (i < r.close && text_[i] != '"') ++i; ++i; continue; }
        if (c == '{') { ++depth; ++i; continue; }
        if (c == '}') { --depth; ++i; continue; }
        if (depth == 0 && (std::isalpha(uint8_t(c)) || c == '_') && !IdChar(text_[i - 1]))
        {
            size_t j = i;
            while (IdChar(text_[j])) ++j;
            const std::string id = text_.substr(i, j - i);
            if (id == "class") { i = j; continue; }
            size_t k = j;
            while (std::isspace(uint8_t(text_[k]))) ++k;
            if (text_.compare(k, 2, "[]") == 0) { k += 2; while (std::isspace(uint8_t(text_[k]))) ++k; }
            if (text_[k] == '=' || text_.compare(k, 2, "+=") == 0)
            {
                // statement end: ';' outside strings / braces
                int d = 0;
                size_t e = k;
                for (; e < r.close; ++e)
                {
                    if (text_[e] == '"') { ++e; while (e < r.close && text_[e] != '"') ++e; continue; }
                    if (text_[e] == '{') ++d;
                    else if (text_[e] == '}') --d;
                    else if (text_[e] == ';' && d == 0) break;
                }
                if (Lower(id) == want) { begin = i; end = e + 1; return true; }
                i = e + 1;
                continue;
            }
            i = j;
            continue;
        }
        ++i;
    }
    return false;
}

std::string ConfigDoc::IndentAt(size_t pos) const
{
    size_t s = text_.rfind('\n', pos == 0 ? 0 : pos - 1);
    s = s == std::string::npos ? 0 : s + 1;
    size_t e = s;
    while (e < text_.size() && (text_[e] == ' ' || text_[e] == '\t')) ++e;
    return text_.substr(s, e - s);
}

std::string ConfigDoc::IndentUnit() const
{
    size_t tabs = 0, spaces = 0;
    for (size_t p = text_.find('\n'); p != std::string::npos; p = text_.find('\n', p + 1))
    {
        if (p + 1 < text_.size() && text_[p + 1] == '\t') ++tabs;
        else if (text_.compare(p + 1, 4, "    ") == 0) ++spaces;
    }
    return tabs >= spaces ? "\t" : "    ";
}

std::string ConfigDoc::BodyIndent(const Range& r) const
{
    for (size_t p = text_.find('\n', r.open); p != std::string::npos && p < r.close; p = text_.find('\n', p + 1))
    {
        size_t e = p + 1;
        while (e < text_.size() && (text_[e] == ' ' || text_[e] == '\t')) ++e;
        if (e >= r.close) break;
        if (text_[e] == '\r' || text_[e] == '\n' || text_[e] == '{' || text_[e] == '}') continue;
        return text_.substr(p + 1, e - p - 1);
    }
    return IndentAt(r.header) + IndentUnit();
}

bool ConfigDoc::ReindentFlat(const std::string& classPath, std::string& error)
{
    // the class itself and its previous sibling (same parent); with no sibling: the parent's body indent
    const auto blocks = ScanBlocks(text_);
    Range r;
    if (!FindClass(classPath, r)) { error = classPath + ": not in this file"; return false; }
    int self = -1;
    for (int b = 0; b < int(blocks.size()); ++b)
        if (blocks[size_t(b)].open == r.open) self = b;
    if (self < 0) { error = classPath + ": not found"; return false; }
    for (const auto& b : blocks)
        if (b.parent == self) { error = classPath + " has subclasses - not re-indented"; return false; }
    const int parent = blocks[size_t(self)].parent;
    int sib = -1;
    for (int b = 0; b < int(blocks.size()); ++b)
        if (b != self && blocks[size_t(b)].parent == parent && blocks[size_t(b)].open < r.open) sib = b;
    std::string head, brace, body, close;
    if (sib >= 0)
    {
        const Block& s = blocks[size_t(sib)];
        head = IndentAt(s.header);
        brace = IndentAt(s.open);
        close = IndentAt(s.close);
        body = BodyIndent(Range{ s.open, s.close, s.header });
    }
    else
    {
        if (parent >= 0)
        {
            const Block& p = blocks[size_t(parent)];
            head = BodyIndent(Range{ p.open, p.close, p.header });
        }
        brace = close = head;
        body = head + IndentUnit();
    }
    // rewrite the lines of the block: header, brace line, body lines, closing line (blank lines stay blank)
    size_t start = r.header == 0 ? std::string::npos : text_.rfind('\n', r.header - 1);
    start = start == std::string::npos ? 0 : start + 1;
    size_t end = text_.find('\n', r.close);
    end = end == std::string::npos ? text_.size() : end + 1;
    std::string out;
    for (size_t ls = start; ls < end;)
    {
        size_t le = text_.find('\n', ls);
        le = le == std::string::npos || le >= end ? end : le + 1;
        size_t c = ls;
        while (c < le && (text_[c] == ' ' || text_[c] == '\t')) ++c;
        const std::string content = text_.substr(c, le - c);
        const bool blank = content.empty() || content[0] == '\r' || content[0] == '\n';
        const std::string& ind = ls == start ? head : c == r.close ? close : c == r.open ? brace : body;
        out += blank ? content : ind + content;
        ls = le;
    }
    text_.replace(start, end - start, out);
    return true;
}

bool ConfigDoc::EntryText(const std::string& classPath, const std::string& name, std::string& out) const
{
    Range r;
    size_t b = 0, e = 0;
    if (!FindClass(classPath, r) || !FindEntry(r, name, b, e)) return false;
    out = text_.substr(b, e - b);
    return true;
}

bool ConfigDoc::ReplaceEntryText(const std::string& classPath, const std::string& name, const std::string& text, std::string& error)
{
    Range r;
    size_t b = 0, e = 0;
    if (!FindClass(classPath, r) || !FindEntry(r, name, b, e)) { error = classPath + " >> " + name + ": not in this file"; return false; }
    Push();
    text_.replace(b, e - b, text);
    return true;
}

bool ConfigDoc::InsertAfterClass(const std::string& classPath, const std::string& text, std::string& error)
{
    Range r;
    if (!FindClass(classPath, r)) { error = classPath + ": class not in " + file_.filename().string(); return false; }
    size_t at = text_.find('\n', r.close);
    at = at == std::string::npos ? text_.size() : at + 1;
    std::string t;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\r') continue;
        if (text[i] == '\n') t += eol_;
        else t += text[i];
    }
    if (!t.empty() && !t.ends_with(eol_)) t += eol_;
    Push();
    if (at == text_.size() && !text_.empty() && text_.back() != '\n') text_ += eol_;
    text_.insert(at, t);
    return true;
}

bool ConfigDoc::Reindent(const std::string& classPath, std::string& error)
{
    const std::string before = text_;
    if (!ReindentFlat(classPath, error)) return false;
    if (text_ == before) return true;
    std::string after = std::move(text_);
    text_ = before;
    Push();
    text_ = std::move(after);
    return true;
}

void ConfigDoc::Push()
{
    undo_.push_back(text_);
    redo_.clear();
    if (undo_.size() > 500) undo_.erase(undo_.begin());
    if (onEdit) onEdit(PathToUtf8(file_.filename()) + " edit");
}

namespace {

// Top-level items of "{a, "b,c", {d}}" (trimmed).
std::vector<std::string> ArrayItems(const std::string& v)
{
    std::vector<std::string> out;
    size_t a = v.find('{'), z = v.rfind('}');
    if (a == std::string::npos || z == std::string::npos || z <= a) return out;
    std::string cur;
    int depth = 0;
    bool str = false;
    for (size_t i = a + 1; i < z; ++i)
    {
        const char c = v[i];
        if (c == '"') str = !str;
        if (!str && c == '{') ++depth;
        if (!str && c == '}') --depth;
        if (!str && depth == 0 && c == ',') { out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    out.push_back(cur);
    for (auto& s : out)
    {
        const size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
        s = b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    }
    if (out.size() == 1 && out[0].empty()) out.clear();
    return out;
}

// New statement written the way the old one was: the text up to and after '=' as it was; a multi-line array keeps its
// line breaks and indents (opening line, item indent, closing brace), one item per line when the old one had that.
std::string StatementLike(const std::string& old, const std::string& value, bool isArray, const std::string& eol)
{
    const size_t eq = old.find('=');
    if (eq == std::string::npos) return {};
    size_t vs = eq + 1;
    while (vs < old.size() && (old[vs] == ' ' || old[vs] == '\t' || old[vs] == '\r' || old[vs] == '\n')) ++vs;
    const std::string prefix = old.substr(0, vs);
    const size_t semi = old.rfind(';');
    const std::string tail = semi == std::string::npos ? std::string(";") : old.substr(semi);
    const std::string oldValue = old.substr(vs, (semi == std::string::npos ? old.size() : semi) - vs);
    if (!isArray || oldValue.empty() || oldValue[0] != '{' || oldValue.find('\n') == std::string::npos) return prefix + value + tail;
    const size_t cb = oldValue.rfind('}');
    const size_t firstItem = oldValue.find_first_not_of(" \t\r\n", 1);
    const std::vector<std::string> oldItems = ArrayItems(oldValue);
    const std::vector<std::string> items = ArrayItems(value);
    if (oldItems.empty() || cb == std::string::npos || firstItem == std::string::npos || firstItem >= cb) return prefix + value + tail;
    const std::string afterOpen = oldValue.substr(1, firstItem - 1); // e.g. "\n\t\t\t\t\t"
    const size_t ls = afterOpen.find_last_of('\n');
    const std::string itemIndent = ls == std::string::npos ? std::string() : afterOpen.substr(ls + 1);
    const size_t lastEnd = oldValue.find_last_not_of(" \t\r\n", cb - 1);
    const std::string beforeClose = oldValue.substr(lastEnd + 1, cb - lastEnd - 1); // e.g. "\n\t\t\t\t"
    // one item per line: a single old item on its own line, or a line break between the first two
    bool perLine = oldItems.size() == 1 && afterOpen.find('\n') != std::string::npos;
    if (oldItems.size() > 1)
    {
        const size_t p1 = oldValue.find(oldItems[0], firstItem) + oldItems[0].size();
        const size_t p2 = oldValue.find(oldItems[1], p1);
        perLine = p2 != std::string::npos && oldValue.substr(p1, p2 - p1).find('\n') != std::string::npos;
    }
    std::string body;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i) body += perLine ? "," + eol + itemIndent : ", ";
        body += items[i];
    }
    return prefix + "{" + afterOpen + body + beforeClose + "}" + tail;
}

} // namespace

bool ConfigDoc::SetEntry(const std::string& classPath, const std::string& name, const std::string& value, bool isArray, std::string& error)
{
    Range r;
    if (!FindClass(classPath, r)) { error = classPath + ": class not in " + file_.filename().string(); return false; }
    const std::string stmt = name + (isArray ? "[]" : "") + " = " + value + ";";
    size_t b = 0, e = 0;
    Push();
    if (FindEntry(r, name, b, e))
    {
        // the statement in its old style (spacing around '=', multi-line array layout)
        const std::string like = StatementLike(text_.substr(b, e - b), value, isArray, eol_);
        text_.replace(b, e - b, like.empty() ? stmt : like);
    }
    else
    {
        // before the closing brace, one level deeper than the brace
        size_t ls = text_.rfind('\n', r.close);
        ls = ls == std::string::npos ? 0 : ls + 1;
        const bool braceAlone = text_.find_first_not_of(" \t", ls) == r.close;
        const std::string ind = BodyIndent(r);
        if (braceAlone) text_.insert(ls, ind + stmt + eol_);
        else text_.insert(r.close, eol_ + ind + stmt + eol_ + IndentAt(r.close));
    }
    return true;
}

bool ConfigDoc::RemoveEntry(const std::string& classPath, const std::string& name, std::string& error)
{
    Range r;
    size_t b = 0, e = 0;
    if (!FindClass(classPath, r) || !FindEntry(r, name, b, e)) { error = classPath + " >> " + name + ": not in this file"; return false; }
    Push();
    size_t ls = text_.rfind('\n', b);
    ls = ls == std::string::npos ? 0 : ls + 1;
    size_t le = text_.find('\n', e);
    le = le == std::string::npos ? text_.size() : le + 1;
    const bool wholeLines = text_.find_first_not_of(" \t", ls) == b && text_.find_first_not_of(" \t\r", e) == le - 1;
    if (wholeLines) text_.erase(ls, le - ls);
    else text_.erase(b, e - b);
    return true;
}

bool ConfigDoc::AddClass(const std::string& parentPath, const std::string& name, const std::string& base, const std::vector<std::string>& body, std::string& error)
{
    Range r;
    if (!FindClass(parentPath, r)) { error = parentPath + ": class not in " + file_.filename().string(); return false; }
    if (HasClass(parentPath + "/" + name)) { error = name + " exists already"; return false; }
    Push();
    const std::string ind = IndentAt(r.close) + "\t";
    std::string blk = ind + "class " + name + (base.empty() ? "" : ": " + base) + eol_ + ind + "{" + eol_;
    for (const auto& l : body) blk += ind + "\t" + l + eol_;
    blk += ind + "};" + eol_;
    size_t ls = text_.rfind('\n', r.close);
    ls = ls == std::string::npos ? 0 : ls + 1;
    if (text_.find_first_not_of(" \t", ls) == r.close) text_.insert(ls, blk);
    else text_.insert(r.close, eol_ + blk + IndentAt(r.close));
    std::string e2;
    ReindentFlat(parentPath + "/" + name, e2); // same tabs / spaces as the neighbours
    return true;
}

bool ConfigDoc::RemoveClass(const std::string& classPath, std::string& error)
{
    Range r;
    if (!FindClass(classPath, r)) { error = classPath + ": not in this file"; return false; }
    Push();
    size_t e = text_.find(';', r.close);
    e = e == std::string::npos ? r.close + 1 : e + 1;
    size_t le = text_.find('\n', e);
    le = le == std::string::npos ? text_.size() : le + 1;
    text_.erase(r.header, le - r.header);
    return true;
}

bool ConfigDoc::RenameClass(const std::string& classPath, const std::string& newName, std::string& error)
{
    Range r;
    if (!FindClass(classPath, r)) { error = classPath + ": not in this file"; return false; }
    const auto parts = SplitPath(classPath);
    const std::string oldName = parts.back();
    Push();
    // the header: "class Old" -> "class New"
    size_t p = text_.find("class", r.header);
    size_t k = p + 5;
    while (std::isspace(uint8_t(text_[k]))) ++k;
    size_t e = k;
    while (IdChar(text_[e])) ++e;
    text_.replace(k, e - k, newName);
    // "Old" references (links, equivalentTo, actions) in the enclosing block of the move config
    std::string parent;
    for (size_t i = 0; i + 1 < parts.size() && i < 1; ++i) parent = parts[i];
    Range pr;
    if (!parent.empty() && FindClass(parent, pr))
    {
        const std::string from = "\"" + Lower(oldName) + "\"";
        std::string lowText = Lower(text_.substr(pr.open, pr.close - pr.open));
        for (size_t q = lowText.find(from); q != std::string::npos; q = lowText.find(from, q + 1))
            text_.replace(pr.open + q + 1, oldName.size(), newName), lowText.replace(q + 1, oldName.size(), Lower(newName));
    }
    return true;
}

bool ConfigDoc::Undo()
{
    if (undo_.empty()) return false;
    redo_.push_back(text_);
    text_ = undo_.back();
    undo_.pop_back();
    return true;
}

bool ConfigDoc::Redo()
{
    if (redo_.empty()) return false;
    undo_.push_back(text_);
    text_ = redo_.back();
    redo_.pop_back();
    return true;
}

void ConfigDoc::Revert()
{
    if (text_ == saved_) return;
    Push();
    text_ = saved_;
}

std::vector<ConfigDoc::DiffLine> ConfigDoc::Diff(int context) const
{
    const auto a = SplitLines(saved_), b = SplitLines(text_);
    // trim the common head / tail, LCS on the middle
    size_t h = 0;
    while (h < a.size() && h < b.size() && a[h] == b[h]) ++h;
    size_t ta = a.size(), tb = b.size();
    while (ta > h && tb > h && a[ta - 1] == b[tb - 1]) { --ta; --tb; }
    const size_t n = ta - h, m = tb - h;
    std::vector<DiffLine> all;
    for (size_t i = 0; i < h; ++i) all.push_back({ ' ', int(i) + 1, int(i) + 1, a[i] });
    if (n * m <= 4000000)
    {
        std::vector<std::vector<uint16_t>> L(n + 1, std::vector<uint16_t>(m + 1, 0));
        for (size_t i = n; i-- > 0;)
            for (size_t j = m; j-- > 0;)
                L[i][j] = a[h + i] == b[h + j] ? uint16_t(L[i + 1][j + 1] + 1) : std::max(L[i + 1][j], L[i][j + 1]);
        size_t i = 0, j = 0;
        while (i < n || j < m)
        {
            if (i < n && j < m && a[h + i] == b[h + j]) { all.push_back({ ' ', int(h + i) + 1, int(h + j) + 1, a[h + i] }); ++i; ++j; }
            else if (j < m && (i == n || L[i][j + 1] >= L[i + 1][j])) { all.push_back({ '+', 0, int(h + j) + 1, b[h + j] }); ++j; }
            else { all.push_back({ '-', int(h + i) + 1, 0, a[h + i] }); ++i; }
        }
    }
    else
    {
        for (size_t i = 0; i < n; ++i) all.push_back({ '-', int(h + i) + 1, 0, a[h + i] });
        for (size_t j = 0; j < m; ++j) all.push_back({ '+', 0, int(h + j) + 1, b[h + j] });
    }
    for (size_t i = ta; i < a.size(); ++i) all.push_back({ ' ', int(i) + 1, int(tb + i - ta) + 1, a[i] });
    // context around changes
    std::vector<DiffLine> out;
    std::vector<uint8_t> keep(all.size(), 0);
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].kind != ' ')
            for (int d = -context; d <= context; ++d)
                if (int(i) + d >= 0 && size_t(int(i) + d) < all.size()) keep[size_t(int(i) + d)] = 1;
    for (size_t i = 0; i < all.size(); ++i)
    {
        if (keep[i]) out.push_back(all[i]);
        else if (!out.empty() && out.back().kind != '~') out.push_back({ '~', 0, 0, "..." });
    }
    return out;
}

bool ConfigDoc::Save(std::string& error)
{
    if (!Dirty()) return true;
    std::error_code ec;
    // never overwrite a backup: the first save keeps <file>.bak (the original), later ones <file>.bak2, .bak3 ...
    fs::path bak = file_;
    bak += ".bak";
    for (int k = 2; fs::exists(bak, ec) && k < 1000; ++k)
    {
        bak = file_;
        bak += ".bak" + std::to_string(k);
    }
    fs::copy_file(file_, bak, fs::copy_options::none, ec);
    if (ec) { error = "cannot write the backup " + bak.string() + ": " + ec.message(); return false; }
    std::ofstream out(file_, std::ios::binary | std::ios::trunc);
    out << text_;
    if (!out) { error = "cannot write " + file_.string(); return false; }
    saved_ = text_;
    lastBackup_ = bak;
    return true;
}

// ============================================================================================ scenarios

std::vector<MoveScenario> LoadScenarios(const fs::path& file)
{
    std::vector<MoveScenario> out;
    std::ifstream f(file);
    for (std::string line; std::getline(f, line);)
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t c = line.find('#');
        if (c != std::string::npos) line.resize(c);
        std::istringstream is(line);
        std::string cmd;
        if (!(is >> cmd)) continue;
        cmd = Lower(cmd);
        std::string rest;
        std::getline(is, rest);
        rest.erase(0, rest.find_first_not_of(" \t"));
        if (cmd == "test") { out.push_back({}); out.back().name = rest; continue; }
        if (out.empty()) out.push_back({ "unnamed" });
        if (cmd == "start") out.back().start = rest;
        else if (cmd == "expect") out.back().expect = rest;
        else if (cmd == "do")
        {
            std::istringstream r(rest);
            MoveScenario::Step st;
            r >> st.action;
            if (!(r >> st.seconds)) st.seconds = -1;
            out.back().steps.push_back(st);
        }
    }
    return out;
}

ScenarioResult RunScenario(const MovesGraph& g, const MoveScenario& s)
{
    ScenarioResult r;
    r.name = s.name;
    MovesPlayer p;
    const int start = g.Find(s.start);
    if (start < 0) { r.log = "start state " + s.start + " not found"; return r; }
    double t = 0;
    p.SwitchMove(g, start, t);
    int last = start;
    auto advance = [&](double until) {
        for (; t < until; t += 0.02)
        {
            p.Update(g, t);
            if (p.Current() != last) { last = p.Current(); r.log += std::format("  {:.2f} s -> {}\n", t, g.states[size_t(last)].name); }
        }
    };
    for (const auto& st : s.steps)
    {
        const int target = g.StateOfAction(p.Current(), st.action);
        if (target < 0) { r.log += std::format("  {}: no state for this action in {}\n", st.action, g.states[size_t(p.Current())].actions); continue; }
        float cost = 0;
        if (!p.PlayMove(g, target, t, &cost)) { r.log += std::format("  {} -> {}: no path from {}\n", st.action, g.states[size_t(target)].name, g.states[size_t(p.Current())].name); continue; }
        r.log += std::format("{:.2f} s: {} -> {} (cost {:g}, {} step(s))\n", t, st.action, g.states[size_t(target)].name, cost, p.PathShown.size() - 1);
        if (st.seconds >= 0) advance(t + st.seconds);
        else
        {
            const double limit = t + 60;
            while ((!p.Queue().empty() || p.Previous() >= 0) && t < limit) advance(t + 0.1);
        }
    }
    r.reached = g.states[size_t(p.Current())].name;
    r.ok = s.expect.empty() || Lower(r.reached) == Lower(s.expect);
    r.log += std::format("end: {}{}\n", r.reached, s.expect.empty() ? "" : r.ok ? " (as expected)" : " - expected " + s.expect);
    return r;
}

// ============================================================================================ traps

std::vector<MoveTrap> FindMoveTraps(const MovesGraph& g, bool poseJumps, const Skeleton* skel, bool onlyOwn, float jumpMeters, float jumpDegrees)
{
    std::vector<MoveTrap> out;
    const int n = int(g.states.size());
    // reachable from actions; idle = what Stop / Default / StopRelaxed lead to
    std::vector<uint8_t> reached(size_t(n), 0), idle(size_t(n), 0);
    std::vector<int> q;
    for (const auto& [cls, m] : g.actions)
        for (const auto& [a, s] : m)
        {
            const int k = g.Find(s);
            if (k < 0) continue;
            if (!reached[size_t(k)]) { reached[size_t(k)] = 1; q.push_back(k); }
            if (a == "stop" || a == "default" || a == "stoprelaxed") idle[size_t(k)] = 1;
        }
    for (size_t h = 0; h < q.size(); ++h)
        for (const auto& e : g.states[size_t(q[h])].edges)
            if (!reached[size_t(e.to)]) { reached[size_t(e.to)] = 1; q.push_back(e.to); }
    // can get back to an idle state (reverse BFS)
    std::vector<std::vector<int>> rev(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        for (const auto& e : g.states[size_t(i)].edges) rev[size_t(e.to)].push_back(i);
    std::vector<uint8_t> back(size_t(n), 0);
    q.clear();
    for (int i = 0; i < n; ++i)
        if (idle[size_t(i)]) { back[size_t(i)] = 1; q.push_back(i); }
    for (size_t h = 0; h < q.size(); ++h)
        for (int p : rev[size_t(q[h])])
            if (!back[size_t(p)]) { back[size_t(p)] = 1; q.push_back(p); }
    auto own = [&](int i) { return g.states[size_t(i)].cfgFile < kGameFileBase; };
    const bool anyIdle = std::any_of(idle.begin(), idle.end(), [](uint8_t x) { return x; });
    for (int i = 0; i < n; ++i)
    {
        const MoveState& s = g.states[size_t(i)];
        if (s.abstract || s.terminal || !reached[size_t(i)] || (onlyOwn && !own(i))) continue;
        if (anyIdle && !back[size_t(i)]) out.push_back({ i, std::format("{}: reached by actions, but no ConnectTo / InterpolateTo path back to an idle state (Stop / Default)", s.name), true });
    }
    // closed loops without an exit (strongly connected components with no edge out)
    std::vector<int> idx(size_t(n), -1), low(size_t(n), 0), comp(size_t(n), -1);
    std::vector<int> st;
    std::vector<uint8_t> on(size_t(n), 0);
    int counter = 0, comps = 0;
    std::function<void(int)> strong = [&](int v) {
        idx[size_t(v)] = low[size_t(v)] = counter++;
        st.push_back(v);
        on[size_t(v)] = 1;
        for (const auto& e : g.states[size_t(v)].edges)
        {
            if (idx[size_t(e.to)] < 0) { strong(e.to); low[size_t(v)] = std::min(low[size_t(v)], low[size_t(e.to)]); }
            else if (on[size_t(e.to)]) low[size_t(v)] = std::min(low[size_t(v)], idx[size_t(e.to)]);
        }
        if (low[size_t(v)] == idx[size_t(v)])
        {
            for (;;)
            {
                const int w = st.back();
                st.pop_back();
                on[size_t(w)] = 0;
                comp[size_t(w)] = comps;
                if (w == v) break;
            }
            ++comps;
        }
    };
    if (n < 20000)
        for (int i = 0; i < n; ++i)
            if (idx[size_t(i)] < 0) strong(i);
    std::vector<std::vector<int>> members(static_cast<size_t>(comps));
    for (int i = 0; i < n; ++i)
        if (comp[size_t(i)] >= 0) members[size_t(comp[size_t(i)])].push_back(i);
    for (const auto& mem : members)
    {
        if (mem.size() < 2) continue;
        bool exit = false, hasIdle = false, reach = false, mine = false;
        for (int v : mem)
        {
            hasIdle |= idle[size_t(v)] != 0;
            reach |= reached[size_t(v)] != 0;
            mine |= own(v);
            for (const auto& e : g.states[size_t(v)].edges) exit |= comp[size_t(e.to)] != comp[size_t(v)];
        }
        if (exit || hasIdle || !reach || (onlyOwn && !mine)) continue;
        std::string names;
        for (size_t k = 0; k < mem.size() && k < 5; ++k) names += (k ? ", " : "") + g.states[size_t(mem[k])].name;
        out.push_back({ mem[0], std::format("closed loop without an exit ({} states: {}{})", mem.size(), names, mem.size() > 5 ? ", ..." : ""), true });
    }
    // random walk over the actions from the idle states: actions whose state cannot be reached from where they are offered
    std::mt19937 rng(12345);
    std::set<std::pair<int, std::string>> reported;
    for (int i = 0; i < n && reported.size() < 60; ++i)
    {
        if (!idle[size_t(i)] || (onlyOwn && !own(i))) continue;
        int cur = i;
        for (int step = 0; step < 400; ++step)
        {
            const auto acts = g.ActionsOf(cur);
            if (acts.empty()) break;
            const auto& [a, target] = acts[rng() % acts.size()];
            if (target < 0) continue;
            if (g.Path(cur, target).empty())
            {
                if ((!onlyOwn || own(cur)) && reported.insert({ cur, Lower(a) }).second)
                    out.push_back({ cur, std::format("{}: action {} -> {} has no path (the game ignores the key there)", g.states[size_t(cur)].name, a, g.states[size_t(target)].name), false });
                continue;
            }
            cur = target;
        }
    }
    // pose jumps on ConnectTo (no blend): last frame of A vs first frame of B
    if (poseJumps && skel)
    {
        std::map<std::string, std::shared_ptr<RtmAnimation>> cache;
        auto load = [&](int s) -> std::shared_ptr<RtmAnimation> {
            const MoveState& m = g.states[size_t(s)];
            if (!m.fileExists) return nullptr;
            auto it = cache.find(Lower(m.path.string()));
            if (it != cache.end()) return it->second;
            auto a = std::make_shared<RtmAnimation>();
            std::string err;
            std::vector<std::string> warn;
            std::shared_ptr<RtmAnimation> res;
            if (LoadRtm(m.path, *a, err, warn)) res = a;
            cache[Lower(m.path.string())] = res;
            return res;
        };
        auto pose = [&](const RtmAnimation& a, float phase, std::vector<glm::mat4>& mats) {
            AnimStack st2;
            AnimLayer l;
            l.anim = std::make_shared<RtmAnimation>(a);
            l.speed = 0;
            l.loop = false;
            l.offset = phase;
            st2.layers.push_back(l);
            st2.Bind(*skel);
            st2.Evaluate(*skel, 0.0, mats);
        };
        int checked = 0;
        for (int i = 0; i < n && checked < 400; ++i)
            for (const auto& e : g.states[size_t(i)].edges)
            {
                if (e.interpolate || e.to == i || (onlyOwn && !own(i) && !own(e.to))) continue;
                auto a = load(i), b = load(e.to);
                if (!a || !b) continue;
                ++checked;
                std::vector<glm::mat4> ma, mb;
                pose(*a, 1.0f, ma);
                pose(*b, 0.0f, mb);
                float worstD = 0, worstA = 0;
                std::string bone;
                for (size_t k = 0; k < skel->bones.size() && k < ma.size() && k < mb.size(); ++k)
                {
                    const glm::vec3 pv = skel->bones[k].pivot;
                    const float d = glm::length(glm::vec3(ma[k] * glm::vec4(pv, 1)) - glm::vec3(mb[k] * glm::vec4(pv, 1)));
                    const float ang = glm::degrees(glm::angle(BonePose::FromMatrix(ma[k]).rot * glm::inverse(BonePose::FromMatrix(mb[k]).rot)));
                    if (d > worstD) { worstD = d; bone = skel->bones[k].name; }
                    worstA = std::max(worstA, std::min(ang, 360.0f - ang));
                }
                if (worstD > jumpMeters || worstA > jumpDegrees)
                    out.push_back({ i, std::format("{} -> {} (ConnectTo, no blend): pose jumps {:.3f} m at {}, up to {:.0f} deg", g.states[size_t(i)].name,
                                                   g.states[size_t(e.to)].name, worstD, bone, worstA), false });
            }
    }
    return out;
}
