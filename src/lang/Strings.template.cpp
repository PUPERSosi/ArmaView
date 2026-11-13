// UI language: the viewer's one string table (bottom of this file, generated from src/lang/*.tsv by gen_strings.py -
// edit the tables, not core/Strings.cpp) and its lookups. No ImGui (the ui:: wrappers are in Lang.cpp).
#include "Strings.h"

#include <cctype>
#include <cstdint>
#include <set>
#include <string_view>
#include <unordered_map>

#include "CoreGuard.h"

namespace {

// One row per English UI string: its Russian text, and for a control label its tooltip (what it does, 1-2 sentences).
// cmd: a hotkey registry description whose key is appended to the tooltip.
struct Row
{
    const char* en;
    const char* ru;
    const char* tipEn = nullptr;
    const char* tipRu = nullptr;
    const char* cmd = nullptr;
};
extern const Row kRows[];
extern const size_t kRowCount;

UiLang g_lang = UiLang::Ru;
std::vector<std::pair<std::string, std::string>> g_commands; // description -> key
std::set<std::string> g_missingText, g_missingTip;

std::string LowerAscii(std::string s)
{
    for (auto& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

const std::unordered_map<std::string_view, const Row*>& Index()
{
    static const auto idx = [] {
        std::unordered_map<std::string_view, const Row*> m;
        for (size_t i = 0; i < kRowCount; ++i) m.emplace(kRows[i].en, &kRows[i]);
        return m;
    }();
    return idx;
}

const Row* Find(std::string_view en)
{
    const auto& m = Index();
    auto it = m.find(en);
    return it == m.end() ? nullptr : it->second;
}

bool Ascii(std::string_view s)
{
    for (char c : s)
        if (uint8_t(c) >= 0x80) return false;
    return true;
}

int Letters(std::string_view s)
{
    int n = 0;
    for (char c : s) n += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return n;
}

const char* Tr(const char* en, bool record)
{
    if (!en || !*en) return en;
    const std::string_view s(en);
    const size_t hash = s.find("##");
    const std::string_view vis = s.substr(0, hash);
    if (vis.empty()) return en;
    if (g_lang == UiLang::En) return en;
    const Row* r = Find(vis);
    if (!r || !r->ru)
    {
        if (record && Letters(vis) >= 3 && vis.size() < 300 && Ascii(vis) && g_missingText.size() < 5000) g_missingText.emplace(vis);
        return en;
    }
    if (hash == std::string_view::npos) return r->ru;
    static std::unordered_map<std::string, std::string> cache; // node-based: c_str() stays valid
    auto [it, fresh] = cache.try_emplace(std::string(s));
    if (fresh) it->second = std::string(r->ru) + std::string(s.substr(hash));
    return it->second.c_str();
}

} // namespace

void SetUiLang(UiLang lang) { g_lang = lang; }
UiLang GetUiLang() { return g_lang; }
const char* T(const char* en) { return Tr(en, true); }
const char* TrNoRecord(const char* s) { return Tr(s, false); }

void SetCommandKeys(std::vector<std::pair<std::string, std::string>> descriptionToKey) { g_commands = std::move(descriptionToKey); }

std::string LabelKey(const char* label)
{
    if (!label) return {};
    std::string_view s(label);
    const size_t hash = s.find("##");
    std::string_view vis = s.substr(0, hash);
    if (Letters(vis) == 0 && hash != std::string_view::npos)
    {
        std::string_view id = s.substr(hash);
        while (!id.empty() && id.front() == '#') id.remove_prefix(1);
        return std::string(id);
    }
    return std::string(vis);
}

std::string TipOf(const char* label)
{
    const std::string key = LabelKey(label);
    if (key.empty()) return {};
    const bool ru = g_lang == UiLang::Ru;
    auto keyOf = [&](const std::string& desc) -> std::string {
        for (const auto& [d, k] : g_commands)
            if (LowerAscii(d) == LowerAscii(desc)) return k;
        return {};
    };
    const Row* r = nullptr;
    if (std::string_view(label).find("##") != std::string_view::npos) r = Find(label); // "Reset##moves": one of several "Reset"
    if (!r || !r->tipEn) r = Find(key);
    if (r && r->tipEn)
    {
        std::string tip = ru && r->tipRu ? r->tipRu : r->tipEn;
        if (r->cmd)
            if (const std::string k = keyOf(r->cmd); !k.empty()) tip += std::string(ru ? "\nКлавиша: " : "\nKey: ") + k;
        return tip;
    }
    for (const auto& [d, k] : g_commands)
        if (LowerAscii(d) == LowerAscii(key))
            return std::string(T(d.c_str())) + (k.empty() || k == "-" ? "" : std::string(ru ? "\nКлавиша: " : "\nKey: ") + k);
    return {};
}

void NoteMissingTip(const std::string& key)
{
    if (!key.empty() && Ascii(key) && g_missingTip.size() < 5000) g_missingTip.insert(key);
}

std::vector<std::string> MissingTranslations() { return { g_missingText.begin(), g_missingText.end() }; }
std::vector<std::string> MissingTips() { return { g_missingTip.begin(), g_missingTip.end() }; }

std::pair<size_t, size_t> TableStats()
{
    size_t tips = 0;
    for (size_t i = 0; i < kRowCount; ++i) tips += kRows[i].tipEn != nullptr;
    return { kRowCount, tips };
}

// ============================================================================================ the string table
