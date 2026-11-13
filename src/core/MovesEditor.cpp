#include "MovesEditor.h"
#include "Text.h"

#include <format>
#include <set>

#include "CoreGuard.h"

namespace fs = std::filesystem;

std::string MovesEditor::PairsText(const std::vector<std::pair<std::string, float>>& v)
{
    std::string s = "{";
    for (size_t i = 0; i < v.size(); ++i) s += std::format("{}\"{}\",{:g}", i ? "," : "", v[i].first, v[i].second);
    return s + "}";
}

bool MovesEditor::IsOwn(int s) const
{
    return s >= 0 && s < int(moves_.states.size()) && moves_.states[size_t(s)].cfgFile < kGameFileBase;
}

bool MovesEditor::OwnFile(const ArmaConfig* cfg, fs::path& file) const
{
    if (!cfg || moves_.Empty()) return false;
    std::set<std::string> files;
    for (const auto& s : moves_.states)
        if (s.cfgFile < kGameFileBase) files.insert(PathToUtf8(cfg->FileOf(s.cfgFile)));
    if (files.empty()) return false;
    file = fs::path(Utf8ToWide(preferredFile.empty() || !files.count(preferredFile) ? *files.begin() : preferredFile));
    return true;
}

void MovesEditor::Open(const ArmaConfig* cfg)
{
    fs::path f;
    if (!OwnFile(cfg, f)) { msg = "This move config has no states in your files (BI only: read only)"; return; }
    if (doc_.Loaded() && doc_.File() == f && doc_.Dirty()) return; // keep unsaved edits
    std::string err;
    if (!doc_.Load(f, err)) msg = err;
}

bool MovesEditor::Link(const ArmaConfig* cfg, int from, int to, bool interpolate)
{
    if (!IsOwn(from)) { msg = moves_.states[size_t(from)].name + " is a BI state (read only) - copy it into your config first"; return false; }
    Open(cfg);
    const MoveState& s = moves_.states[size_t(from)];
    auto list = interpolate ? s.interpolateRaw : s.connectRaw;
    std::erase_if(list, [&](const auto& p) { return IEquals(p.first, moves_.states[size_t(to)].name); });
    list.push_back({ moves_.states[size_t(to)].name, 0.05f });
    std::string err;
    if (!doc_.SetEntry(StatePath(s.name), interpolate ? "interpolateTo" : "connectTo", PairsText(list), true, err)) { msg = err; return false; }
    msg = std::format("{} -> {}: {} (cost 0.05) - pending, Diff & Save", s.name, moves_.states[size_t(to)].name, interpolate ? "interpolateTo" : "connectTo");
    return true;
}

bool MovesEditor::SetLinks(int state, bool interpolate, const std::vector<std::pair<std::string, float>>& links)
{
    std::string err;
    if (doc_.SetEntry(StatePath(moves_.states[size_t(state)].name), interpolate ? "interpolateTo" : "connectTo", PairsText(links), true, err)) return true;
    msg = err;
    return false;
}

bool MovesEditor::SetValue(int state, const std::string& entry, const std::string& value, bool quoted)
{
    std::string err;
    if (doc_.SetEntry(StatePath(moves_.states[size_t(state)].name), entry, quoted ? Quote(value) : value, false, err)) return true;
    msg = err;
    return false;
}

bool MovesEditor::SetFile(int state, const std::string& armaPath) { return SetValue(state, "file", armaPath, true); }

bool MovesEditor::Rename(int state, const std::string& newName)
{
    const std::string old = moves_.states[size_t(state)].name;
    std::string err;
    if (!doc_.RenameClass(StatePath(old), newName, err)) { msg = err; return false; }
    msg = std::format("renamed {} -> {} (links and actions in {} too)", old, newName, moves_.cls);
    return true;
}

bool MovesEditor::Remove(int state)
{
    const std::string name = moves_.states[size_t(state)].name;
    std::string err;
    if (!doc_.RemoveClass(StatePath(name), err)) { msg = err; return false; }
    msg = name + " removed (pending; links to it are reported by Validation)";
    return true;
}

bool MovesEditor::CopyIntoOwn(int state, const std::string& newName)
{
    const MoveState& s = moves_.states[size_t(state)];
    std::vector<std::string> body = { std::format("file = \"{}\";", s.file), std::format("speed = {:g};", s.speed), std::format("looped = {};", s.looped ? 1 : 0),
                                      std::format("interpolationSpeed = {:g};", s.interpolationSpeed), "connectTo[] = {};", "interpolateTo[] = {};" };
    if (!s.actions.empty()) body.push_back(std::format("actions = \"{}\";", s.actions));
    const std::string name = newName.empty() ? s.name : newName;
    std::string err;
    if (!doc_.AddClass(moves_.cls + "/States", name, "", body, err)) { msg = err; return false; }
    msg = std::format("{} copied as {} (pending)", s.name, name);
    return true;
}

bool MovesEditor::AddState(const std::string& name, const std::string& base)
{
    std::string err;
    if (!doc_.AddClass(moves_.cls + "/States", name, base, { "file = \"\";", "connectTo[] = {};", "interpolateTo[] = {};" }, err)) { msg = err; return false; }
    msg = std::format("{} added at the end of {} >> States (pending)", name, moves_.cls);
    return true;
}
